#include "channel.h"
#include "config.h"
#include "mqtt.h"
#include "utils.h"
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

static const char *TAG = "channel";

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Derive a default mqtt_name from a channel name: lowercase, non-alphanumeric → '_' */
static void derive_mqtt_name(const char *name, char *out, size_t out_size)
{
    size_t i = 0;
    for (; name[i] && i + 1 < out_size; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[i] = ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ? c : '_';
    }
    out[i] = '\0';
}

/* Convert the sstr_t proto field to a cosmo_proto_t for the radio layer. */
static cosmo_proto_t channel_proto_to_cosmo(sstr_t proto)
{
    return (sstr_compare_c(proto, "2way") == 0) ? PROTO_COSMO_2WAY : PROTO_COSMO_1WAY;
}

/* Deep-copy a cosmo_channel_t. Caller must cosmo_channel_t_clear(dst) after use. */
void channel_deep_copy(struct cosmo_channel_t *dst, const struct cosmo_channel_t *src)
{
    cosmo_channel_t_init(dst);
    dst->serial                = src->serial;
    dst->name                  = sstr_dup(src->name);
    dst->proto                 = sstr_dup(src->proto);
    dst->counter               = src->counter;
    dst->state                 = src->state;
    dst->rssi                  = src->rssi;
    dst->last_seen_ts          = src->last_seen_ts;
    dst->position              = src->position;
    dst->has_position          = src->has_position;
    dst->reports_tilt_support  = src->reports_tilt_support;
    dst->force_tilt_support    = src->force_tilt_support;
    dst->bidirectional_feedback  = src->bidirectional_feedback;
    dst->feedback_timeout_s      = src->feedback_timeout_s;
    dst->state_type              = src->state_type;
    dst->last_command_ts         = src->last_command_ts;
    dst->last_position_query_ts  = src->last_position_query_ts;
    dst->device_class            = src->device_class;
    dst->mqtt_name             = sstr_dup(src->mqtt_name);

    if (src->external_remotes_len > 0 && src->external_remotes) {
        dst->external_remotes = malloc((size_t)src->external_remotes_len * sizeof(uint32_t));
        if (dst->external_remotes) {
            memcpy(dst->external_remotes, src->external_remotes,
                   (size_t)src->external_remotes_len * sizeof(uint32_t));
            dst->external_remotes_len = src->external_remotes_len;
        }
    }
}

/* ── WS broadcast helpers ────────────────────────────────────────────────── */

/* Build and broadcast {"cmd":"channel_update","payload":{...}}.
 * ch must be a valid pointer to a channel (deep copy, not inside g_config). */
static void broadcast_channel_update(const struct cosmo_channel_t *ch)
{
    sstr_t json = sstr_new();
    sstr_append_cstr(json, "{\"cmd\":\"channel_update\",\"payload\":");
    json_marshal_cosmo_channel_t((struct cosmo_channel_t *)ch, json);
    sstr_append_cstr(json, "}");
    webserver_ws_broadcast_json(sstr_cstr(json));
    sstr_free(json);
}

/* Build and broadcast {"cmd":"channel_deleted","serial":N}. */
static void broadcast_channel_deleted(uint32_t serial)
{
    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"cmd\":\"channel_deleted\",\"serial\":%"PRIu32"}", serial);
    webserver_ws_broadcast_json(buf);
}

/* ── find_locked — caller must hold g_config_mutex ───────────────────────── */

static struct cosmo_channel_t *channel_find_locked(uint32_t serial)
{
    for (int i = 0; i < g_config.channels_len; i++)
        if (g_config.channels[i].serial == serial)
            return &g_config.channels[i];
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void channel_init(void)
{
    /* channels are already loaded by config_init() */
}

esp_err_t channel_create(const char *name, const char *proto,
                          int device_class, bool has_device_class,
                          const char *mqtt_name, bool has_mqtt_name)
{
    config_lock();

    /* Generate a unique serial */
    uint32_t serial;
    do {
        serial = esp_random();
        serial &= ~0x1Fu;
    } while (channel_find_locked(serial) != NULL);

    /* Grow the channels array */
    int new_len = g_config.channels_len + 1;
    struct cosmo_channel_t *arr = realloc(g_config.channels,
                                           (size_t)new_len * sizeof(struct cosmo_channel_t));
    if (!arr) {
        config_unlock();
        return ESP_ERR_NO_MEM;
    }
    g_config.channels = arr;

    struct cosmo_channel_t *ch = &g_config.channels[g_config.channels_len];
    cosmo_channel_t_init(ch);

    ch->serial                = serial;
    ch->state                 = channel_state_t_unknown;
    ch->has_position          = 0;
    ch->position              = 0;
    ch->bidirectional_feedback = 1;
    ch->feedback_timeout_s    = 120;
    ch->device_class          = has_device_class
                                 ? device_class
                                 : channel_device_class_t_shutter;
    ch->counter                = 0;
    ch->reports_tilt_support   = 0;
    ch->force_tilt_support     = 0;
    ch->state_type             = channel_state_type_t_reported;
    ch->last_command_ts        = 0;
    ch->last_position_query_ts = 0;

    ch->name  = sstr(name ? name : "Unnamed");
    ch->proto = sstr((proto && strcmp(proto, "2way") == 0) ? "2way" : "1way");

    if (has_mqtt_name && mqtt_name && mqtt_name[0]) {
        ch->mqtt_name = sstr(mqtt_name);
    } else {
        char derived[64];
        derive_mqtt_name(sstr_cstr(ch->name), derived, sizeof(derived));
        ch->mqtt_name = sstr(derived);
    }

    g_config.channels_len = new_len;

    struct cosmo_channel_t copy;
    channel_deep_copy(&copy, ch);
    config_unlock();

    config_mark_dirty();

    ESP_LOGI(TAG, "Created channel \"%s\" serial=0x%08X proto=%s",
             sstr_cstr(copy.name), (unsigned)copy.serial, sstr_cstr(copy.proto));

    broadcast_channel_update(&copy);
    mqtt_publish_channel_created(&copy);
    cosmo_channel_t_clear(&copy);
    return ESP_OK;
}

esp_err_t channel_delete(uint32_t serial)
{
    config_lock();
    int idx = -1;
    for (int i = 0; i < g_config.channels_len; i++) {
        if (g_config.channels[i].serial == serial) { idx = i; break; }
    }
    if (idx < 0) {
        config_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    struct cosmo_channel_t deleted;
    channel_deep_copy(&deleted, &g_config.channels[idx]);

    /* Clear the removed channel's sstr_t fields before overwriting */
    cosmo_channel_t_clear(&g_config.channels[idx]);
    /* Shift remaining channels */
    for (int i = idx; i < g_config.channels_len - 1; i++)
        g_config.channels[i] = g_config.channels[i + 1];
    g_config.channels_len--;

    config_unlock();
    config_mark_dirty();

    ESP_LOGI(TAG, "Deleted channel serial=0x%08X", (unsigned)serial);
    broadcast_channel_deleted(serial);
    mqtt_publish_channel_deleted(&deleted);
    cosmo_channel_t_clear(&deleted);
    return ESP_OK;
}

esp_err_t channel_update(uint32_t serial, const struct ws_update_channel_msg_t *msg)
{
    config_lock();
    struct cosmo_channel_t *ch = channel_find_locked(serial);
    if (!ch) {
        config_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    /* Save old values for MQTT comparison */
    int old_device_class = ch->device_class;
    bool old_proto_is_2way = (sstr_compare_c(ch->proto, "2way") == 0);
    char old_mqtt_name[64], old_name[32];
    strlcpy(old_mqtt_name, sstr_cstr(ch->mqtt_name), sizeof(old_mqtt_name));
    strlcpy(old_name, sstr_cstr(ch->name), sizeof(old_name));

    if (msg->has_name && sstr_length(msg->name) > 0) {
        sstr_clear(ch->name);
        sstr_append(ch->name, msg->name);
    }
    if (msg->has_proto && sstr_length(msg->proto) > 0) {
        sstr_clear(ch->proto);
        sstr_append(ch->proto, msg->proto);
    }
    if (msg->has_force_tilt_support)
        ch->force_tilt_support = msg->force_tilt_support;
    if (msg->has_bidirectional_feedback)
        ch->bidirectional_feedback = msg->bidirectional_feedback;
    if (msg->has_feedback_timeout_s)
        ch->feedback_timeout_s = (uint16_t)msg->feedback_timeout_s;
    if (msg->has_device_class)
        ch->device_class = msg->device_class;

    if (msg->has_mqtt_name && sstr_length(msg->mqtt_name) > 0) {
        sstr_clear(ch->mqtt_name);
        sstr_append(ch->mqtt_name, msg->mqtt_name);
    } else if (msg->has_name && sstr_length(msg->name) > 0) {
        /* Derive new mqtt_name from the new channel name */
        char derived[64];
        derive_mqtt_name(sstr_cstr(ch->name), derived, sizeof(derived));
        sstr_clear(ch->mqtt_name);
        sstr_append_cstr(ch->mqtt_name, derived);
    }

    /* Replace external_remotes list (edit form always sends the full current list). */
    free(ch->external_remotes);
    ch->external_remotes     = NULL;
    ch->external_remotes_len = 0;
    if (msg->external_remotes_len > 0 && msg->external_remotes) {
        ch->external_remotes = malloc((size_t)msg->external_remotes_len * sizeof(uint32_t));
        if (ch->external_remotes) {
            memcpy(ch->external_remotes, msg->external_remotes,
                   (size_t)msg->external_remotes_len * sizeof(uint32_t));
            ch->external_remotes_len = msg->external_remotes_len;
        }
    }

    struct cosmo_channel_t copy;
    channel_deep_copy(&copy, ch);
    config_unlock();

    config_mark_dirty();

    ESP_LOGI(TAG, "Updated channel serial=0x%08X name=\"%s\"",
             (unsigned)copy.serial, sstr_cstr(copy.name));

    broadcast_channel_update(&copy);

    bool discovery_changed = (old_device_class != copy.device_class)               ||
                             ((sstr_compare_c(copy.proto, "2way") == 0) != old_proto_is_2way) ||
                             (strcmp(old_mqtt_name, sstr_cstr(copy.mqtt_name)) != 0) ||
                             (strcmp(old_name,      sstr_cstr(copy.name)) != 0);
    if (discovery_changed) {
        /* Build old_ch with old data for clearing HA discovery */
        struct cosmo_channel_t old_ch;
        cosmo_channel_t_init(&old_ch);
        old_ch.device_class = old_device_class;
        old_ch.mqtt_name    = sstr(old_mqtt_name);
        old_ch.name         = sstr(old_name);
        old_ch.proto        = sstr(old_proto_is_2way ? "2way" : "1way");
        mqtt_republish_channel_discovery(&old_ch, &copy);
        cosmo_channel_t_clear(&old_ch);
    } else {
        mqtt_publish_channel_update(&copy);
    }

    cosmo_channel_t_clear(&copy);
    return ESP_OK;
}

/* Apply optimistic state for a motion command (UP/DOWN/STOP).
 * Caller must hold config_lock.
 * Arms or cancels the feedback timer directly (timer ops with timeout=0 are
 * safe while holding the lock). */
static void apply_optimistic_cmd(struct cosmo_channel_t *ch, cosmo_cmd_t cmd)
{
    bool bidir = (bool)ch->bidirectional_feedback;
    ch->last_command_ts = (int64_t)time(NULL);
    switch (cmd) {
    case COSMO_BTN_UP:
        ch->state      = bidir ? channel_state_t_opening : channel_state_t_open;
        ch->state_type = bidir ? channel_state_type_t_optimistic : channel_state_type_t_reported;
        break;
    case COSMO_BTN_DOWN:
        ch->state      = bidir ? channel_state_t_closing : channel_state_t_closed;
        ch->state_type = bidir ? channel_state_type_t_optimistic : channel_state_type_t_reported;
        break;
    case COSMO_BTN_STOP:
        if (ch->state == channel_state_t_opening  ||
            ch->state == channel_state_t_closing  ||
            ch->state == channel_state_t_in_motion) {
            ch->state = channel_state_t_partially_open;
        }
        ch->state_type = bidir ? channel_state_type_t_optimistic : channel_state_type_t_reported;
        break;
    default:
        break;
    }
}

/* Map RX feedback packet command to channel state.
 * FEEDBACK_IN_MOTION never downgrades a known direction (opening/closing) to
 * the generic in_motion state — the optimistic direction is kept until a
 * definitive feedback arrives. */
static int rx_feedback_state(cosmo_cmd_t cmd, int current)
{
    switch (cmd) {
    case COSMO_BTN_FEEDBACK_TOP:         return channel_state_t_open;
    case COSMO_BTN_FEEDBACK_BOTTOM:      return channel_state_t_closed;
    case COSMO_BTN_FEEDBACK_COMFORT:     return channel_state_t_comfort;
    case COSMO_BTN_FEEDBACK_PARTIAL:     return channel_state_t_partially_open;
    case COSMO_BTN_FEEDBACK_OBSTRUCTION: return channel_state_t_obstruction;
    case COSMO_BTN_FEEDBACK_IN_MOTION:
        if (current == channel_state_t_opening || current == channel_state_t_closing)
            return current;
        return channel_state_t_in_motion;
    default:                             return current;
    }
}

/* Apply received-packet state update to ch (must hold config_lock). */
static void apply_feedback_to_channel(struct cosmo_channel_t *ch, const cosmo_packet_t *pkt)
{
    ch->state      = rx_feedback_state(pkt->cmd, ch->state);
    ch->state_type = channel_state_type_t_reported;
    ch->rssi       = pkt->rssi;
    ch->last_seen_ts        = (int64_t)time(NULL);

    if (pkt->cmd == COSMO_BTN_FEEDBACK_PARTIAL) {
        if(pkt->proto == PROTO_COSMO_2WAY) {
            ch->position     = (int16_t)pkt->extra_payload;
            ch->has_position = 1;
        } else {
            ch->has_position = 0;
        }

    } else if (pkt->cmd == COSMO_BTN_FEEDBACK_TOP ||
               pkt->cmd == COSMO_BTN_FEEDBACK_BOTTOM) {
        ch->has_position = 0;
    }
}

void channel_update_from_packet(const cosmo_packet_t *pkt)
{
    config_lock();

    /* Look up the channel this packet belongs to.  Try the main serial first;
     * if not found, scan external_remotes lists on all channels. */
    bool is_external = false;
    struct cosmo_channel_t *ch = channel_find_locked(pkt->serial);
    if (!ch) {
        for (int i = 0; i < g_config.channels_len && !ch; i++) {
            struct cosmo_channel_t *c = &g_config.channels[i];
            for (int j = 0; j < c->external_remotes_len; j++) {
                if (c->external_remotes[j] == pkt->serial) {
                    ch = c;
                    is_external = true;
                    break;
                }
            }
        }
    }

    /* Unknown serial or bidirectional feedback disabled — nothing to do. */
    if (!ch || !ch->bidirectional_feedback) {
        config_unlock();
        return;
    }

    bool is_motion = (pkt->cmd == COSMO_BTN_UP ||
                      pkt->cmd == COSMO_BTN_DOWN ||
                      pkt->cmd == COSMO_BTN_STOP);

    if (is_external && is_motion) {
        /* Motion command from a paired remote control — apply optimistic state.
         * rssi/last_seen_ts are NOT updated: the packet came from the remote,
         * not from the motor. */
        apply_optimistic_cmd(ch, pkt->cmd);
    } else {
        /* Feedback from the motor (via main serial or external remote serial). */
        apply_feedback_to_channel(ch, pkt);
    }

    struct cosmo_channel_t copy;
    channel_deep_copy(&copy, ch);
    config_unlock();

    broadcast_channel_update(&copy);
    mqtt_publish_channel_update(&copy);
    cosmo_channel_t_clear(&copy);
}

esp_err_t channel_send_cmd(uint32_t serial, cosmo_cmd_t cmd, uint8_t extra_payload)
{
    config_lock();
    struct cosmo_channel_t *ch = channel_find_locked(serial);
    if (!ch) {
        config_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    ch->counter++;

    apply_optimistic_cmd(ch, cmd);

    cosmo_proto_t proto = channel_proto_to_cosmo(ch->proto);

    struct cosmo_channel_t copy;
    channel_deep_copy(&copy, ch);
    config_unlock();

    config_mark_dirty();

    cosmo_packet_t pkt = {
        .proto         = proto,
        .cmd           = cmd,
        .serial        = copy.serial,
        .counter       = (uint16_t)copy.counter,
        .repeat        = 3,
        .extra_payload = extra_payload,
    };
    radio_request_tx(&pkt);
    broadcast_channel_update(&copy);
    mqtt_publish_channel_update(&copy);
    cosmo_channel_t_clear(&copy);
    return ESP_OK;
}

void channel_send_all(int fd)
{
    config_lock();
    sstr_t json = sstr_new();
    sstr_append_cstr(json, "{\"cmd\":\"channels\",\"payload\":");
    json_marshal_array_cosmo_channel_t(g_config.channels, g_config.channels_len, json);
    sstr_append_cstr(json, "}");
    config_unlock();

    webserver_ws_send_json_to_fd(fd, sstr_cstr(json));
    sstr_free(json);
}

esp_err_t channel_find_by_mqtt_name(const char *mqtt_name, struct cosmo_channel_t *out)
{
    if (!mqtt_name || !out) return ESP_ERR_INVALID_ARG;
    config_lock();
    for (int i = 0; i < g_config.channels_len; i++) {
        if (sstr_compare_c(g_config.channels[i].mqtt_name, mqtt_name) == 0) {
            channel_deep_copy(out, &g_config.channels[i]);
            config_unlock();
            return ESP_OK;
        }
    }
    config_unlock();
    return ESP_ERR_NOT_FOUND;
}

/* ── Timeout checker (called from background worker every CHECK_INTERVAL_MS) */

void channel_check_timeouts(void)
{
    if (!utils_time_is_valid())
        return;
    time_t now = time(NULL);

    uint32_t serial = 0;

    config_lock();
    for (int i = 0; i < g_config.channels_len; i++) {
        struct cosmo_channel_t *ch = &g_config.channels[i];
        if (ch->state_type != channel_state_type_t_optimistic)
            continue;
        /* Timeout measured from the latest of last motor feedback or last
         * sent command, so sending a new command postpones the window. */
        time_t last_activity = ((time_t)ch->last_seen_ts > (time_t)ch->last_command_ts)
                               ? (time_t)ch->last_seen_ts
                               : (time_t)ch->last_command_ts;
        if (last_activity <= 0)
            continue;
        if (now - last_activity < (time_t)ch->feedback_timeout_s)
            continue;

        if (ch->state == channel_state_t_opening  ||
            ch->state == channel_state_t_closing  ||
            ch->state == channel_state_t_in_motion)
            ch->state = channel_state_t_partially_open;
        ch->state_type = channel_state_type_t_timed_out;
        serial = ch->serial;
        break; /* one channel per tick */
    }
    config_unlock();

    if (!serial)
        return;

    config_lock();
    struct cosmo_channel_t *ch = channel_find_locked(serial);
    if (!ch) { config_unlock(); return; }
    struct cosmo_channel_t copy;
    channel_deep_copy(&copy, ch);
    config_unlock();

    ESP_LOGI(TAG, "Feedback timeout serial=0x%08X, state→timed_out", (unsigned)serial);
    broadcast_channel_update(&copy);
    mqtt_publish_channel_update(&copy);
    cosmo_channel_t_clear(&copy);
}
