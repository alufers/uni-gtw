#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    int gpio_miso;
    int gpio_mosi;
    int gpio_sck;
    int gpio_csn;
    int gpio_gdo0;  /* stored for use by radio.c ISR registration */
    int spi_freq_hz;
} cc1101_config_t;

/**
 * @brief Initialise the CC1101: SPI bus, CSN GPIO, GDO0 GPIO, chip reset and
 *        register configuration.  Does NOT create any task or install an ISR.
 */
esp_err_t cc1101_init(const cc1101_config_t *cfg);

/* ── State transitions ───────────────────────────────────────────────────── */
void cc1101_enter_idle(void);
void cc1101_enter_rx(void);
void cc1101_start_tx(void);
void cc1101_flush_rx(void);
void cc1101_flush_tx(void);

/* ── Status ──────────────────────────────────────────────────────────────── */
uint8_t cc1101_get_rxbytes(void);
uint8_t cc1101_get_marcstate(void);
int8_t  cc1101_get_freqest(void);

/* ── Channel ─────────────────────────────────────────────────────────────── */
void cc1101_set_channel(uint8_t channel);

/* ── FIFO access ─────────────────────────────────────────────────────────── */
void cc1101_read_rxfifo(uint8_t *buf, uint8_t len);
void cc1101_write_txfifo(const uint8_t *buf, uint8_t len);
