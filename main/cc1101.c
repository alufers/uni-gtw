#include "cc1101.h"
#include "webserver.h"

#include <string.h>
#include <stdio.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cc1101";

/* ── Runtime pin/speed configuration ────────────────────────────────────── */

static int s_miso_gpio;
static int s_csn_gpio;

static spi_device_handle_t s_spi;

/* CC1101 appends 2 status bytes (RSSI + LQI/CRC) after each received packet */
#define CC1101_STATUS_BYTES 2

/* ── CC1101 register & command addresses ─────────────────────────────────── */

#define CC1101_IOCFG0   0x02
#define CC1101_FIFOTHR  0x03
#define CC1101_SYNC1    0x04
#define CC1101_SYNC0    0x05
#define CC1101_PKTLEN   0x06
#define CC1101_PKTCTRL1 0x07
#define CC1101_PKTCTRL0 0x08
#define CC1101_ADDR     0x09
#define CC1101_FSCTRL1  0x0B
#define CC1101_FSCTRL0  0x0C
#define CC1101_MDMCFG4  0x10
#define CC1101_MDMCFG3  0x11
#define CC1101_MDMCFG2  0x12
#define CC1101_MDMCFG1  0x13
#define CC1101_MDMCFG0  0x14

#define CC1101_DEVIATN  0x15
#define CC1101_MCSM0    0x18
#define CC1101_FOCCFG   0x19
#define CC1101_AGCCTRL2 0x1B
#define CC1101_AGCCTRL1 0x1C
#define CC1101_AGCCTRL0 0x1D
#define CC1101_WORCTRL  0x20
#define CC1101_FREQ2    0x0D
#define CC1101_FREQ1    0x0E
#define CC1101_FREQ0    0x0F

#define CC1101_PATABLE  0x3E
#define CC1101_TXFIFO   0x3F
#define CC1101_RXFIFO   0x3F

#define CC1101_CHANNR   0x0A

#define CC1101_PARTNUM   0x30
#define CC1101_VERSION   0x31
#define CC1101_FREQEST   0x32  /* Frequency offset estimate */
#define CC1101_MARCSTATE 0x35
#define CC1101_RXBYTES   0x3B

/* SPI access bits */
#define WRITE_BURST  0x40
#define READ_SINGLE  0x80
#define READ_BURST   0xC0

/* Command strobes */
#define CC1101_SRES  0x30
#define CC1101_SRX   0x34
#define CC1101_STX   0x35
#define CC1101_SIDLE 0x36
#define CC1101_SFRX  0x3A
#define CC1101_SFTX  0x3B

/* ── Register configuration table ───────────────────────────────────────── */
static const uint8_t cc1101_regs[] = {
    CC1101_IOCFG0,   0x06,  /* GDO0: asserts on sync, de-asserts at end of pkt */
    CC1101_FIFOTHR,  0x07,
    CC1101_PKTCTRL0, 0x00,  /* Fixed length, no CRC, no whitening              */
    CC1101_FSCTRL1,  0x06,
    CC1101_SYNC1,    0x2D,
    CC1101_SYNC0,    0xD4,
    CC1101_ADDR,     0x00,
    CC1101_PKTLEN,   0x09,  /* Fixed packet length = 9 bytes                   */
    CC1101_MDMCFG3,  0x83,
    CC1101_MDMCFG4,  0x36,  /* 400 kHz filter bandwidth                        */
    CC1101_MDMCFG2,  0x02,  /* 2-FSK, 16/16 sync word                          */
    CC1101_MDMCFG1,  0x22,  /* Preamble: 4 bytes                               */
    CC1101_MDMCFG0,  0xF8,  /* Channel spacing: 199.951 kHz                    */
    CC1101_DEVIATN,  0x50,
    CC1101_MCSM0,    0x18,
    CC1101_FOCCFG,   0x16,
    CC1101_AGCCTRL2, 0x43,
    CC1101_AGCCTRL1, 0x40,
    CC1101_AGCCTRL0, 0x91,
    CC1101_WORCTRL,  0xFB,
    CC1101_FREQ2,    0x21,  /* Base frequency: 868.149902 MHz                   */
    CC1101_FREQ1,    0x63,
    CC1101_FREQ0,    0xF0,
    0, 0,  /* terminator */
};

static const uint8_t cc1101_patable[8] = {
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* ── Low-level SPI helpers ───────────────────────────────────────────────── */

static inline void cc1101_select(void)   { gpio_set_level(s_csn_gpio, 0); }
static inline void cc1101_deselect(void) { gpio_set_level(s_csn_gpio, 1); }
static inline void wait_miso(void)       { while (gpio_get_level(s_miso_gpio) > 0) {} }

static uint8_t spi_xfer(uint8_t byte)
{
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &byte,
        .flags     = SPI_TRANS_USE_RXDATA,
    };
    spi_device_polling_transmit(s_spi, &t);
    return t.rx_data[0];
}

static void cc1101_write_reg(uint8_t addr, uint8_t val)
{
    cc1101_select();
    wait_miso();
    spi_xfer(addr);
    spi_xfer(val);
    cc1101_deselect();
}

static uint8_t cc1101_read_reg(uint8_t addr, uint8_t type)
{
    cc1101_select();
    wait_miso();
    spi_xfer(addr | type);
    uint8_t val = spi_xfer(0x00);
    cc1101_deselect();
    return val;
}

static void cc1101_write_burst_internal(uint8_t addr, const uint8_t *buf, uint8_t len)
{
    cc1101_select();
    wait_miso();
    spi_xfer(addr | WRITE_BURST);
    for (uint8_t i = 0; i < len; i++)
        spi_xfer(buf[i]);
    cc1101_deselect();
}

static void cc1101_read_burst_internal(uint8_t addr, uint8_t *buf, uint8_t len)
{
    cc1101_select();
    wait_miso();
    spi_xfer(addr | READ_BURST);
    for (uint8_t i = 0; i < len; i++)
        buf[i] = spi_xfer(0x00);
    cc1101_deselect();
}

static void cc1101_strobe(uint8_t cmd)
{
    cc1101_select();
    wait_miso();
    spi_xfer(cmd);
    cc1101_deselect();
}

#define cc1101_read_status(addr) cc1101_read_reg(addr, READ_BURST)

/* ── Chip reset & configuration ──────────────────────────────────────────── */

static void cc1101_reset(void)
{
    cc1101_deselect();
    esp_rom_delay_us(5);
    cc1101_select();
    esp_rom_delay_us(10);
    cc1101_deselect();
    esp_rom_delay_us(41);
    cc1101_select();
    wait_miso();
    spi_xfer(CC1101_SRES);
    wait_miso();
    cc1101_deselect();
}

static void cc1101_apply_config(void)
{
    for (int i = 0; cc1101_regs[i] || cc1101_regs[i + 1]; i += 2)
        cc1101_write_reg(cc1101_regs[i], cc1101_regs[i + 1]);

    cc1101_write_burst_internal(CC1101_PATABLE, cc1101_patable, 8);
}

/* ── radio_ops_t implementation ──────────────────────────────────────────── */

static esp_err_t cc1101_ops_init(const radio_hw_cfg_t *hw, spi_host_device_t host)
{
    s_miso_gpio = hw->gpio_miso;
    s_csn_gpio  = hw->gpio_csn;

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = hw->spi_freq_hz,
        .mode           = 0,
        .spics_io_num   = -1,
        .queue_size     = 4,
        .flags          = SPI_DEVICE_NO_DUMMY,
    };
    esp_err_t err = spi_bus_add_device(host, &devcfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    /* CSN GPIO */
    gpio_reset_pin(s_csn_gpio);
    gpio_set_direction(s_csn_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(s_csn_gpio, 1);

    /* GDO0 — input with NEGEDGE interrupt type; ISR installed by radio.c */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << hw->gpio_gdo0,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);

    cc1101_reset();

    uint8_t partnum = cc1101_read_status(CC1101_PARTNUM);
    uint8_t version = cc1101_read_status(CC1101_VERSION);
    gtw_console_log("CC1101 PARTNUM=0x%02X VERSION=0x%02X", partnum, version);

    cc1101_apply_config();

    partnum = cc1101_read_status(CC1101_PARTNUM);
    version = cc1101_read_status(CC1101_VERSION);

    if (partnum != 0x00 || version == 0x00) {
        gtw_console_log("CC1101 not found! PARTNUM=0x%02X VERSION=0x%02X",
                        partnum, version);
        ESP_LOGE(TAG, "CC1101 not found");
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
        gpio_reset_pin(s_csn_gpio);
        return ESP_FAIL;
    }
    gtw_console_log("CC1101 init OK  PARTNUM=0x%02X VERSION=0x%02X",
                    partnum, version);

    uint8_t ptable[8];
    cc1101_read_burst_internal(CC1101_PATABLE, ptable, 8);
    gtw_console_log("PATABLE: %02X %02X %02X %02X %02X %02X %02X %02X",
                    ptable[0], ptable[1], ptable[2], ptable[3],
                    ptable[4], ptable[5], ptable[6], ptable[7]);

    return ESP_OK;
}

static void cc1101_ops_deinit(void)
{
    if (!s_spi) return;
    cc1101_strobe(CC1101_SIDLE);
    spi_bus_remove_device(s_spi);
    s_spi = NULL;
    gpio_reset_pin(s_csn_gpio);
}

static void cc1101_ops_enter_idle(void)
{
    cc1101_strobe(CC1101_SIDLE);
}

static void cc1101_ops_enter_rx(void)
{
    /* Flush RX FIFO first: safe after consuming a packet or returning from TX */
    cc1101_strobe(CC1101_SFRX);
    cc1101_strobe(CC1101_SRX);
}

static void cc1101_ops_set_channel(uint8_t ch)
{
    cc1101_write_reg(CC1101_CHANNR, ch);
}

static esp_err_t cc1101_ops_handle_rx_irq(uint8_t *buf, uint8_t len,
                                           int8_t *rssi_dbm,
                                           int16_t *freq_off_khz)
{
    uint8_t rxbytes = cc1101_read_status(CC1101_RXBYTES);

    if ((rxbytes & 0x80) || (rxbytes & 0x7F) < len + CC1101_STATUS_BYTES) {
        ESP_LOGW(TAG, "RX FIFO issue (RXBYTES=0x%02X), flushing", rxbytes);
        cc1101_strobe(CC1101_SIDLE);
        cc1101_strobe(CC1101_SFRX);
        cc1101_write_reg(CC1101_CHANNR, 0);
        cc1101_strobe(CC1101_SRX);
        return ESP_FAIL;
    }

    /* Read packet bytes + 2 CC1101 status bytes */
    uint8_t tmp[len + CC1101_STATUS_BYTES];
    cc1101_read_burst_internal(CC1101_RXFIFO, tmp, len + CC1101_STATUS_BYTES);

    int8_t  freqest    = (int8_t)cc1101_read_status(CC1101_FREQEST);
    *freq_off_khz = (int16_t)(((int32_t)freqest * 26000) / 16384);

    /* Re-enter RX */
    cc1101_write_reg(CC1101_CHANNR, 0);
    cc1101_strobe(CC1101_SFRX);
    cc1101_strobe(CC1101_SRX);

    /* Convert RSSI from CC1101 raw encoding */
    uint8_t rssi_raw = tmp[len];
    *rssi_dbm = (rssi_raw >= 128)
                ? ((int8_t)(rssi_raw - 256) / 2) - 74
                : (int8_t)(rssi_raw / 2) - 74;

    memcpy(buf, tmp, len);
    return ESP_OK;
}

static esp_err_t cc1101_ops_transmit(const uint8_t *data, uint8_t len)
{
    /* caller has already called enter_idle() and set_channel() */
    cc1101_strobe(CC1101_SFTX);
    cc1101_strobe(CC1101_SFRX);
    cc1101_write_burst_internal(CC1101_TXFIFO, data, len);
    cc1101_strobe(CC1101_STX);

    /* Wait for TX to complete (MARCSTATE leaves 0x13 = TX) */
    for (int w = 0; w < 30; w++) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (cc1101_read_status(CC1101_MARCSTATE) != 0x13)
            break;
    }
    return ESP_OK;
}

static const radio_ops_t cc1101_ops = {
    .init          = cc1101_ops_init,
    .deinit        = cc1101_ops_deinit,
    .enter_idle    = cc1101_ops_enter_idle,
    .enter_rx      = cc1101_ops_enter_rx,
    .set_channel   = cc1101_ops_set_channel,
    .handle_rx_irq = cc1101_ops_handle_rx_irq,
    .transmit      = cc1101_ops_transmit,
    .irq_edge      = GPIO_INTR_NEGEDGE,
};

const radio_ops_t *cc1101_get_ops(void)
{
    return &cc1101_ops;
}
