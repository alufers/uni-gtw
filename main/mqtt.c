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

static const char *TAG = "mqtt";

/* ── Global status ───────────────────────────────────────────────────────── */

int g_mqtt_status = mqtt_status_t_unconfigured;

/* ── Internal state ──────────────────────────────────────────────────────── */

static esp_mqtt_client_handle_t s_client = NULL;
static SemaphoreHandle_t        s_mutex;

/* Snapshot of mqtt config used for the running client (to detect changes).
 * Protected by s_mutex. Uses plain chars to avoid sstr_t lifetime issues. */
typedef struct {
    bool     enabled;
    char     broker[128];
    int      port;
    char     username[64];
    char     password[128];
} mqtt_active_cfg_t;

static mqtt_active_cfg_t s_active_cfg;

/* ── Device class helpers ────────────────────────────────────────────────── */

static bool is_cover_class(int dc)
{
    switch (dc) {
    case channel_device_class_t_generic:
    case channel_device_class_t_awning:
    case channel_device_class_t_blind:
    case channel_device_class_t_curtain:
    case channel_device_class_t_damper:
    case channel_device_class_t_door:
    case channel_device_class_t_garage:
    case channel_device_class_t_gate:
    case channel_device_class_t_shade:
    case channel_device_class_t_shutter:
    case channel_device_class_t_window:
        return true;
    default:
        return false;
    }
}

/* Returns the HA device_class string for cover, or NULL for GENERIC. */
static const char *cover_device_class_str(int dc)
{
    switch (dc) {
    case channel_device_class_t_awning:  return "awning";
    case channel_device_class_t_blind:   return "blind";
    case channel_device_class_t_curtain: return "curtain";
    case channel_device_class_t_damper:  return "damper";
    case channel_device_class_t_door:    return "door";
    case channel_device_class_t_garage:  return "garage";
    case channel_device_class_t_gate:    return "gate";
    case channel_device_class_t_shade:   return "shade";
    case channel_device_class_t_shutter: return "shutter";
    case channel_device_class_t_window:  return "window";
    default:                             return NULL;
    }
}

/* Map channel state to the MQTT cover state string. Returns NULL for UNKNOWN. */
static const char *channel_state_to_mqtt(int state)
{
    switch (state) {
    case channel_state_t_closed:         return "closed";
    case channel_state_t_closing:        return "closing";
    case channel_state_t_opening:        return "opening";
    case channel_state_t_open:           return "open";
    case channel_state_t_in_motion:      return "opening";
    case channel_state_t_comfort:        return "open";
    case channel_state_t_partially_open: return "open";
    case channel_state_t_obstruction:    return "open";
    default:                             return NULL;
    }
}

/* ── HA Discovery JSON builder using json-gen-c ──────────────────────────── */

static char *build_discovery_json(const struct cosmo_channel_t *ch,
                                  const char *prefix, const char *ha_prefix,
                                  bool ha_discovery_enabled)
{
    (void)ha_discovery_enabled; /* always called with enabled=true */

    const char *mname  = sstr_cstr(ch->mqtt_name);
    const char *chname = sstr_cstr(ch->name);

    char avty_topic[160], state_topic[160], cmd_topic[160];
    char pos_topic[160], set_pos_topic[160], obs_topic[160], rssi_topic[160];
    char dev_id[128], uid[160];

    snprintf(avty_topic,     sizeof(avty_topic),     "%s/availability",       prefix);
    snprintf(state_topic,    sizeof(state_topic),    "%s/%s/state",           prefix, mname);
    snprintf(cmd_topic,      sizeof(cmd_topic),      "%s/%s/command",         prefix, mname);
    snprintf(pos_topic,      sizeof(pos_topic),      "%s/%s/position",        prefix, mname);
    snprintf(set_pos_topic,  sizeof(set_pos_topic),  "%s/%s/set_position",    prefix, mname);
    snprintf(obs_topic,      sizeof(obs_topic),      "%s/%s/obstruction",     prefix, mname);
    snprintf(rssi_topic,     sizeof(rssi_topic),     "%s/%s/rssi",            prefix, mname);
    snprintf(dev_id,         sizeof(dev_id),         "%s_%s",                 prefix, mname);

    bool is_2way = (sstr_compare_c(ch->proto, "2way") == 0);

    struct ha_discovery_msg_t msg;
    ha_discovery_msg_t_init(&msg);

    /* Device */
    {
        sstr_t id_str = sstr(dev_id);
        /* ha_device_info_t.ids is a sstr_t[] dynamic array */
        msg.device.ids = malloc(sizeof(sstr_t));
        if (msg.device.ids) {
            msg.device.ids[0] = id_str;
            msg.device.ids_len = 1;
        } else {
            sstr_free(id_str);
        }
        msg.device.name = sstr(chname);
        msg.device.mf   = sstr("uni-gtw");
    }

    /* Origin */
    msg.origin.name = sstr("uni-gtw");
    msg.origin.sw   = sstr("1.0.0");

    /* Availability */
    {
        msg.availability = malloc(sizeof(struct ha_availability_entry_t));
        if (msg.availability) {
            ha_availability_entry_t_init(&msg.availability[0]);
            msg.availability[0].topic                = sstr(avty_topic);
            msg.availability[0].payload_available    = sstr("online");
            msg.availability[0].payload_not_available = sstr("offline");
            msg.availability_len = 1;
        }
    }

    /* Components */
    if (is_cover_class(ch->device_class)) {
        /* Cover component */
        msg.components.has_cover = 1;
        struct ha_cover_component_t *cover = &msg.components.cover;
        ha_cover_component_t_init(cover);

        cover->p    = sstr("cover");
        cover->name = sstr("Cover");
        snprintf(uid, sizeof(uid), "%s_cover", dev_id);
        cover->unique_id = sstr(uid);

        const char *dc_str = cover_device_class_str(ch->device_class);
        if (dc_str) {
            cover->has_device_class = 1;
            cover->device_class = sstr(dc_str);
        }

        cover->command_topic = sstr(cmd_topic);
        cover->state_topic   = sstr(state_topic);

        if (is_2way) {

            cover->has_set_position_topic = 1;

            cover->set_position_topic = sstr(set_pos_topic);

        }
        cover->has_position_topic     = 1;
        cover->has_position_open      = 1;
        cover->has_position_closed    = 1;
        cover->position_topic     = sstr(pos_topic);
        cover->position_open   = 100;
        cover->position_closed = 0;

        cover->payload_open  = sstr("OPEN");
        cover->payload_close = sstr("CLOSE");
        cover->payload_stop  = sstr("STOP");
        cover->state_open    = sstr("open");
        cover->state_closed  = sstr("closed");
        cover->state_opening = sstr("opening");
        cover->state_closing = sstr("closing");
        cover->optimistic = 0;

        /* Obstruction binary sensor */
        msg.components.has_obstruction = 1;
        struct ha_obstruction_component_t *obs = &msg.components.obstruction;
        ha_obstruction_component_t_init(obs);
        obs->p            = sstr("binary_sensor");
        obs->name         = sstr("Obstruction");
        snprintf(uid, sizeof(uid), "%s_obstruction", dev_id);
        obs->unique_id    = sstr(uid);
        obs->device_class = sstr("problem");
        obs->state_topic  = sstr(obs_topic);
        obs->payload_on   = sstr("obstruction");
        obs->payload_off  = sstr("clear");

    } else if (ch->device_class == channel_device_class_t_switch) {
        msg.components.has_switch_component = 1;
        struct ha_switch_component_t *sw = &msg.components.switch_component;
        ha_switch_component_t_init(sw);
        sw->p             = sstr("switch");
        sw->name          = sstr("Switch");
        snprintf(uid, sizeof(uid), "%s_switch", dev_id);
        sw->unique_id     = sstr(uid);
        sw->command_topic = sstr(cmd_topic);
        sw->state_topic   = sstr(state_topic);
        sw->payload_on    = sstr("ON");
        sw->payload_off   = sstr("OFF");
        sw->state_on      = sstr("ON");
        sw->state_off     = sstr("OFF");

    } else if (ch->device_class == channel_device_class_t_light) {
        msg.components.has_light = 1;
        struct ha_light_component_t *light = &msg.components.light;
        ha_light_component_t_init(light);
        light->p             = sstr("light");
        light->name          = sstr("Light");
        snprintf(uid, sizeof(uid), "%s_light", dev_id);
        light->unique_id     = sstr(uid);
        light->command_topic = sstr(cmd_topic);
        light->state_topic   = sstr(state_topic);
        light->payload_on    = sstr("ON");
        light->payload_off   = sstr("OFF");
    }

    /* Signal strength sensor (all non-hidden) */
    {
        msg.components.has_signal_strength = 1;
        struct ha_rssi_sensor_component_t *rssi = &msg.components.signal_strength;
        ha_rssi_sensor_component_t_init(rssi);
        rssi->p                   = sstr("sensor");
        rssi->name                = sstr("Signal Strength");
        snprintf(uid, sizeof(uid), "%s_rssi", dev_id);
        rssi->unique_id           = sstr(uid);
        rssi->device_class        = sstr("signal_strength");
        rssi->state_class         = sstr("measurement");
        rssi->unit_of_measurement = sstr("dBm");
        rssi->state_topic         = sstr(rssi_topic);
        rssi->entity_category     = sstr("diagnostic");
    }

    sstr_t out = sstr_new();
    json_marshal_ha_discovery_msg_t(&msg, out);
    ha_discovery_msg_t_clear(&msg);

    char *result = strdup(sstr_cstr(out));
    sstr_free(out);
    return result; /* caller must free() */
}

/* ── Subscribe to command topics ─────────────────────────────────────────── */

static void subscribe_channel(esp_mqtt_client_handle_t client,
                               const struct cosmo_channel_t *ch,
                               const char *prefix)
{
    char topic[160];
    snprintf(topic, sizeof(topic), "%s/%s/command", prefix, sstr_cstr(ch->mqtt_name));
    esp_mqtt_client_subscribe_single(client, topic, 1);

    if (is_cover_class(ch->device_class) &&
        sstr_compare_c(ch->proto, "2way") == 0) {
        snprintf(topic, sizeof(topic), "%s/%s/set_position", prefix, sstr_cstr(ch->mqtt_name));
        esp_mqtt_client_subscribe_single(client, topic, 1);
    }
}

/* ── Publish channel state ───────────────────────────────────────────────── */

static void publish_channel_state(esp_mqtt_client_handle_t client,
                                  const struct cosmo_channel_t *ch,
                                  const char *prefix)
{
    char topic[160];

    if (is_cover_class(ch->device_class)) {
        const char *state_str = channel_state_to_mqtt(ch->state);
        if (state_str) {
            snprintf(topic, sizeof(topic), "%s/%s/state", prefix, sstr_cstr(ch->mqtt_name));
            esp_mqtt_client_enqueue(client, topic, state_str, 0, 0, 1, true);
        }
 int pos_val = 0;
        switch (ch->state) {
            case channel_state_t_open:
                pos_val = 100; break;

            case channel_state_t_opening:
            case channel_state_t_comfort:
            case channel_state_t_partially_open:
            case channel_state_t_in_motion:
            case channel_state_t_closing:
            if (ch->has_position) {
                pos_val = ch->position;
            } else {
                pos_val = 50;
            }
            break;
            case channel_state_t_closed:
                    pos_val = 0; break;
            default: break;
        }



        char val[8];
        snprintf(val, sizeof(val), "%d", pos_val);
        snprintf(topic, sizeof(topic), "%s/%s/position", prefix, sstr_cstr(ch->mqtt_name));
        esp_mqtt_client_enqueue(client, topic, val, 0, 0, 1, true);


        {
            const char *obs = (ch->state == channel_state_t_obstruction)
                              ? "obstruction" : "clear";
            snprintf(topic, sizeof(topic), "%s/%s/obstruction", prefix, sstr_cstr(ch->mqtt_name));
            esp_mqtt_client_enqueue(client, topic, obs, 0, 0, 1, true);
        }
    } else {
        const char *on_off = NULL;
        switch (ch->state) {
        case channel_state_t_open:
        case channel_state_t_opening:
        case channel_state_t_comfort:
        case channel_state_t_partially_open:
        case channel_state_t_in_motion:
            on_off = "ON"; break;
        case channel_state_t_closed:
        case channel_state_t_closing:
            on_off = "OFF"; break;
        default: break;
        }
        if (on_off) {
            snprintf(topic, sizeof(topic), "%s/%s/state", prefix, sstr_cstr(ch->mqtt_name));
            esp_mqtt_client_enqueue(client, topic, on_off, 0, 0, 1, true);
        }
    }

    if (ch->rssi != 0) {
        char val[8];
        snprintf(val, sizeof(val), "%d", (int)ch->rssi);
        snprintf(topic, sizeof(topic), "%s/%s/rssi", prefix, sstr_cstr(ch->mqtt_name));
        esp_mqtt_client_publish(client, topic, val, 0, 0, 1);
    }
}

/* ── Internal: publish discovery + subscribe + state ────────────────────── */

static void publish_full_channel(esp_mqtt_client_handle_t client,
                                 const struct cosmo_channel_t *ch,
                                 const char *prefix, const char *ha_prefix,
                                 bool ha_disc_enabled)
{
    if (ch->device_class == channel_device_class_t_hidden) return;

    if (ha_disc_enabled) {
        char disc_topic[256];
        snprintf(disc_topic, sizeof(disc_topic),
                 "%s/device/%s_%s/config",
                 ha_prefix, prefix, sstr_cstr(ch->mqtt_name));
        char *disc_json = build_discovery_json(ch, prefix, ha_prefix, true);
        if (disc_json) {
            ESP_LOGI(TAG, "Publishingto %s:  %s ", disc_topic, disc_json);
            int ret = esp_mqtt_client_enqueue(client, disc_topic, disc_json, 0, 1, 1, true);
            if(ret < 0) {
                  ESP_LOGE(TAG, "Failed to publish homeassistant device config with error code %d", ret);
            }
            free(disc_json);
        }
    }
    subscribe_channel(client, ch, prefix);
    publish_channel_state(client, ch, prefix);
}

static void publish_empty_discovery(esp_mqtt_client_handle_t client,
                                    const struct cosmo_channel_t *ch,
                                    const char *prefix, const char *ha_prefix,
                                    bool ha_disc_enabled)
{
    if (!ha_disc_enabled) return;
    char disc_topic[256];
    snprintf(disc_topic, sizeof(disc_topic),
             "%s/device/%s_%s/config",
             ha_prefix, prefix, sstr_cstr(ch->mqtt_name));
    esp_mqtt_client_enqueue(client, disc_topic, "", 0, 1, 1, true);
}

/* ── MQTT event handler ──────────────────────────────────────────────────── */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event   = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "Connected to broker");
        g_mqtt_status = mqtt_status_t_connected;

        /* Read config under lock */
        char prefix[64], ha_prefix[64];
        bool ha_disc;
        config_lock();
        strlcpy(prefix,    sstr_cstr(g_config.mqtt.mqtt_prefix), sizeof(prefix));
        strlcpy(ha_prefix, sstr_cstr(g_config.mqtt.ha_prefix),   sizeof(ha_prefix));
        ha_disc = (bool)g_config.mqtt.ha_discovery_enabled;
        config_unlock();

        /* Publish gateway availability */
        char avty_topic[96];
        snprintf(avty_topic, sizeof(avty_topic), "%s/availability", prefix);
        int ret = esp_mqtt_client_enqueue(client, avty_topic, "online", 6, 1, 1, true);
        if(ret < 0) {
            ESP_LOGE(TAG, "Failed to publish availability with error code %d", ret);
        }

        /* For each non-hidden channel: publish discovery, subscribe, publish state.
         * We snapshot each channel one at a time to avoid holding the lock during I/O. */
        config_lock();
        int count = g_config.channels_len;
        config_unlock();

        for (int i = 0; i < count; i++) {
            struct cosmo_channel_t ch;
            config_lock();
            if (i >= g_config.channels_len) { config_unlock(); break; }
            channel_deep_copy(&ch, &g_config.channels[i]);  /* one channel copy */
            config_unlock();
            publish_full_channel(client, &ch, prefix, ha_prefix, ha_disc);
            cosmo_channel_t_clear(&ch);
        }
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Disconnected from broker");
        g_mqtt_status = mqtt_status_t_disconnected;
        break;

    case MQTT_EVENT_DATA: {
        if (!event->topic || event->topic_len == 0) break;

        char prefix[64];
        config_lock();
        strlcpy(prefix, sstr_cstr(g_config.mqtt.mqtt_prefix), sizeof(prefix));
        config_unlock();

        size_t prefix_len = strlen(prefix);
        if ((size_t)event->topic_len <= prefix_len + 1) break;
        if (strncmp(event->topic, prefix, prefix_len) != 0) break;
        if (event->topic[prefix_len] != '/') break;

        const char *after_prefix = event->topic + prefix_len + 1;
        int after_len = event->topic_len - (int)prefix_len - 1;

        int last_slash = -1;
        for (int i = 0; i < after_len; i++)
            if (after_prefix[i] == '/') last_slash = i;
        if (last_slash < 0) break;

        char mqtt_name[64] = {0};
        int name_len = last_slash < 63 ? last_slash : 63;
        memcpy(mqtt_name, after_prefix, (size_t)name_len);

        const char *subtopic     = after_prefix + last_slash + 1;
        int         subtopic_len = after_len - last_slash - 1;

        struct cosmo_channel_t ch;
        cosmo_channel_t_init(&ch);
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
            if (sstr_compare_c(ch.proto, "2way") == 0) {
                int pos = atoi(data);
                if (pos < 0)   pos = 0;
                if (pos > 100) pos = 100;
                channel_send_cmd(ch.serial, COSMO_BTN_SET_POSITION, (uint8_t)pos);
                background_worker_inhibit_position_query();
            }
        }
        cosmo_channel_t_clear(&ch);
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
    g_mqtt_status = mqtt_status_t_unconfigured;
    memset(&s_active_cfg, 0, sizeof(s_active_cfg));
}

static void mqtt_do_init(const mqtt_active_cfg_t *cfg)
{
    if (!cfg->enabled || cfg->broker[0] == '\0') {
        g_mqtt_status = mqtt_status_t_unconfigured;
        return;
    }

    char uri[192];
    char avty_topic[96];
    config_lock();
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", cfg->broker, cfg->port);
    snprintf(avty_topic, sizeof(avty_topic), "%s/availability",
             sstr_cstr(g_config.mqtt.mqtt_prefix));
    config_unlock();

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri    = uri,
        .session.last_will     = {
            .topic             = avty_topic,
            .msg               = "offline",
            .msg_len           = 7,
            .qos               = 1,
            .retain            = 1,
        },
        .buffer = {
          .size = 4096,
          .out_size = 4096,
        },
        .session.keepalive     = 60,
        .task.stack_size       = 8000,
    };

    if (cfg->username[0]) {
        mqtt_cfg.credentials.username                  = cfg->username;
        mqtt_cfg.credentials.authentication.password   = cfg->password;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        g_mqtt_status = mqtt_status_t_disconnected;
        return;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    g_mqtt_status = mqtt_status_t_connecting;
    s_active_cfg  = *cfg;
    ESP_LOGI(TAG, "MQTT client started → %s", uri);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void mqtt_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    g_mqtt_status = mqtt_status_t_unconfigured;
    memset(&s_active_cfg, 0, sizeof(s_active_cfg));

    mqtt_active_cfg_t cfg = {0};
    config_lock();
    cfg.enabled = (bool)g_config.mqtt.enabled;
    cfg.port    = g_config.mqtt.port;
    strlcpy(cfg.broker,   sstr_cstr(g_config.mqtt.broker),   sizeof(cfg.broker));
    strlcpy(cfg.username, sstr_cstr(g_config.mqtt.username), sizeof(cfg.username));
    strlcpy(cfg.password, sstr_cstr(g_config.mqtt.password), sizeof(cfg.password));
    config_unlock();

    mqtt_do_init(&cfg);
}

void mqtt_apply_config(void)
{
    mqtt_active_cfg_t cfg = {0};
    config_lock();
    cfg.enabled = (bool)g_config.mqtt.enabled;
    cfg.port    = g_config.mqtt.port;
    strlcpy(cfg.broker,   sstr_cstr(g_config.mqtt.broker),   sizeof(cfg.broker));
    strlcpy(cfg.username, sstr_cstr(g_config.mqtt.username), sizeof(cfg.username));
    strlcpy(cfg.password, sstr_cstr(g_config.mqtt.password), sizeof(cfg.password));
    config_unlock();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = (memcmp(&s_active_cfg, &cfg, sizeof(cfg)) != 0);
    if (changed) {
        ESP_LOGI(TAG, "MQTT config changed, reinitialising");
        mqtt_do_deinit();
        mqtt_do_init(&cfg);
    }
    xSemaphoreGive(s_mutex);
}

void mqtt_publish_channel_update(const struct cosmo_channel_t *ch)
{
    if (!ch) return;
    if (ch->device_class == channel_device_class_t_hidden) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_mqtt_client_handle_t client = s_client;
    xSemaphoreGive(s_mutex);

    if (!client || g_mqtt_status != mqtt_status_t_connected) return;

    char prefix[64];
    config_lock();
    strlcpy(prefix, sstr_cstr(g_config.mqtt.mqtt_prefix), sizeof(prefix));
    config_unlock();

    publish_channel_state(client, ch, prefix);
}

void mqtt_publish_channel_created(const struct cosmo_channel_t *ch)
{
    if (!ch) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_mqtt_client_handle_t client = s_client;
    xSemaphoreGive(s_mutex);

    if (!client || g_mqtt_status != mqtt_status_t_connected) return;

    char prefix[64], ha_prefix[64];
    bool ha_disc;
    config_lock();
    strlcpy(prefix,    sstr_cstr(g_config.mqtt.mqtt_prefix), sizeof(prefix));
    strlcpy(ha_prefix, sstr_cstr(g_config.mqtt.ha_prefix),   sizeof(ha_prefix));
    ha_disc = (bool)g_config.mqtt.ha_discovery_enabled;
    config_unlock();

    publish_full_channel(client, ch, prefix, ha_prefix, ha_disc);
}

void mqtt_publish_channel_deleted(const struct cosmo_channel_t *ch)
{
    if (!ch) return;
    if (ch->device_class == channel_device_class_t_hidden) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_mqtt_client_handle_t client = s_client;
    xSemaphoreGive(s_mutex);

    if (!client || g_mqtt_status != mqtt_status_t_connected) return;

    char prefix[64], ha_prefix[64];
    bool ha_disc;
    config_lock();
    strlcpy(prefix,    sstr_cstr(g_config.mqtt.mqtt_prefix), sizeof(prefix));
    strlcpy(ha_prefix, sstr_cstr(g_config.mqtt.ha_prefix),   sizeof(ha_prefix));
    ha_disc = (bool)g_config.mqtt.ha_discovery_enabled;
    config_unlock();

    publish_empty_discovery(client, ch, prefix, ha_prefix, ha_disc);
}

void mqtt_republish_channel_discovery(const struct cosmo_channel_t *old_ch,
                                      const struct cosmo_channel_t *new_ch)
{
    if (!old_ch || !new_ch) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_mqtt_client_handle_t client = s_client;
    xSemaphoreGive(s_mutex);

    if (!client || g_mqtt_status != mqtt_status_t_connected) return;

    char prefix[64], ha_prefix[64];
    bool ha_disc;
    config_lock();
    strlcpy(prefix,    sstr_cstr(g_config.mqtt.mqtt_prefix), sizeof(prefix));
    strlcpy(ha_prefix, sstr_cstr(g_config.mqtt.ha_prefix),   sizeof(ha_prefix));
    ha_disc = (bool)g_config.mqtt.ha_discovery_enabled;
    config_unlock();

    if (ha_disc && old_ch->device_class != channel_device_class_t_hidden)
        publish_empty_discovery(client, old_ch, prefix, ha_prefix, ha_disc);

    publish_full_channel(client, new_ch, prefix, ha_prefix, ha_disc);
}
