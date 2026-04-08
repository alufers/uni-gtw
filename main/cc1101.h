#pragma once

#include "cosmo.h"   /* cosmo_raw_packet_t, COSMO_RAW_PACKET_LEN */

/* CC1101_PACKET_LEN kept for internal cc1101.c use */
#define CC1101_PACKET_LEN COSMO_RAW_PACKET_LEN

esp_err_t cc1101_init(void);
esp_err_t cc1101_send(const uint8_t *data, uint8_t len);
