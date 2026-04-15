#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "cosmo/cosmo.h"
#include "esp_err.h"
#include "json.gen.h"
#include "config.h"

/* Initialise the channel subsystem (call after config_init). */
void      channel_init(void);

/* Create a new channel. name must be non-NULL. Returns ESP_ERR_NO_MEM if full. */
esp_err_t channel_create(const char *name, const char *proto,
                          int device_class, bool has_device_class,
                          const char *mqtt_name, bool has_mqtt_name);

/* Delete a channel by serial. Returns ESP_ERR_NOT_FOUND if absent. */
esp_err_t channel_delete(uint32_t serial);

/* Apply a parsed update message to a channel. Returns ESP_ERR_NOT_FOUND if absent. */
esp_err_t channel_update(uint32_t serial, const struct ws_update_channel_msg_t *msg);

/* Update channel state from a received radio packet. */
void      channel_update_from_packet(const cosmo_packet_t *pkt);

/* Send a command to a channel. */
esp_err_t channel_send_cmd(uint32_t serial, cosmo_cmd_t cmd, uint8_t extra_payload);

/* Send the full channel list to one WebSocket fd ({"cmd":"channels","payload":[...]}). */
void      channel_send_all(int fd);

/* Find a channel by mqtt_name. Returns ESP_ERR_NOT_FOUND if absent.
 * Deep-copies the channel into *out. Caller must cosmo_channel_t_clear(out) after use. */
esp_err_t channel_find_by_mqtt_name(const char *mqtt_name, struct cosmo_channel_t *out);

/* Deep-copy src into dst. dst must not be previously initialised (or must be cleared first).
 * Caller must cosmo_channel_t_clear(dst) when done with it. */
void channel_deep_copy(struct cosmo_channel_t *dst, const struct cosmo_channel_t *src);

