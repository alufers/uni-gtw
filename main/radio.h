#pragma once

#include "cosmo.h"
#include "esp_err.h"

typedef enum {
    RADIO_STATE_NOT_CONFIGURED = 0,
    RADIO_STATE_ERROR,
    RADIO_STATE_OK,
} radio_state_t;

/**
 * @brief Initialise the radio subsystem.
 *
 * Calls cc1101_init(), creates the event queue and radio task, installs the
 * GDO0 ISR, then enters RX mode.  Call once from app_main after
 * webserver_early_init().
 */
esp_err_t radio_init(void);

/**
 * @brief Queue a packet for transmission.
 *
 * The radio task will idle the chip, transmit the packet (pkt->repeat + 1)
 * times with 50 ms gaps between transmissions, then return to RX mode.
 * Safe to call from any task.
 *
 * @param pkt  Packet to transmit (pkt->repeat controls extra repetitions).
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if the queue is full.
 */
esp_err_t radio_request_tx(const cosmo_packet_t *pkt);
