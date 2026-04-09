#include "channel.h"
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
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "channel";
static const char *CONFIG_PATH = "/littlefs/config.json";

/* ── State ───────────────────────────────────────────────────────────────── */

static cosmo_channel_t   s_channels[CHANNEL_MAX_COUNT];
static int               s_channel_count = 0;
static SemaphoreHandle_t s_mutex;
static TimerHandle_t     s_save_timer;
static TaskHandle_t      s_save_task;

/* ── Config save/load ────────────────────────────────────────────────────── */

static void config_save(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = s_channel_count;
    cosmo_channel_t snap[CHANNEL_MAX_COUNT];
    memcpy(snap, s_channels, (size_t)count * sizeof(cosmo_channel_t));
    xSemaphoreGive(s_mutex);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *ch = cJSON_CreateObject();
        cJSON_AddStringToObject(ch, "name",    snap[i].name);
        cJSON_AddStringToObject(ch, "proto",   snap[i].proto == PROTO_COSMO_2WAY ? "2way" : "1way");
        cJSON_AddNumberToObject(ch, "serial",  (double)(uint32_t)snap[i].serial);
        cJSON_AddNumberToObject(ch, "counter", (double)snap[i].counter);
        cJSON_AddItemToArray(arr, ch);
    }
    cJSON_AddItemToObject(root, "channels", arr);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (str) {
        FILE *f = fopen(CONFIG_PATH, "w");
        if (f) {
            fputs(str, f);
            fclose(f);
            ESP_LOGI(TAG, "Config saved (%d channels)", count);
        } else {
            ESP_LOGW(TAG, "Failed to open config for writing");
        }
        free(str);
    }
}

static void config_load(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No config file found, starting fresh");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 65536) { fclose(f); return; }

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[size] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "Config JSON parse error");
        return;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "channels");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return;
    }

    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (count >= CHANNEL_MAX_COUNT) break;
        const char *name      = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
        const char *proto_str = cJSON_GetStringValue(cJSON_GetObjectItem(item, "proto"));
        cJSON *serial_j  = cJSON_GetObjectItem(item, "serial");
        cJSON *counter_j = cJSON_GetObjectItem(item, "counter");
        if (!name || !proto_str || !cJSON_IsNumber(serial_j) || !cJSON_IsNumber(counter_j))
            continue;
        cosmo_channel_t *ch = &s_channels[count++];
        memset(ch, 0, sizeof(*ch));
        snprintf(ch->name, sizeof(ch->name), "%s", name);
        ch->proto   = (strcmp(proto_str, "2way") == 0) ? PROTO_COSMO_2WAY : PROTO_COSMO_1WAY;
        ch->serial   = (uint32_t)cJSON_GetNumberValue(serial_j);
        ch->serial  &= ~0x1F;
        ch->counter  = (uint16_t)cJSON_GetNumberValue(counter_j);
        ch->state    = CHANNEL_STATE_UNKNOWN;
        ch->position = -1;
    }
    s_channel_count = count;
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d channel(s) from config", count);
}

static void save_task_fn(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        config_save();
    }
}

static void save_timer_cb(TimerHandle_t t)
{
    if (s_save_task)
        xTaskNotifyGive(s_save_task);
}

/* Mark channels dirty – resets the 5-second save debounce timer. */
static void channel_mark_dirty(void)
{
    if (s_save_timer)
        xTimerReset(s_save_timer, 0);
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

/* Minimal JSON string escape for user-supplied channel names */
static size_t ch_json_escape(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (; *src && n + 7 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if      (c == '"')  { dst[n++] = '\\'; dst[n++] = '"';  }
        else if (c == '\\') { dst[n++] = '\\'; dst[n++] = '\\'; }
        else if (c == '\n') { dst[n++] = '\\'; dst[n++] = 'n';  }
        else if (c == '\r') { dst[n++] = '\\'; dst[n++] = 'r';  }
        else if (c == '\t') { dst[n++] = '\\'; dst[n++] = 't';  }
        else if (c < 0x20)  { n += (size_t)snprintf(dst + n, cap - n, "\\u%04X", c); }
        else                { dst[n++] = (char)c; }
    }
    dst[n] = '\0';
    return n;
}

static size_t channel_to_json(const cosmo_channel_t *ch, char *buf, size_t cap)
{
    const char *proto_str = (ch->proto == PROTO_COSMO_2WAY) ? "2way" : "1way";
    char name_esc[128];
    ch_json_escape(name_esc, sizeof(name_esc), ch->name);
    size_t n = (size_t)snprintf(buf, cap,
        "{\"serial\":%u,\"name\":\"%s\",\"proto\":\"%s\",\"counter\":%u"
        ",\"state\":\"%s\",\"rssi\":%d,\"last_seen_ts\":%lld",
        (unsigned)ch->serial, name_esc, proto_str,
        (unsigned)ch->counter, channel_state_name(ch->state),
        (int)ch->rssi, (long long)ch->last_seen_ts);
    if (ch->proto == PROTO_COSMO_2WAY && ch->position >= 0)
        n += (size_t)snprintf(buf + n, cap - n, ",\"position\":%d", (int)ch->position);
    else
        n += (size_t)snprintf(buf + n, cap - n, ",\"position\":null");
    n += (size_t)snprintf(buf + n, cap - n, "}");
    return n;
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
    char ch_json[320];
    channel_to_json(ch, ch_json, sizeof(ch_json));

    char json[400];
    snprintf(json, sizeof(json), "{\"cmd\":\"channel_update\",\"payload\":%s}", ch_json);
    webserver_ws_broadcast_json(json);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void channel_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_channel_count = 0;

    config_load();

    s_save_timer = xTimerCreate("chan_save", pdMS_TO_TICKS(5000),
                                pdFALSE, NULL, save_timer_cb);
    xTaskCreate(save_task_fn, "chan_save", 4096, NULL, 5, &s_save_task);
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
    char json[64];
    snprintf(json, sizeof(json),
             "{\"cmd\":\"channel_deleted\",\"payload\":{\"serial\":%u}}", (unsigned)serial);
    webserver_ws_broadcast_json(json);
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

    /* Each channel JSON is ~320 bytes; add overhead for wrapper */
    size_t cap = (size_t)count * 352 + 64;
    if (cap < 64) cap = 64;
    char *json = malloc(cap);
    if (!json) return;

    size_t n = 0;
    n += (size_t)snprintf(json + n, cap - n, "{\"cmd\":\"channels\",\"payload\":[");
    for (int i = 0; i < count; i++) {
        if (i > 0 && n < cap) json[n++] = ',';
        char ch_json[320];
        channel_to_json(&snapshot[i], ch_json, sizeof(ch_json));
        size_t ch_len = strlen(ch_json);
        if (n + ch_len + 4 < cap) {
            memcpy(json + n, ch_json, ch_len);
            n += ch_len;
        }
    }
    if (n < cap) {
        json[n++] = ']';
        json[n++] = '}';
        json[n]   = '\0';
    }

    webserver_ws_send_json_to_fd(fd, json);
    free(json);
}
