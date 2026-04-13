#include "mqtt.h"
#include "background_worker.h"
#include "channel.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

static const char *TAG = "mqtt";

/* ── Global status ───────────────────────────────────────────────────────── */

mqtt_status_t g_mqtt_status = MQTT_STATUS_UNCONFIGURED;

/* ── Internal state ──────────────────────────────────────────────────────── */

static esp_mqtt_client_handle_t s_client     = NULL;
static SemaphoreHandle_t        s_mutex;
static gateway_config_t         s_active_cfg; /* config used for the current client init */

/* ── Device class helpers ────────────────────────────────────────────────── */

static bool is_cover_class(cosmo_channel_device_class_t dc)
{
    switch (dc) {
    case CHANNEL_DEVICE_CLASS_GENERIC:
    case CHANNEL_DEVICE_CLASS_AWNING:
    case CHANNEL_DEVICE_CLASS_BLIND:
    case CHANNEL_DEVICE_CLASS_CURTAIN:
    case CHANNEL_DEVICE_CLASS_DAMPER:
    case CHANNEL_DEVICE_CLASS_DOOR:
    case CHANNEL_DEVICE_CLASS_GARAGE:
    case CHANNEL_DEVICE_CLASS_GATE:
    case CHANNEL_DEVICE_CLASS_SHADE:
    case CHANNEL_DEVICE_CLASS_SHUTTER:
    case CHANNEL_DEVICE_CLASS_WINDOW:
        return true;
    default:
        return false;
    }
}

/* Returns the HA device_class string for cover components, or NULL for GENERIC. */
static const char *cover_device_class_str(cosmo_channel_device_class_t dc)
{
    switch (dc) {
    case CHANNEL_DEVICE_CLASS_AWNING:  return "awning";
    case CHANNEL_DEVICE_CLASS_BLIND:   return "blind";
    case CHANNEL_DEVICE_CLASS_CURTAIN: return "curtain";
    case CHANNEL_DEVICE_CLASS_DAMPER:  return "damper";
    case CHANNEL_DEVICE_CLASS_DOOR:    return "door";
    case CHANNEL_DEVICE_CLASS_GARAGE:  return "garage";
    case CHANNEL_DEVICE_CLASS_GATE:    return "gate";
    case CHANNEL_DEVICE_CLASS_SHADE:   return "shade";
    case CHANNEL_DEVICE_CLASS_SHUTTER: return "shutter";
    case CHANNEL_DEVICE_CLASS_WINDOW:  return "window";
    default:                           return NULL; /* GENERIC: omit device_class */
    }
}

/* Map channel state to the MQTT cover state string. Returns NULL for UNKNOWN. */
static const char *channel_state_to_mqtt(cosmo_channel_state_t state)
{
    switch (state) {
    case CHANNEL_STATE_CLOSED:         return "closed";
    case CHANNEL_STATE_CLOSING:        return "closing";
    case CHANNEL_STATE_OPENING:        return "opening";
    case CHANNEL_STATE_OPEN:           return "open";
    case CHANNEL_STATE_IN_MOTION:      return "opening";   /* direction unknown */
    case CHANNEL_STATE_COMFORT:        return "open";
    case CHANNEL_STATE_PARTIALLY_OPEN: return "open";
    case CHANNEL_STATE_OBSTRUCTION:    return "open";
    default:                           return NULL;        /* UNKNOWN: skip */
    }
}

/* ── HA Discovery JSON builder ───────────────────────────────────────────── */

static char *build_discovery_json(const cosmo_channel_t *ch,
                                  const gateway_config_t *cfg)
{
    const char *prefix   = cfg->mqtt.mqtt_prefix;
    const char *mname    = ch->mqtt_name;

    char avty_topic[160];
    snprintf(avty_topic, sizeof(avty_topic), "%s/availability", prefix);

    char state_topic[160], cmd_topic[160], pos_topic[160], set_pos_topic[160];
    char obs_topic[160], rssi_topic[160];
    snprintf(state_topic,   sizeof(state_topic),   "%s/%s/state",        prefix, mname);
    snprintf(cmd_topic,     sizeof(cmd_topic),     "%s/%s/command",      prefix, mname);
    snprintf(pos_topic,     sizeof(pos_topic),     "%s/%s/position",     prefix, mname);
    snprintf(set_pos_topic, sizeof(set_pos_topic), "%s/%s/set_position", prefix, mname);
    snprintf(obs_topic,     sizeof(obs_topic),     "%s/%s/obstruction",  prefix, mname);
    snprintf(rssi_topic,    sizeof(rssi_topic),    "%s/%s/rssi",         prefix, mname);

    char dev_id[128];
    snprintf(dev_id, sizeof(dev_id), "%s_%s", prefix, mname);

    cJSON *root = cJSON_CreateObject();

    /* Device */
    cJSON *dev = cJSON_CreateObject();
    cJSON *ids = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString(dev_id));
    cJSON_AddItemToObject(dev, "ids",  ids);
    cJSON_AddStringToObject(dev, "name", ch->name);
    cJSON_AddStringToObject(dev, "mf",   "uni-gtw");
    cJSON_AddItemToObject(root, "dev", dev);

    /* Origin */
    cJSON *origin = cJSON_CreateObject();
    cJSON_AddStringToObject(origin, "name", "uni-gtw");
    cJSON_AddStringToObject(origin, "sw",   "1.0.0");
    cJSON_AddItemToObject(root, "o", origin);

    /* Availability */
    cJSON *avty_arr = cJSON_CreateArray();
    cJSON *avty_entry = cJSON_CreateObject();
    cJSON_AddStringToObject(avty_entry, "topic",                avty_topic);
    cJSON_AddStringToObject(avty_entry, "payload_available",    "online");
    cJSON_AddStringToObject(avty_entry, "payload_not_available","offline");
    cJSON_AddItemToArray(avty_arr, avty_entry);
    cJSON_AddItemToObject(root, "avty", avty_arr);

    /* Components */
    cJSON *cmps = cJSON_CreateObject();

    if (is_cover_class(ch->device_class)) {
        /* ── Cover ── */
        cJSON *cover = cJSON_CreateObject();
        cJSON_AddStringToObject(cover, "p",             "cover");
        cJSON_AddStringToObject(cover, "name",          "Cover");

        char uid[160];
        snprintf(uid, sizeof(uid), "%s_cover", dev_id);
        cJSON_AddStringToObject(cover, "unique_id",     uid);

        const char *dc_str = cover_device_class_str(ch->device_class);
        if (dc_str)
            cJSON_AddStringToObject(cover, "device_class", dc_str);

        cJSON_AddStringToObject(cover, "command_topic",     cmd_topic);
        cJSON_AddStringToObject(cover, "state_topic",       state_topic);
        /* Position control is only available for two-way (feedback) channels */
        if (ch->proto == PROTO_COSMO_2WAY) {
            cJSON_AddStringToObject(cover, "position_topic",    pos_topic);
            cJSON_AddStringToObject(cover, "set_position_topic",set_pos_topic);
            cJSON_AddNumberToObject(cover, "position_open",     100);
            cJSON_AddNumberToObject(cover, "position_closed",   0);
        }
        cJSON_AddStringToObject(cover, "payload_open",      "OPEN");
        cJSON_AddStringToObject(cover, "payload_close",     "CLOSE");
        cJSON_AddStringToObject(cover, "payload_stop",      "STOP");
        cJSON_AddStringToObject(cover, "state_open",        "open");
        cJSON_AddStringToObject(cover, "state_closed",      "closed");
        cJSON_AddStringToObject(cover, "state_opening",     "opening");
        cJSON_AddStringToObject(cover, "state_closing",     "closing");
        cJSON_AddBoolToObject  (cover, "optimistic",        false);
        cJSON_AddItemToObject(cmps, "cover", cover);

        /* ── Obstruction binary sensor ── */
        cJSON *obs = cJSON_CreateObject();
        cJSON_AddStringToObject(obs, "p",           "binary_sensor");
        cJSON_AddStringToObject(obs, "name",        "Obstruction");
        snprintf(uid, sizeof(uid), "%s_obstruction", dev_id);
        cJSON_AddStringToObject(obs, "unique_id",   uid);
        cJSON_AddStringToObject(obs, "device_class","problem");
        cJSON_AddStringToObject(obs, "state_topic", obs_topic);
        cJSON_AddStringToObject(obs, "payload_on",  "obstruction");
        cJSON_AddStringToObject(obs, "payload_off", "clear");
        cJSON_AddItemToObject(cmps, "obstruction", obs);

    } else if (ch->device_class == CHANNEL_DEVICE_CLASS_SWITCH) {
        /* ── Switch ── */
        cJSON *sw = cJSON_CreateObject();
        cJSON_AddStringToObject(sw, "p",               "switch");
        cJSON_AddStringToObject(sw, "name",            "Switch");
        char uid[160];
        snprintf(uid, sizeof(uid), "%s_switch", dev_id);
        cJSON_AddStringToObject(sw, "unique_id",       uid);
        cJSON_AddStringToObject(sw, "command_topic",   cmd_topic);
        cJSON_AddStringToObject(sw, "state_topic",     state_topic);
        cJSON_AddStringToObject(sw, "payload_on",      "ON");
        cJSON_AddStringToObject(sw, "payload_off",     "OFF");
        cJSON_AddStringToObject(sw, "state_on",        "ON");
        cJSON_AddStringToObject(sw, "state_off",       "OFF");
        cJSON_AddItemToObject(cmps, "switch", sw);

    } else if (ch->device_class == CHANNEL_DEVICE_CLASS_LIGHT) {
        /* ── Light ── */
        cJSON *light = cJSON_CreateObject();
        cJSON_AddStringToObject(light, "p",             "light");
        cJSON_AddStringToObject(light, "name",          "Light");
        char uid[160];
        snprintf(uid, sizeof(uid), "%s_light", dev_id);
        cJSON_AddStringToObject(light, "unique_id",     uid);
        cJSON_AddStringToObject(light, "command_topic", cmd_topic);
        cJSON_AddStringToObject(light, "state_topic",   state_topic);
        cJSON_AddStringToObject(light, "payload_on",    "ON");
        cJSON_AddStringToObject(light, "payload_off",   "OFF");
        cJSON_AddItemToObject(cmps, "light", light);
    }

    /* ── Signal strength sensor (all non-hidden) ── */
    cJSON *rssi_sensor = cJSON_CreateObject();
    cJSON_AddStringToObject(rssi_sensor, "p",                   "sensor");
    cJSON_AddStringToObject(rssi_sensor, "name",                "Signal Strength");
    char uid[160];
    snprintf(uid, sizeof(uid), "%s_rssi", dev_id);
    cJSON_AddStringToObject(rssi_sensor, "unique_id",           uid);
    cJSON_AddStringToObject(rssi_sensor, "device_class",        "signal_strength");
    cJSON_AddStringToObject(rssi_sensor, "state_class",         "measurement");
    cJSON_AddStringToObject(rssi_sensor, "unit_of_measurement", "dBm");
    cJSON_AddStringToObject(rssi_sensor, "state_topic",         rssi_topic);
    cJSON_AddStringToObject(rssi_sensor, "entity_category",     "diagnostic");
    cJSON_AddItemToObject(cmps, "signal_strength", rssi_sensor);

    cJSON_AddItemToObject(root, "cmps", cmps);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out; /* caller must free() */
}

/* ── Subscribe to command topics for all channels ────────────────────────── */

static void subscribe_channel(esp_mqtt_client_handle_t client,
                               const cosmo_channel_t *ch,
                               const char *prefix)
{
    char topic[160];
    snprintf(topic, sizeof(topic), "%s/%s/command", prefix, ch->mqtt_name);
    esp_mqtt_client_subscribe_single(client, topic, 1);

    /* set_position is only available on two-way cover channels */
    if (is_cover_class(ch->device_class) && ch->proto == PROTO_COSMO_2WAY) {
        snprintf(topic, sizeof(topic), "%s/%s/set_position", prefix, ch->mqtt_name);
        esp_mqtt_client_subscribe_single(client, topic, 1);
    }
}

/* ── Publish current state for one channel ───────────────────────────────── */

static void publish_channel_state(esp_mqtt_client_handle_t client,
                                  const cosmo_channel_t *ch,
                                  const char *prefix)
{
    char topic[160];

    if (is_cover_class(ch->device_class)) {
        /* Cover state */
        const char *state_str = channel_state_to_mqtt(ch->state);
        if (state_str) {
            snprintf(topic, sizeof(topic), "%s/%s/state", prefix, ch->mqtt_name);
            esp_mqtt_client_enqueue(client, topic, state_str, 0, 0, 1, true);
        }
        /* Position */
        if (ch->position >= 0) {
            char val[8];
            snprintf(val, sizeof(val), "%d", (int)ch->position);
            snprintf(topic, sizeof(topic), "%s/%s/position", prefix, ch->mqtt_name);
            esp_mqtt_client_enqueue(client, topic, val, 0, 0, 1, true);
        }
        /* Obstruction */
        {
            const char *obs = (ch->state == CHANNEL_STATE_OBSTRUCTION) ? "obstruction" : "clear";
            snprintf(topic, sizeof(topic), "%s/%s/obstruction", prefix, ch->mqtt_name);
            esp_mqtt_client_enqueue(client, topic, obs, 0, 0, 1, true);
        }
    } else {
        /* Switch / Light: ON = open, OFF = closed */
        const char *on_off = NULL;
        switch (ch->state) {
        case CHANNEL_STATE_OPEN:
        case CHANNEL_STATE_OPENING:
        case CHANNEL_STATE_COMFORT:
        case CHANNEL_STATE_PARTIALLY_OPEN:
        case CHANNEL_STATE_IN_MOTION:
            on_off = "ON"; break;
        case CHANNEL_STATE_CLOSED:
        case CHANNEL_STATE_CLOSING:
            on_off = "OFF"; break;
        default:
            break;
        }
        if (on_off) {
            snprintf(topic, sizeof(topic), "%s/%s/state", prefix, ch->mqtt_name);
            esp_mqtt_client_enqueue(client, topic, on_off, 0, 0, 1, true);
        }
    }

    /* RSSI (all) */
    if (ch->rssi != 0) {
        char val[8];
        snprintf(val, sizeof(val), "%d", (int)ch->rssi);
        snprintf(topic, sizeof(topic), "%s/%s/rssi", prefix, ch->mqtt_name);
        esp_mqtt_client_publish(client, topic, val, 0, 0, 1);
    }
}

/* ── Internal helper: publish discovery + subscribe + state for one channel ── */

static void publish_full_channel(esp_mqtt_client_handle_t client,
                                 const cosmo_channel_t *ch,
                                 const gateway_config_t *cfg)
{
    if (ch->device_class == CHANNEL_DEVICE_CLASS_HIDDEN) return;
    const char *prefix = cfg->mqtt.mqtt_prefix;

    if (cfg->mqtt.ha_discovery_enabled) {
        char disc_topic[256];
        snprintf(disc_topic, sizeof(disc_topic),
                 "%s/device/%s_%s/config",
                 cfg->mqtt.ha_prefix, prefix, ch->mqtt_name);
        char *disc_json = build_discovery_json(ch, cfg);
        if (disc_json) {
            esp_mqtt_client_enqueue(client, disc_topic, disc_json, 0, 1, 1, true);
            free(disc_json);
        }
    }
    subscribe_channel(client, ch, prefix);
    publish_channel_state(client, ch, prefix);
}

/* ── Internal helper: clear the HA discovery entry for a channel ─────────── */

static void publish_empty_discovery(esp_mqtt_client_handle_t client,
                                    const cosmo_channel_t *ch,
                                    const gateway_config_t *cfg)
{
    if (!cfg->mqtt.ha_discovery_enabled) return;
    const char *prefix = cfg->mqtt.mqtt_prefix;
    char disc_topic[256];
    snprintf(disc_topic, sizeof(disc_topic),
             "%s/device/%s_%s/config",
             cfg->mqtt.ha_prefix, prefix, ch->mqtt_name);
    /* Publish empty retained payload — HA will remove the device */
    esp_mqtt_client_enqueue(client, disc_topic, "", 0, 1, 1, true);
}

/* ── MQTT event handler ──────────────────────────────────────────────────── */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event  = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "Connected to broker");
        g_mqtt_status = MQTT_STATUS_CONNECTED;

        gateway_config_t cfg;
        config_get(&cfg);
        const char *prefix = cfg.mqtt.mqtt_prefix;

        /* Publish gateway availability */
        char avty_topic[96];
        snprintf(avty_topic, sizeof(avty_topic), "%s/availability", prefix);
        esp_mqtt_client_enqueue(client, avty_topic, "online", 6, 1, 1, true);

        /* For each non-hidden channel: publish discovery, subscribe, publish state */
        cosmo_channel_t channels[CHANNEL_MAX_COUNT];
        int count = channel_snapshot(channels);

        for (int i = 0; i < count; i++) {
            publish_full_channel(client, &channels[i], &cfg);
        }
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Disconnected from broker");
        g_mqtt_status = MQTT_STATUS_DISCONNECTED;
        break;

    case MQTT_EVENT_DATA: {
        if (!event->topic || event->topic_len == 0) break;

        gateway_config_t cfg;
        config_get(&cfg);
        const char *prefix = cfg.mqtt.mqtt_prefix;
        size_t prefix_len  = strlen(prefix);

        /* topic must start with "{prefix}/" */
        if ((size_t)event->topic_len <= prefix_len + 1) break;
        if (strncmp(event->topic, prefix, prefix_len) != 0) break;
        if (event->topic[prefix_len] != '/') break;

        /* topic = "{prefix}/{mqtt_name}/{subtopic}" */
        const char *after_prefix = event->topic + prefix_len + 1;
        int after_len = event->topic_len - (int)prefix_len - 1;

        /* Find the last '/' to split mqtt_name / subtopic */
        int last_slash = -1;
        for (int i = 0; i < after_len; i++)
            if (after_prefix[i] == '/') last_slash = i;
        if (last_slash < 0) break;

        char mqtt_name[64] = {0};
        int name_len = last_slash < 63 ? last_slash : 63;
        memcpy(mqtt_name, after_prefix, (size_t)name_len);

        const char *subtopic     = after_prefix + last_slash + 1;
        int         subtopic_len = after_len - last_slash - 1;

        cosmo_channel_t ch;
        if (channel_find_by_mqtt_name(mqtt_name, &ch) != ESP_OK) break;

        char data[64] = {0};
        int dlen = event->data_len < 63 ? event->data_len : 63;
        memcpy(data, event->data, (size_t)dlen);

        if (strncmp(subtopic, "command", (size_t)subtopic_len) == 0) {
            if (strcmp(data, "OPEN") == 0 || strcmp(data, "ON") == 0)
                channel_send_cmd(ch.serial, COSMO_BTN_UP, 0);
            else if (strcmp(data, "CLOSE") == 0 || strcmp(data, "OFF") == 0)
                channel_send_cmd(ch.serial, COSMO_BTN_DOWN, 0);
            else if (strcmp(data, "STOP") == 0)
                channel_send_cmd(ch.serial, COSMO_BTN_STOP, 0);
            background_worker_inhibit_position_query();

        } else if (strncmp(subtopic, "set_position", (size_t)subtopic_len) == 0) {
            /* Only honour set_position for two-way (feedback) channels */
            if (ch.proto == PROTO_COSMO_2WAY) {
                int pos = atoi(data);
                if (pos < 0)   pos = 0;
                if (pos > 100) pos = 100;
                channel_send_cmd(ch.serial, COSMO_BTN_SET_POSITION, (uint8_t)pos);
                background_worker_inhibit_position_query();
            }
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT error: type=%d", (int)event->error_handle->error_type);
        break;

    default:
        break;
    }
}

/* ── Init / deinit helpers ───────────────────────────────────────────────── */

static void mqtt_do_deinit(void)
{
    if (!s_client) return;
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    g_mqtt_status = MQTT_STATUS_UNCONFIGURED;
    memset(&s_active_cfg, 0, sizeof(s_active_cfg));
}

static void mqtt_do_init(const gateway_config_t *cfg)
{
    if (!cfg->mqtt.enabled || cfg->mqtt.broker[0] == '\0') {
        g_mqtt_status = MQTT_STATUS_UNCONFIGURED;
        return;
    }

    char uri[192];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", cfg->mqtt.broker, cfg->mqtt.port);

    char avty_topic[96];
    snprintf(avty_topic, sizeof(avty_topic), "%s/availability", cfg->mqtt.mqtt_prefix);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri    = uri,
        .session.last_will     = {
            .topic             = avty_topic,
            .msg               = "offline",
            .msg_len           = 7,
            .qos               = 1,
            .retain            = 1,
        },
        .session.keepalive     = 60,
        .task = {
            .stack_size = 8000,
        },
    };

    if (cfg->mqtt.username[0]) {
        mqtt_cfg.credentials.username                  = cfg->mqtt.username;
        mqtt_cfg.credentials.authentication.password  = cfg->mqtt.password;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        g_mqtt_status = MQTT_STATUS_DISCONNECTED;
        return;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    g_mqtt_status = MQTT_STATUS_CONNECTING;
    memcpy(&s_active_cfg, cfg, sizeof(*cfg));
    ESP_LOGI(TAG, "MQTT client started → %s", uri);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void mqtt_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    g_mqtt_status = MQTT_STATUS_UNCONFIGURED;
    memset(&s_active_cfg, 0, sizeof(s_active_cfg));

    gateway_config_t cfg;
    config_get(&cfg);
    mqtt_do_init(&cfg);
}

void mqtt_apply_config(void)
{
    gateway_config_t cfg;
    config_get(&cfg);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = (memcmp(&s_active_cfg.mqtt, &cfg.mqtt, sizeof(cfg.mqtt)) != 0);
    if (changed) {
        ESP_LOGI(TAG, "MQTT config changed, reinitialising");
        mqtt_do_deinit();
        mqtt_do_init(&cfg);
    }
    xSemaphoreGive(s_mutex);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void mqtt_publish_channel_update(const cosmo_channel_t *ch)
{
    if (!ch) return;
    if (ch->device_class == CHANNEL_DEVICE_CLASS_HIDDEN) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_mqtt_client_handle_t client = s_client;
    xSemaphoreGive(s_mutex);

    if (!client || g_mqtt_status != MQTT_STATUS_CONNECTED) return;

    gateway_config_t cfg;
    config_get(&cfg);

    publish_channel_state(client, ch, cfg.mqtt.mqtt_prefix);
}

void mqtt_publish_channel_created(const cosmo_channel_t *ch)
{
    if (!ch) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_mqtt_client_handle_t client = s_client;
    xSemaphoreGive(s_mutex);

    if (!client || g_mqtt_status != MQTT_STATUS_CONNECTED) return;

    gateway_config_t cfg;
    config_get(&cfg);

    publish_full_channel(client, ch, &cfg);
}

void mqtt_publish_channel_deleted(const cosmo_channel_t *ch)
{
    if (!ch) return;
    if (ch->device_class == CHANNEL_DEVICE_CLASS_HIDDEN) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_mqtt_client_handle_t client = s_client;
    xSemaphoreGive(s_mutex);

    if (!client || g_mqtt_status != MQTT_STATUS_CONNECTED) return;

    gateway_config_t cfg;
    config_get(&cfg);

    publish_empty_discovery(client, ch, &cfg);
}

void mqtt_republish_channel_discovery(const cosmo_channel_t *old_ch,
                                      const cosmo_channel_t *new_ch)
{
    if (!old_ch || !new_ch) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_mqtt_client_handle_t client = s_client;
    xSemaphoreGive(s_mutex);

    if (!client || g_mqtt_status != MQTT_STATUS_CONNECTED) return;

    gateway_config_t cfg;
    config_get(&cfg);

    if (cfg.mqtt.ha_discovery_enabled &&
        old_ch->device_class != CHANNEL_DEVICE_CLASS_HIDDEN) {
        publish_empty_discovery(client, old_ch, &cfg);
    }

    publish_full_channel(client, new_ch, &cfg);
}
