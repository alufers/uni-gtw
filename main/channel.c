#include "channel.h"
#include "radio.h"
#include "webserver.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "channel";

/* ── State ───────────────────────────────────────────────────────────────── */

static cosmo_channel_t   s_channels[CHANNEL_MAX_COUNT];
static int               s_channel_count = 0;
static SemaphoreHandle_t s_mutex;

/* ── JSON helpers ────────────────────────────────────────────────────────── */

static const char *channel_state_name(cosmo_channel_state_t state)
{
    switch (state) {
    case CHANNEL_STATE_CLOSED:         return "closed";
    case CHANNEL_STATE_OPEN:           return "open";
    case CHANNEL_STATE_COMFORT:        return "comfort";
    case CHANNEL_STATE_PARTIALLY_OPEN: return "partially_open";
    case CHANNEL_STATE_OBSTRUCTION:    return "obstruction";
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
    return (size_t)snprintf(buf, cap,
        "{\"serial\":%u,\"name\":\"%s\",\"proto\":\"%s\",\"counter\":%u,\"state\":\"%s\"}",
        (unsigned)ch->serial, name_esc, proto_str,
        (unsigned)ch->counter, channel_state_name(ch->state));
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

/* Map an incoming command to a channel state. */
static cosmo_channel_state_t cmd_to_state(cosmo_cmd_t cmd, cosmo_channel_state_t current)
{
    switch (cmd) {
    case COSMO_BTN_UP:
    case COSMO_BTN_FEEDBACK_TOP:      return CHANNEL_STATE_OPEN;
    case COSMO_BTN_DOWN:
    case COSMO_BTN_FEEDBACK_BOTTOM:   return CHANNEL_STATE_CLOSED;
    case COSMO_BTN_FEEDBACK_COMFORT:  return CHANNEL_STATE_COMFORT;
    case COSMO_BTN_FEEDBACK_PARTIAL:  return CHANNEL_STATE_PARTIALLY_OPEN;
    case COSMO_BTN_OBSTRUCTION:       return CHANNEL_STATE_OBSTRUCTION;
    default:                          return current;
    }
}

static void broadcast_channel_update(const cosmo_channel_t *ch)
{
    char ch_json[256];
    channel_to_json(ch, ch_json, sizeof(ch_json));

    char json[320];
    snprintf(json, sizeof(json), "{\"cmd\":\"channel_update\",\"payload\":%s}", ch_json);
    webserver_ws_broadcast_json(json);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void channel_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_channel_count = 0;
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
    } while (channel_find_locked(serial) != NULL);

    cosmo_channel_t *ch = &s_channels[s_channel_count++];
    memset(ch, 0, sizeof(*ch));
    snprintf(ch->name, sizeof(ch->name), "%s", name ? name : "Unnamed");
    ch->proto   = proto;
    ch->serial  = serial;
    ch->counter = 0;
    ch->state   = CHANNEL_STATE_UNKNOWN;

    cosmo_channel_t copy = *ch;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Created channel \"%s\" serial=0x%08X proto=%s",
             copy.name, (unsigned)copy.serial,
             (proto == PROTO_COSMO_2WAY) ? "2way" : "1way");

    broadcast_channel_update(&copy);
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

    cosmo_channel_state_t new_state = cmd_to_state(pkt->cmd, ch->state);
    bool changed    = (new_state != ch->state);
    ch->state       = new_state;
    ch->counter     = pkt->counter;

    cosmo_channel_t copy = *ch;
    xSemaphoreGive(s_mutex);

    if (changed)
        broadcast_channel_update(&copy);
}

esp_err_t channel_send_cmd(uint32_t serial, cosmo_cmd_t cmd)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cosmo_channel_t *ch = channel_find_locked(serial);
    if (!ch) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    ch->counter++;
    ch->state = cmd_to_state(cmd, ch->state);

    cosmo_channel_t copy = *ch;
    xSemaphoreGive(s_mutex);

    cosmo_packet_t pkt = {
        .proto   = copy.proto,
        .cmd     = cmd,
        .serial  = copy.serial,
        .counter = copy.counter,
        .repeat  = 3,
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

    /* Each channel JSON is ~200 bytes; add overhead for wrapper */
    size_t cap = (size_t)count * 256 + 64;
    if (cap < 64) cap = 64;
    char *json = malloc(cap);
    if (!json) return;

    size_t n = 0;
    n += (size_t)snprintf(json + n, cap - n, "{\"cmd\":\"channels\",\"payload\":[");
    for (int i = 0; i < count; i++) {
        if (i > 0 && n < cap) json[n++] = ',';
        char ch_json[256];
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
