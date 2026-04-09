#pragma once

#include <stdint.h>
#include "esp_err.h"

/* GPIO exposed so radio.c can install the ISR handler */
#define CC1101_GDO0_GPIO 15

/**
 * @brief Initialise the CC1101: SPI bus, CSN GPIO, GDO0 GPIO, chip reset and
 *        register configuration.  Does NOT create any task or install an ISR.
 */
esp_err_t cc1101_init(void);

/* ── State transitions ───────────────────────────────────────────────────── */
void cc1101_enter_idle(void);
void cc1101_enter_rx(void);
void cc1101_start_tx(void);
void cc1101_flush_rx(void);
void cc1101_flush_tx(void);

/* ── Status ──────────────────────────────────────────────────────────────── */
uint8_t cc1101_get_rxbytes(void);
uint8_t cc1101_get_marcstate(void);
int8_t  cc1101_get_freqest(void);  /* FREQEST (0x32): signed freq offset raw value */

/* ── Channel ─────────────────────────────────────────────────────────────── */
void cc1101_set_channel(uint8_t channel); /* write CHANNR register; must be in IDLE */

/* ── FIFO access ─────────────────────────────────────────────────────────── */
void cc1101_read_rxfifo(uint8_t *buf, uint8_t len);
void cc1101_write_txfifo(const uint8_t *buf, uint8_t len);
