#pragma once

#include <stdint.h>
#include "cosmo/cosmo.h"
#include "esp_err.h"

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
} cosmo_channel_state_t;

typedef struct {
    char                  name[32];
    cosmo_proto_t         proto;
    uint32_t              serial;
    uint16_t              counter;
    cosmo_channel_state_t state;
    int8_t                rssi;
    int64_t               last_seen_ts;
} cosmo_channel_t;

void      channel_init(void);
esp_err_t channel_create(const char *name, cosmo_proto_t proto);
esp_err_t channel_delete(uint32_t serial);
void      channel_update_from_packet(const cosmo_packet_t *pkt);
esp_err_t channel_send_cmd(uint32_t serial, cosmo_cmd_t cmd);
void      channel_send_all(int fd); /* send {"cmd":"channels","payload":[...]} to one fd */
