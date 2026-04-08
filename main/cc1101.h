#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Fixed packet length (no length byte, no CRC). */
#define CC1101_PACKET_LEN 9

typedef struct {
    uint8_t data[CC1101_PACKET_LEN];
} cc1101_packet_t;

/**
 * @brief  Initialize the CC1101 radio, start the receive task.
 * @return ESP_OK on success, ESP_FAIL if the chip is not detected.
 */
esp_err_t cc1101_init(void);

/**
 * @brief  Transmit a packet (fills to CC1101_PACKET_LEN, zero-padded).
 */
esp_err_t cc1101_send(const uint8_t *data, uint8_t len);
