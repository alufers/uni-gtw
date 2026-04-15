#pragma once

#include "esp_err.h"
#include "json.gen.h"

/* Current MQTT connection status (mqtt_status_t enum from json.gen.h). */
extern int g_mqtt_status; /* enum mqtt_status_t values */

/* Called once from app_main after channel_init(). */
void mqtt_init(void);

/* Called from webserver apply_settings to react to config changes at runtime.
 * Safe to call from any task (NOT from an MQTT event handler). */
void mqtt_apply_config(void);

/* Called from channel.c after every state/position/rssi change.
 * No-op if MQTT is not connected or channel is HIDDEN. Thread-safe.
 * ch must be a deep copy (not a pointer into g_config). */
void mqtt_publish_channel_update(const struct cosmo_channel_t *ch);

/* Called from channel.c after a channel is created.
 * Publishes HA discovery, subscribes to command topics, and publishes state.
 * No-op if MQTT is not connected. Thread-safe.
 * ch must be a deep copy (not a pointer into g_config). */
void mqtt_publish_channel_created(const struct cosmo_channel_t *ch);

/* Called from channel.c after a channel is deleted.
 * Clears the HA discovery config for the channel. Thread-safe.
 * ch must be a deep copy (not a pointer into g_config). */
void mqtt_publish_channel_deleted(const struct cosmo_channel_t *ch);

/* Called from channel.c when discovery-relevant fields change.
 * Clears the old discovery entry, re-publishes new discovery and re-subscribes.
 * Both old_ch and new_ch must be deep copies. Thread-safe. */
void mqtt_republish_channel_discovery(const struct cosmo_channel_t *old_ch,
                                      const struct cosmo_channel_t *new_ch);
