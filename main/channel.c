#include "channel.h"
#include "config.h"
#include "mqtt.h"
#include "radio.h"
#include "webserver.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "cJSON.h"

static const char *TAG = "channel";

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Derive a default mqtt_name from channel name: lowercase, non-alphanumeric → '_' */
static void derive_mqtt_name(const char *name, char *out, size_t out_size)
{
    size_t i = 0;
    for (; name[i] && i + 1 < out_size; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            out[i] = c;
        else
            out[i] = '_';
    }
    out[i] = '\0';
}

/* ── State ───────────────────────────────────────────────────────────────── */

static cosmo_channel_t   s_channels[CHANNEL_MAX_COUNT];
static int               s_channel_count = 0;
static SemaphoreHandle_t s_mutex;

typedef struct {
    uint32_t      serial;   /* 0 = slot unused */
    TimerHandle_t timer;
} feedback_timer_slot_t;

static feedback_timer_slot_t s_feedback_timers[CHANNEL_MAX_COUNT];

/* Mark channels dirty — caller must hold s_mutex */
static void channel_mark_dirty(void)
{
    cosmo_channel_t snap[CHANNEL_MAX_COUNT];
    int count = s_channel_count;
    memcpy(snap, s_channels, (size_t)count * sizeof(cosmo_channel_t));
    config_save_channels(snap, count); /* debounced write via config module */
}

/* ── JSON helpers ────────────────────────────────────────────────────────── */

static const char *channel_state_name(cosmo_channel_state_t state)
{
    switch (state) {
    case CHANNEL_STATE_CLOSING:        return "closing";
    case CHANNEL_STATE_CLOSED:         return "closed";
    case CHANNEL_STATE_OPENING:        return "opening";
    case CHANNEL_STATE_OPEN:           return "open";
    case CHANNEL_STATE_COMFORT:        return "comfort";
    case CHANNEL_STATE_PARTIALLY_OPEN: return "partially_open";
    case CHANNEL_STATE_OBSTRUCTION:    return "obstruction";
    case CHANNEL_STATE_IN_MOTION:      return "in_motion";
    default:                           return "unknown";
    }
}

cJSON *channel_to_cjson(const cosmo_channel_t *ch)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "serial",       (double)(uint32_t)ch->serial);
    cJSON_AddStringToObject(obj, "name",         ch->name);
    cJSON_AddStringToObject(obj, "proto",        ch->proto == PROTO_COSMO_2WAY ? "2way" : "1way");
    cJSON_AddNumberToObject(obj, "counter",      (double)ch->counter);
    cJSON_AddStringToObject(obj, "state",        channel_state_name(ch->state));
    cJSON_AddNumberToObject(obj, "rssi",         (double)ch->rssi);
    cJSON_AddNumberToObject(obj, "last_seen_ts", (double)ch->last_seen_ts);
    if (ch->proto == PROTO_COSMO_2WAY && ch->position >= 0)
        cJSON_AddNumberToObject(obj, "position", (double)ch->position);
    else
        cJSON_AddNullToObject(obj, "position");
    cJSON_AddBoolToObject(obj, "reports_tilt_support",  ch->reports_tilt_support);
    cJSON_AddBoolToObject(obj, "force_tilt_support",    ch->force_tilt_support);
    cJSON_AddBoolToObject(obj, "bidirectional_feedback", ch->bidirectional_feedback);
    cJSON_AddNumberToObject(obj, "feedback_timeout_s",  (double)ch->feedback_timeout_s);
    cJSON_AddBoolToObject(obj, "is_state_optimistic",   ch->is_state_optimistic);
    cJSON_AddNumberToObject(obj, "device_class",        (double)ch->device_class);
    cJSON_AddStringToObject(obj, "mqtt_name",           ch->mqtt_name);
    return obj;
}

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Caller must hold s_mutex. */
static cosmo_channel_t *channel_find_locked(uint32_t serial)
{
    for (int i = 0; i < s_channel_count; i++)
        if (s_channels[i].serial == serial)
            return &s_channels[i];
    return NULL;
}

/* State from received feedback packets only. */
static cosmo_channel_state_t rx_feedback_state(cosmo_cmd_t cmd, cosmo_channel_state_t current)
{
    switch (cmd) {
    case COSMO_BTN_FEEDBACK_TOP:         return CHANNEL_STATE_OPEN;
    case COSMO_BTN_FEEDBACK_BOTTOM:      return CHANNEL_STATE_CLOSED;
    case COSMO_BTN_FEEDBACK_COMFORT:     return CHANNEL_STATE_COMFORT;
    case COSMO_BTN_FEEDBACK_PARTIAL:     return CHANNEL_STATE_PARTIALLY_OPEN;
    case COSMO_BTN_FEEDBACK_OBSTRUCTION: return CHANNEL_STATE_OBSTRUCTION;
    case COSMO_BTN_FEEDBACK_IN_MOTION:   return CHANNEL_STATE_IN_MOTION;
    default:                             return current;
    }
}

static void broadcast_channel_update(const cosmo_channel_t *ch)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "channel_update");
    cJSON_AddItemToObject(root, "payload", channel_to_cjson(ch));
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) { webserver_ws_broadcast_json(json); free(json); }
}

/* ── Feedback timer ──────────────────────────────────────────────────────── */

static void feedback_timer_cb(TimerHandle_t t)
{
    uint32_t serial = (uint32_t)(uintptr_t)pvTimerGetTimerID(t);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cosmo_channel_t *ch = channel_find_locked(serial);
    if (!ch) { xSemaphoreGive(s_mutex); return; }

    if (ch->state == CHANNEL_STATE_OPENING  ||
        ch->state == CHANNEL_STATE_CLOSING  ||
        ch->state == CHANNEL_STATE_IN_MOTION) {
        ch->state = CHANNEL_STATE_PARTIALLY_OPEN;
    }
    ch->is_state_optimistic = false;

    cosmo_channel_t copy = *ch;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Feedback timer expired for serial=0x%08X, state→partially_open", (unsigned)serial);
    broadcast_channel_update(&copy);
    mqtt_publish_channel_update(&copy);
}

/* Find the timer slot for a given serial (any slot), or the first unused slot. */
static feedback_timer_slot_t *feedback_timer_find_slot(uint32_t serial)
{
    feedback_timer_slot_t *empty = NULL;
    for (int i = 0; i < CHANNEL_MAX_COUNT; i++) {
        if (s_feedback_timers[i].serial == serial && serial != 0)
            return &s_feedback_timers[i];
        if (!empty && s_feedback_timers[i].serial == 0)
            empty = &s_feedback_timers[i];
    }
    return empty; /* return first empty slot if serial not found */
}

/* Start/restart the feedback guard timer for a channel. */
static void feedback_timer_arm(uint32_t serial, uint16_t timeout_s)
{
    if (timeout_s == 0) return;
    feedback_timer_slot_t *slot = feedback_timer_find_slot(serial);
    if (!slot) return;

    TickType_t ticks = pdMS_TO_TICKS((uint32_t)timeout_s * 1000u);
    if (slot->timer == NULL) {
        slot->serial = serial;
        slot->timer  = xTimerCreate("fb_tmr", ticks, pdFALSE,
                                    (void *)(uintptr_t)serial,
                                    feedback_timer_cb);
        if (slot->timer)
            xTimerStart(slot->timer, 0);
    } else {
        /* xTimerChangePeriod also restarts the timer */
        xTimerChangePeriod(slot->timer, ticks, 0);
    }
}

/* Stop the timer without deleting it. */
static void feedback_timer_cancel(uint32_t serial)
{
    for (int i = 0; i < CHANNEL_MAX_COUNT; i++) {
        if (s_feedback_timers[i].serial == serial && s_feedback_timers[i].timer) {
            xTimerStop(s_feedback_timers[i].timer, 0);
            return;
        }
    }
}

/* Stop and delete the timer; clear the slot. */
static void feedback_timer_destroy(uint32_t serial)
{
    for (int i = 0; i < CHANNEL_MAX_COUNT; i++) {
        if (s_feedback_timers[i].serial == serial) {
            if (s_feedback_timers[i].timer) {
                xTimerStop(s_feedback_timers[i].timer, 0);
                xTimerDelete(s_feedback_timers[i].timer, 0);
            }
            s_feedback_timers[i].serial = 0;
            s_feedback_timers[i].timer  = NULL;
            return;
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void channel_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_channel_count = 0;
    memset(s_feedback_timers, 0, sizeof(s_feedback_timers));
    config_load_channels(s_channels, &s_channel_count);
}

esp_err_t channel_create(const char *name, cosmo_proto_t proto)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_channel_count >= CHANNEL_MAX_COUNT) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "channel list full");
        return ESP_ERR_NO_MEM;
    }

    /* Generate a serial number unique within the current channel list */
    uint32_t serial;
    do {
        serial = esp_random();
        serial &= ~0x1F; // last 5 significant bits are always zero
    } while (channel_find_locked(serial) != NULL);

    cosmo_channel_t *ch = &s_channels[s_channel_count++];
    memset(ch, 0, sizeof(*ch));
    ch->bidirectional_feedback = true;
    ch->feedback_timeout_s     = 120;
    ch->device_class           = CHANNEL_DEVICE_CLASS_SHUTTER;
    snprintf(ch->name, sizeof(ch->name), "%s", name ? name : "Unnamed");
    derive_mqtt_name(ch->name, ch->mqtt_name, sizeof(ch->mqtt_name));
    ch->proto    = proto;
    ch->serial   = serial;
    ch->counter  = 0;
    ch->state    = CHANNEL_STATE_UNKNOWN;
    ch->position = -1;

    cosmo_channel_t copy = *ch;
    channel_mark_dirty();
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Created channel \"%s\" serial=0x%08X proto=%s",
             copy.name, (unsigned)copy.serial,
             (proto == PROTO_COSMO_2WAY) ? "2way" : "1way");

    broadcast_channel_update(&copy);
    mqtt_publish_channel_created(&copy);
    return ESP_OK;
}

esp_err_t channel_delete(uint32_t serial)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < s_channel_count; i++) {
        if (s_channels[i].serial == serial) { idx = i; break; }
    }
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    cosmo_channel_t deleted = s_channels[idx]; /* save before removal */
    for (int i = idx; i < s_channel_count - 1; i++)
        s_channels[i] = s_channels[i + 1];
    s_channel_count--;
    channel_mark_dirty();
    xSemaphoreGive(s_mutex);

    feedback_timer_destroy(serial);
    mqtt_publish_channel_deleted(&deleted);

    ESP_LOGI(TAG, "Deleted channel serial=0x%08X", (unsigned)serial);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "channel_deleted");
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "serial", (double)(uint32_t)serial);
    cJSON_AddItemToObject(root, "payload", payload);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) { webserver_ws_broadcast_json(json); free(json); }
    return ESP_OK;
}

esp_err_t channel_update(uint32_t serial, const cosmo_channel_settings_t *s)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cosmo_channel_t *ch = channel_find_locked(serial);
    if (!ch) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    cosmo_channel_t old_ch = *ch; /* capture state before update */
    if (s->name[0])
        snprintf(ch->name, sizeof(ch->name), "%s", s->name);
    ch->proto                  = s->proto;
    ch->force_tilt_support     = s->force_tilt_support;
    ch->bidirectional_feedback = s->bidirectional_feedback;
    ch->feedback_timeout_s     = s->feedback_timeout_s;
    ch->device_class           = s->device_class;
    if (s->mqtt_name[0])
        snprintf(ch->mqtt_name, sizeof(ch->mqtt_name), "%s", s->mqtt_name);
    else
        derive_mqtt_name(ch->name, ch->mqtt_name, sizeof(ch->mqtt_name));

    cosmo_channel_t copy = *ch;
    channel_mark_dirty();
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Updated channel serial=0x%08X name=\"%s\" proto=%s force_tilt=%d bidir=%d timeout=%u",
             (unsigned)copy.serial, copy.name,
             (copy.proto == PROTO_COSMO_2WAY) ? "2way" : "1way",
             (int)copy.force_tilt_support,
             (int)copy.bidirectional_feedback,
             (unsigned)copy.feedback_timeout_s);
    broadcast_channel_update(&copy);

    /* Republish HA discovery if any discovery-relevant field changed */
    bool discovery_changed = (old_ch.device_class != copy.device_class)  ||
                             (old_ch.proto         != copy.proto)         ||
                             (strcmp(old_ch.mqtt_name, copy.mqtt_name) != 0) ||
                             (strcmp(old_ch.name,      copy.name)      != 0);
    if (discovery_changed)
        mqtt_republish_channel_discovery(&old_ch, &copy);
    else
        mqtt_publish_channel_update(&copy);
    return ESP_OK;
}

void channel_update_from_packet(const cosmo_packet_t *pkt)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cosmo_channel_t *ch = channel_find_locked(pkt->serial);
    if (!ch) {
        xSemaphoreGive(s_mutex);
        return; /* unknown serial — ignore */
    }

    if (!ch->bidirectional_feedback) {
        xSemaphoreGive(s_mutex);
        return; /* 1-way channel — ignore all radio feedback */
    }

    /* Any real feedback cancels the motion-guard timer */
    uint32_t ch_serial = ch->serial;

    if (pkt->cmd == COSMO_BTN_FEEDBACK_IN_MOTION &&
        (ch->state == CHANNEL_STATE_OPENING || ch->state == CHANNEL_STATE_CLOSING)) {
        /*
         * We already know the direction; IN_MOTION has less information.
         * Keep the directional state but acknowledge the device responded.
         */
        ch->is_state_optimistic = false;
    } else {
        ch->state               = rx_feedback_state(pkt->cmd, ch->state);
        ch->is_state_optimistic = false;
    }

    ch->rssi         = pkt->rssi;
    ch->last_seen_ts = (int64_t)time(NULL);

    if (pkt->cmd == COSMO_BTN_FEEDBACK_PARTIAL)
        ch->position = (int16_t)pkt->extra_payload;
    else if (pkt->cmd == COSMO_BTN_FEEDBACK_TOP || pkt->cmd == COSMO_BTN_FEEDBACK_BOTTOM)
        ch->position = -1;

    cosmo_channel_t copy = *ch;
    xSemaphoreGive(s_mutex);

    feedback_timer_cancel(ch_serial);
    broadcast_channel_update(&copy);
    mqtt_publish_channel_update(&copy);
}

esp_err_t channel_send_cmd(uint32_t serial, cosmo_cmd_t cmd, uint8_t extra_payload)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cosmo_channel_t *ch = channel_find_locked(serial);
    if (!ch) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    ch->counter++;

    if (ch->bidirectional_feedback) {
        switch (cmd) {
        case COSMO_BTN_UP:
            ch->state = CHANNEL_STATE_OPENING;
            ch->is_state_optimistic = true;
            break;
        case COSMO_BTN_DOWN:
            ch->state = CHANNEL_STATE_CLOSING;
            ch->is_state_optimistic = true;
            break;
        case COSMO_BTN_STOP:
            if (ch->state == CHANNEL_STATE_OPENING ||
                ch->state == CHANNEL_STATE_CLOSING  ||
                ch->state == CHANNEL_STATE_IN_MOTION) {
                ch->state = CHANNEL_STATE_PARTIALLY_OPEN;
            }
            ch->is_state_optimistic = true;
            break;
        default:
            break;
        }
    } else {
        /* 1-way: jump straight to final state */
        switch (cmd) {
        case COSMO_BTN_UP:
            ch->state = CHANNEL_STATE_OPEN;
            ch->is_state_optimistic = true;
            break;
        case COSMO_BTN_DOWN:
            ch->state = CHANNEL_STATE_CLOSED;
            ch->is_state_optimistic = true;
            break;
        default:
            break;
        }
    }

    /* Capture serial and timeout for timer use outside the mutex */
    uint32_t ch_serial    = ch->serial;
    uint16_t ch_timeout_s = ch->feedback_timeout_s;
    bool     ch_bidir     = ch->bidirectional_feedback;

    cosmo_channel_t copy = *ch;
    channel_mark_dirty();
    xSemaphoreGive(s_mutex);

    /* Manage feedback guard timer (bidirectional channels only) */
    if (ch_bidir) {
        if (cmd == COSMO_BTN_UP || cmd == COSMO_BTN_DOWN) {
            feedback_timer_arm(ch_serial, ch_timeout_s);
        } else if (cmd == COSMO_BTN_STOP) {
            feedback_timer_cancel(ch_serial);
        }
    }

    cosmo_packet_t pkt = {
        .proto         = copy.proto,
        .cmd           = cmd,
        .serial        = copy.serial,
        .counter       = copy.counter,
        .repeat        = 3,
        .extra_payload = extra_payload,
    };
    radio_request_tx(&pkt);
    broadcast_channel_update(&copy);
    mqtt_publish_channel_update(&copy);
    return ESP_OK;
}

void channel_send_all(int fd)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = s_channel_count;
    cosmo_channel_t snapshot[CHANNEL_MAX_COUNT];
    memcpy(snapshot, s_channels, (size_t)count * sizeof(cosmo_channel_t));
    xSemaphoreGive(s_mutex);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "channels");
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++)
        cJSON_AddItemToArray(arr, channel_to_cjson(&snapshot[i]));
    cJSON_AddItemToObject(root, "payload", arr);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) { webserver_ws_send_json_to_fd(fd, json); free(json); }
}

int channel_snapshot(cosmo_channel_t out[CHANNEL_MAX_COUNT])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = s_channel_count;
    memcpy(out, s_channels, (size_t)count * sizeof(cosmo_channel_t));
    xSemaphoreGive(s_mutex);
    return count;
}

esp_err_t channel_find_by_mqtt_name(const char *mqtt_name, cosmo_channel_t *out)
{
    if (!mqtt_name || !out) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_channel_count; i++) {
        if (strcmp(s_channels[i].mqtt_name, mqtt_name) == 0) {
            *out = s_channels[i];
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}
