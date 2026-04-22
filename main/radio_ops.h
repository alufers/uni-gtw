#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

/* ── Hardware configuration snapshot ────────────────────────────────────── */
/* Passed to radio_ops_t::init(); radio.c owns the SPI bus, drivers own only
 * the SPI device (spi_bus_add_device / spi_bus_remove_device).             */

typedef struct {
    bool           enabled;
    int            type;      /* radio_type_t value; int to avoid generated-header dep */
    int            gpio_miso;
    int            gpio_mosi;
    int            gpio_sck;
    int            gpio_csn;
    int            gpio_gdo0;   /* CC1101: GDO0;  SX1262: DIO1 (IRQ) */
    int            gpio_rst;    /* SX1262 RST  (-1 for CC1101)        */
    int            gpio_busy;   /* SX1262 BUSY (-1 for CC1101)        */
    int            spi_freq_hz;
} radio_hw_cfg_t;

/* ── Driver ops interface ────────────────────────────────────────────────── */

typedef struct radio_ops_s {
    /* Initialise the driver.  The SPI bus identified by `host` has already
     * been initialised by radio.c.  The driver adds its own device. */
    esp_err_t       (*init)(const radio_hw_cfg_t *cfg, spi_host_device_t host);

    /* Release the SPI device (spi_bus_remove_device).  radio.c calls
     * spi_bus_free() afterwards. */
    void            (*deinit)(void);

    /* State transitions */
    void            (*enter_idle)(void);
    /* Re-enter RX on the RX channel (channel 0).  For CC1101 also flushes
     * the RX FIFO first so stale bytes from a previous TX are discarded. */
    void            (*enter_rx)(void);
    void            (*set_channel)(uint8_t ch);

    /* Called from the radio task when the IRQ GPIO fires.
     * Reads exactly `len` packet bytes into `buf`, sets *rssi_dbm and
     * *freq_off_khz.  On error the driver resets to RX internally and
     * returns ESP_FAIL; radio.c discards the call silently.              */
    esp_err_t       (*handle_rx_irq)(uint8_t *buf, uint8_t len,
                                     int8_t  *rssi_dbm,
                                     int16_t *freq_off_khz);

    /* Transmit one raw packet synchronously.  The caller has already called
     * enter_idle() and set_channel().  The driver leaves the chip idle
     * (not in RX) after returning.                                       */
    esp_err_t       (*transmit)(const uint8_t *data, uint8_t len);

    /* Edge for gpio_isr_handler_add on gpio_gdo0 / DIO1 */
    gpio_int_type_t irq_edge;
} radio_ops_t;
