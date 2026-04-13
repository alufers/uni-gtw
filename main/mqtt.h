#pragma once

#include "esp_err.h"
#include "channel.h"

typedef enum {
    MQTT_STATUS_UNCONFIGURED = 0,
    MQTT_STATUS_CONNECTING,
    MQTT_STATUS_CONNECTED,
    MQTT_STATUS_DISCONNECTED,
} mqtt_status_t;

extern mqtt_status_t g_mqtt_status;

/* Called once from app_main after channel_init(). */
void mqtt_init(void);

/* Called from webserver apply_settings to react to config changes at runtime.
 * Safe to call from any task (NOT from an MQTT event handler). */
void mqtt_apply_config(void);

/* Called from channel.c after every state/position/rssi change.
 * No-op if MQTT is not connected or channel is HIDDEN. Thread-safe. */
void mqtt_publish_channel_update(const cosmo_channel_t *ch);

/* Called from channel.c after a channel is created.
 * Publishes HA discovery, subscribes to command topics, and publishes state.
 * No-op if MQTT is not connected. Thread-safe. */
void mqtt_publish_channel_created(const cosmo_channel_t *ch);

/* Called from channel.c after a channel is deleted.
 * Clears the HA discovery config for the channel. Thread-safe. */
void mqtt_publish_channel_deleted(const cosmo_channel_t *ch);

/* Called from channel.c when discovery-relevant fields (name, device_class,
 * mqtt_name) change.  Clears the old discovery entry, re-publishes new
 * discovery, and re-subscribes to command topics.  Thread-safe. */
void mqtt_republish_channel_discovery(const cosmo_channel_t *old_ch,
                                      const cosmo_channel_t *new_ch);
