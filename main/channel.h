#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "cosmo/cosmo.h"
#include "esp_err.h"
#include "cJSON.h"

#define CHANNEL_MAX_COUNT 16

typedef enum {
    CHANNEL_STATE_UNKNOWN = 0,
    CHANNEL_STATE_CLOSING,
    CHANNEL_STATE_CLOSED,
    CHANNEL_STATE_OPENING,
    CHANNEL_STATE_OPEN,
    CHANNEL_STATE_COMFORT,
    CHANNEL_STATE_PARTIALLY_OPEN,
    CHANNEL_STATE_OBSTRUCTION,
    CHANNEL_STATE_IN_MOTION,
} cosmo_channel_state_t;

typedef struct {
    char                  name[32];
    cosmo_proto_t         proto;
    uint32_t              serial;
    uint16_t              counter;
    cosmo_channel_state_t state;
    int8_t                rssi;
    int64_t               last_seen_ts;
    int16_t               position;              /* 2-way only: 0-100 % open, -1 = unknown */
    bool                  reports_tilt_support;  /* set by firmware on RX; not user-configurable */
    bool                  force_tilt_support;    /* user override: always show tilt controls */

    /* Feedback behaviour */
    bool     bidirectional_feedback; /* accept RX feedback packets for this channel (default: true) */
    uint16_t feedback_timeout_s;     /* seconds before an in-motion state times out to PARTIALLY_OPEN (default: 120) */

    /* Runtime-only state (not meaningful across reboots) */
    bool is_state_optimistic; /* true when state is a local guess, not yet confirmed by the device */
} cosmo_channel_t;

/**
 * @brief Settings that can be changed after channel creation.
 *
 * Passed to channel_update().  All fields are applied atomically.
 */
typedef struct {
    char          name[32];
    cosmo_proto_t proto;
    bool          force_tilt_support;
    bool          bidirectional_feedback;
    uint16_t      feedback_timeout_s;
} cosmo_channel_settings_t;

void      channel_init(void);
esp_err_t channel_create(const char *name, cosmo_proto_t proto);
esp_err_t channel_delete(uint32_t serial);
esp_err_t channel_update(uint32_t serial, const cosmo_channel_settings_t *s);
void      channel_update_from_packet(const cosmo_packet_t *pkt);
esp_err_t channel_send_cmd(uint32_t serial, cosmo_cmd_t cmd, uint8_t extra_payload);
void      channel_send_all(int fd);       /* send {"cmd":"channels","payload":[...]} to one fd */

/**
 * @brief Serialise a channel to a cJSON object.
 *
 * Caller owns the returned object and must call cJSON_Delete() on it.
 * Used both for WebSocket broadcasts and for config persistence.
 */
cJSON    *channel_to_cjson(const cosmo_channel_t *ch);