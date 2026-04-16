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
 * @brief Compare running radio config against the current persisted config and
 *        reinitialize hardware only if something changed (or enable/disable).
 *
 * Returns the same codes as radio_init(): ESP_OK, ESP_ERR_NOT_SUPPORTED
 * (radio disabled), or an error.  Safe to call from any task.
 */
esp_err_t radio_apply_config(void);

/**
 * @brief Return the current radio state (NOT_CONFIGURED, ERROR, or OK).
 *
 * Thread-safe; may be called from any task.
 */
radio_state_t radio_get_state(void);

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
