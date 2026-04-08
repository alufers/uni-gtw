#include "cc1101.h"
#include "webserver.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "cc1101";

/* ── Pin definitions ─────────────────────────────────────────────────────── */

#define MISO_GPIO  19
#define SCK_GPIO   18
#define MOSI_GPIO  23
#define CSN_GPIO    5
#define GDO0_GPIO  15

/* ── SPI ─────────────────────────────────────────────────────────────────── */

#define SPI_HOST_ID  SPI2_HOST
#define SPI_FREQ_HZ  500000

static spi_device_handle_t s_spi;

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
#define CC1101_MDMCFG4  0x10
#define CC1101_MDMCFG3  0x11
#define CC1101_MDMCFG2  0x12
#define CC1101_DEVIATN  0x15
#define CC1101_MCSM0    0x18
#define CC1101_FOCCFG   0x19
#define CC1101_AGCCTRL2 0x1B
#define CC1101_AGCCTRL1 0x1C
#define CC1101_AGCCTRL0 0x1D
#define CC1101_WORCTRL  0x20
#define CC1101_FREQ2			 	0x0D	// Frequency Control Word, High Byte
#define CC1101_FREQ1				0x0E	// Frequency Control Word, Middle Byte
#define CC1101_FREQ0				0x0F	// Frequency Control Word, Low Byte

#define CC1101_PATABLE  0x3E
#define CC1101_TXFIFO   0x3F
#define CC1101_RXFIFO   0x3F

#define CC1101_PARTNUM   0x30   /* status registers */
#define CC1101_VERSION   0x31
#define CC1101_MARCSTATE 0x35
#define CC1101_RXBYTES   0x3B
#define CC1101_TXBYTES   0x3A

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
/* Format: {register, value} pairs, terminated by {0, 0}. */
static const uint8_t cc1101_regs[] = {
    CC1101_IOCFG0,   0x06,  /* GDO0: asserts on sync, de-asserts at end of pkt */
    CC1101_FIFOTHR,  0x07,  /* RX/TX FIFO threshold */
    CC1101_PKTCTRL0, 0x00,  /* Fixed length, no CRC, no whitening */
    CC1101_FSCTRL1,  0x06,
    CC1101_SYNC1,    0x2D,
    CC1101_SYNC0,    0xD4,
    CC1101_ADDR,     0x00,
    CC1101_PKTLEN,   0x09,  /* Fixed packet length = 9 bytes */
    CC1101_MDMCFG3,  0x83,
    CC1101_MDMCFG4,  0x56,
    CC1101_MDMCFG2,  0x02,
    CC1101_DEVIATN,  0x50,
    CC1101_MCSM0,    0x18,
    CC1101_FOCCFG,   0x16,
    CC1101_AGCCTRL2, 0x43,
    CC1101_AGCCTRL1, 0x40,
    CC1101_AGCCTRL0, 0x91,
    CC1101_WORCTRL,  0xFB,
    CC1101_FREQ2,
       0x21, // Base frequency: 868.199890 MHz
       CC1101_FREQ1,
       0x64,
       CC1101_FREQ0,
       0x6E,


    0, 0,  /* terminator */
};

static const uint8_t cc1101_patable[8] = {
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* ── Receive task ────────────────────────────────────────────────────────── */

static TaskHandle_t s_rx_task_handle = NULL;

/* ── Low-level SPI helpers ───────────────────────────────────────────────── */

#define cc1101_select()   gpio_set_level(CSN_GPIO, 0)
#define cc1101_deselect() gpio_set_level(CSN_GPIO, 1)
#define wait_miso()       while (gpio_get_level(MISO_GPIO) > 0) {}

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

static void cc1101_write_burst(uint8_t addr, const uint8_t *buf, uint8_t len)
{
    cc1101_select();
    wait_miso();
    spi_xfer(addr | WRITE_BURST);
    for (uint8_t i = 0; i < len; i++)
        spi_xfer(buf[i]);
    cc1101_deselect();
}

static void cc1101_read_burst(uint8_t addr, uint8_t *buf, uint8_t len)
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
#define cc1101_read_config(addr) cc1101_read_reg(addr, READ_SINGLE)

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
    /* Write register table (reg+val pairs until {0,0}). */
    for (int i = 0; cc1101_regs[i] || cc1101_regs[i + 1]; i += 2)
        cc1101_write_reg(cc1101_regs[i], cc1101_regs[i + 1]);

    /* Write PATABLE. */
    cc1101_write_burst(CC1101_PATABLE, cc1101_patable, 8);
}

/* ── GDO0 ISR ────────────────────────────────────────────────────────────── */

static void IRAM_ATTR gdo0_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_rx_task_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ── Receive task ────────────────────────────────────────────────────────── */

static void cc1101_rx_task(void *arg)
{
    while (1) {
        /* Wait for GDO0 falling edge (end of received packet). */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint8_t rxbytes = cc1101_read_status(CC1101_RXBYTES);

        /* Check for RX FIFO overflow (bit 7) or insufficient data. */
        if ((rxbytes & 0x80) || (rxbytes & 0x7F) < CC1101_PACKET_LEN+2) {
            ESP_LOGW(TAG, "RX issue: RXBYTES=0x%02X, flushing", rxbytes);
            cc1101_strobe(CC1101_SIDLE);
            cc1101_strobe(CC1101_SFRX);
            cc1101_strobe(CC1101_SRX);
            continue;
        }

        uint8_t tmp_buf[CC1101_PACKET_LEN + COSMO_STATUS_BYTES];
        cc1101_read_burst(CC1101_RXFIFO, tmp_buf, sizeof(tmp_buf));

        /* Re-enter RX immediately to avoid missing next packet. */
        cc1101_strobe(CC1101_SRX);

        /* Extract RSSI from first status byte (CC1101 datasheet §10.14.3) */
        uint8_t rssi_raw = tmp_buf[CC1101_PACKET_LEN];
        int8_t rssi_dbm = (rssi_raw >= 128)
                          ? ((int8_t)(rssi_raw - 256) / 2) - 74
                          : (int8_t)(rssi_raw / 2) - 74;

        cosmo_raw_packet_t raw;
        memcpy(raw.data, tmp_buf, CC1101_PACKET_LEN);
        raw.rssi = rssi_dbm;

        cosmo_packet_t pkt;
        if (cosmo_decode(&raw, &pkt) == ESP_OK) {
            cosmo_packet_log(&pkt);
        } else {
            /* Log raw hex for undecodable packets */
            char hex[CC1101_PACKET_LEN * 3 + 1];
            for (int i = 0; i < CC1101_PACKET_LEN; i++)
                snprintf(hex + i * 3, 4, "%02X ", raw.data[i]);
            hex[CC1101_PACKET_LEN * 3 - 1] = '\0';
            gtw_console_log("RX (bad pkt) hex: %s rssi=%d dBm", hex, (int)rssi_dbm);

            char bin[CC1101_PACKET_LEN * 9 + 1];
            for (int i = 0; i < CC1101_PACKET_LEN; i++) {
                for (int b = 7; b >= 0; b--)
                    bin[i * 9 + (7 - b)] = ((raw.data[i] >> b) & 1) ? '1' : '0';
                bin[i * 9 + 8] = (i < CC1101_PACKET_LEN - 1) ? ' ' : '\0';
            }
            gtw_console_log("RX (bad pkt) bin: %s rssi=%d dBm", bin, (int)rssi_dbm);
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t cc1101_init(void)
{
    /* ── SPI bus ── */
    spi_bus_config_t buscfg = {
        .sclk_io_num   = SCK_GPIO,
        .mosi_io_num   = MOSI_GPIO,
        .miso_io_num   = MISO_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_ID, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_FREQ_HZ,
        .mode           = 0,
        .spics_io_num   = -1,    /* manual CS via GPIO */
        .queue_size     = 4,
        .flags          = SPI_DEVICE_NO_DUMMY,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI_HOST_ID, &devcfg, &s_spi));

    /* ── CSN GPIO ── */
    gpio_reset_pin(CSN_GPIO);
    gpio_set_direction(CSN_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CSN_GPIO, 1);

    /* ── GDO0 GPIO + interrupt ── */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GDO0_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,   /* de-assert = end of packet */
    };
    gpio_config(&io);

    /* ── Reset & configure ── */
    cc1101_reset();

    {
        uint8_t partnum = cc1101_read_status(CC1101_PARTNUM);
        uint8_t version = cc1101_read_status(CC1101_VERSION);
        gtw_console_log("CC1101 PARTNUM=0x%02X VERSION=0x%02X", partnum, version);
    }

    cc1101_apply_config();

    /* ── Verify chip identity ── */
    uint8_t partnum = cc1101_read_status(CC1101_PARTNUM);
    uint8_t version = cc1101_read_status(CC1101_VERSION);
    gtw_console_log("CC1101 PARTNUM=0x%02X VERSION=0x%02X", partnum, version);

    if (partnum != 0x00 || version == 0x00) {
        gtw_console_log("CC1101 not found! (expected PARTNUM=0x00 VERSION!=0x14)");
        ESP_LOGE(TAG, "CC1101 not found");
        return ESP_FAIL;
    }
    gtw_console_log("CC1101 initialized OK, entering RX");

    /* ── Read back PATABLE for verification ── */
    uint8_t ptable[8];
    cc1101_read_burst(CC1101_PATABLE, ptable, 8);
    gtw_console_log("PATABLE: %02X %02X %02X %02X %02X %02X %02X %02X",
                    ptable[0], ptable[1], ptable[2], ptable[3],
                    ptable[4], ptable[5], ptable[6], ptable[7]);

    /* ── Start receive task ── */
    xTaskCreate(cc1101_rx_task, "cc1101_rx", 4096, NULL, 10, &s_rx_task_handle);

    /* ── Enable GDO0 interrupt ── */
    gpio_install_isr_service(0);
    gpio_isr_handler_add(GDO0_GPIO, gdo0_isr, NULL);

    /* ── Enter RX mode ── */
    cc1101_strobe(CC1101_SRX);

    return ESP_OK;
}

esp_err_t cc1101_send(const uint8_t *data, uint8_t len)
{
    if (len > CC1101_PACKET_LEN) len = CC1101_PACKET_LEN;

    uint8_t buf[CC1101_PACKET_LEN] = {0};
    memcpy(buf, data, len);

    cc1101_strobe(CC1101_SIDLE);
    cc1101_strobe(CC1101_SFTX);
    cc1101_write_burst(CC1101_TXFIFO, buf, CC1101_PACKET_LEN);
    cc1101_strobe(CC1101_STX);

    return ESP_OK;
}
