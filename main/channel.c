#include "channel.h"
#include "config.h"
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
#include "cJSON.h"

static const char *TAG = "channel";

/* ── State ───────────────────────────────────────────────────────────────── */

static cosmo_channel_t   s_channels[CHANNEL_MAX_COUNT];
static int               s_channel_count = 0;
static SemaphoreHandle_t s_mutex;

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

static cJSON *channel_to_cjson(const cosmo_channel_t *ch)
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

/* Optimistic state for TX: only UP and DOWN change the displayed state. */
static cosmo_channel_state_t tx_optimistic_state(cosmo_cmd_t cmd, cosmo_channel_state_t current)
{
    switch (cmd) {
    case COSMO_BTN_UP:   return CHANNEL_STATE_OPENING;
    case COSMO_BTN_DOWN: return CHANNEL_STATE_CLOSING;
    default:             return current;
    }
}

/* State from received feedback packets only. */
static cosmo_channel_state_t rx_feedback_state(cosmo_cmd_t cmd, cosmo_channel_state_t current)
{
    switch (cmd) {
    case COSMO_BTN_FEEDBACK_TOP:             return CHANNEL_STATE_OPEN;
    case COSMO_BTN_FEEDBACK_BOTTOM:          return CHANNEL_STATE_CLOSED;
    case COSMO_BTN_FEEDBACK_COMFORT:         return CHANNEL_STATE_COMFORT;
    case COSMO_BTN_FEEDBACK_PARTIAL:         return CHANNEL_STATE_PARTIALLY_OPEN;
    case COSMO_BTN_FEEDBACK_OBSTRUCTION:     return CHANNEL_STATE_OBSTRUCTION;
    case COSMO_BTN_FEEDBACK_IN_MOTION:       return CHANNEL_STATE_IN_MOTION;
    default:                                 return current;
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

/* ── Public API ──────────────────────────────────────────────────────────── */

void channel_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_channel_count = 0;
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
    serial &= ~0x1F; // last 5 significant bits are always zero
    do {
        serial = esp_random();
    } while (channel_find_locked(serial) != NULL);

    cosmo_channel_t *ch = &s_channels[s_channel_count++];
    memset(ch, 0, sizeof(*ch));
    snprintf(ch->name, sizeof(ch->name), "%s", name ? name : "Unnamed");
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
    for (int i = idx; i < s_channel_count - 1; i++)
        s_channels[i] = s_channels[i + 1];
    s_channel_count--;
    channel_mark_dirty();
    xSemaphoreGive(s_mutex);

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

void channel_update_from_packet(const cosmo_packet_t *pkt)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cosmo_channel_t *ch = channel_find_locked(pkt->serial);
    if (!ch) {
        xSemaphoreGive(s_mutex);
        return; /* unknown serial — ignore */
    }

    cosmo_channel_state_t new_state = rx_feedback_state(pkt->cmd, ch->state);
    bool state_changed = (new_state != ch->state);
    ch->state        = new_state;
    ch->rssi         = pkt->rssi;
    ch->last_seen_ts = (int64_t)time(NULL);

    if (pkt->cmd == COSMO_BTN_FEEDBACK_PARTIAL)
        ch->position = (int16_t)pkt->extra_payload;
    else if (pkt->cmd == COSMO_BTN_FEEDBACK_TOP || pkt->cmd == COSMO_BTN_FEEDBACK_BOTTOM)
        ch->position = -1;

    cosmo_channel_t copy = *ch;
    xSemaphoreGive(s_mutex);

    /* Always broadcast on RX (rssi/last_seen_ts changed even if state didn't) */
    broadcast_channel_update(&copy);
    (void)state_changed;
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
    ch->state = tx_optimistic_state(cmd, ch->state);

    cosmo_channel_t copy = *ch;
    channel_mark_dirty();
    xSemaphoreGive(s_mutex);

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
