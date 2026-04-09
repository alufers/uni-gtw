#include "cc1101.h"
#include "webserver.h"

#include <string.h>
#include <stdio.h>

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
/* GDO0_GPIO is in cc1101.h (CC1101_GDO0_GPIO = 15) */

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
#define CC1101_FSCTRL0  0x0C
#define CC1101_MDMCFG4  0x10
#define CC1101_MDMCFG3  0x11
#define CC1101_MDMCFG2  0x12
#define CC1101_MDMCFG1  0x13
#define CC1101_MDMCFG0	0x14

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

#define CC1101_CHANNR	0x0A

#define CC1101_PARTNUM   0x30
#define CC1101_VERSION   0x31
#define CC1101_FREQEST   0x32  /* Frequency offset estimate (2's complement, FXTAL/2^14 resolution) */
#define CC1101_MARCSTATE 0x35
#define CC1101_RXBYTES   0x3B
#define CC1101_TXBYTES   0x3A

#define CC1101_FSCAL3				0x23	// Frequency Synthesizer Calibration
#define CC1101_FSCAL2				0x24	// Frequency Synthesizer Calibration
#define CC1101_FSCAL1				0x25	// Frequency Synthesizer Calibration
#define CC1101_FSCAL0				0x26	// Frequency Synthesizer Calibration

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
    CC1101_PKTCTRL0, 0x00,  /* Fixed length, no CRC, no whitening */
    CC1101_FSCTRL1,  0x06,
    CC1101_SYNC1,    0x2D,
    CC1101_SYNC0,    0xD4,
    CC1101_ADDR,     0x00,
    CC1101_PKTLEN,   0x09,  /* Fixed packet length = 9 bytes */
    CC1101_MDMCFG3,  0x83,
    CC1101_MDMCFG4,  0x56,
    CC1101_MDMCFG2,  0x02,
    CC1101_MDMCFG1,  0x22, // Preamble: 4 bytes
    CC1101_MDMCFG0,  0xF8, // Channel spacing: 199.951 kHz
    CC1101_DEVIATN,  0x50,
    CC1101_MCSM0,    0x18,
    CC1101_FOCCFG,   0x16,
    CC1101_AGCCTRL2, 0x43,
    CC1101_AGCCTRL1, 0x40,
    CC1101_AGCCTRL0, 0x91,
    CC1101_WORCTRL,  0xFB,

        CC1101_FREQ2,
        0x21, // Base frequency: 868.000336 MHz
        CC1101_FREQ1,
        0x62,
        CC1101_FREQ0,
        0x77,

        CC1101_FSCTRL0, 0x2c, // offset by 70khz



        CC1101_FSCAL3,      0xE9,
        CC1101_FSCAL2,      0x2A,
        CC1101_FSCAL1,      0x00,
        CC1101_FSCAL0,      0x1F,


    0, 0,  /* terminator */
};

static const uint8_t cc1101_patable[8] = {
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

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

/* ── Public state-transition and FIFO functions ──────────────────────────── */

void cc1101_enter_idle(void)  { cc1101_strobe(CC1101_SIDLE); }
void cc1101_enter_rx(void)    { cc1101_strobe(CC1101_SRX);   }
void cc1101_set_channel(uint8_t channel) { cc1101_write_reg(CC1101_CHANNR, channel); }
void cc1101_start_tx(void)    { cc1101_strobe(CC1101_STX);   }
void cc1101_flush_rx(void)    { cc1101_strobe(CC1101_SFRX);  }
void cc1101_flush_tx(void)    { cc1101_strobe(CC1101_SFTX);  }

uint8_t cc1101_get_rxbytes(void)   { return cc1101_read_status(CC1101_RXBYTES);   }
uint8_t cc1101_get_marcstate(void) { return cc1101_read_status(CC1101_MARCSTATE); }
int8_t  cc1101_get_freqest(void)   { return (int8_t)cc1101_read_status(CC1101_FREQEST); }

void cc1101_read_rxfifo(uint8_t *buf, uint8_t len)
{
    cc1101_read_burst_internal(CC1101_RXFIFO, buf, len);
}

void cc1101_write_txfifo(const uint8_t *buf, uint8_t len)
{
    cc1101_write_burst_internal(CC1101_TXFIFO, buf, len);
}

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
        .spics_io_num   = -1,
        .queue_size     = 4,
        .flags          = SPI_DEVICE_NO_DUMMY,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI_HOST_ID, &devcfg, &s_spi));

    /* ── CSN GPIO ── */
    gpio_reset_pin(CSN_GPIO);
    gpio_set_direction(CSN_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CSN_GPIO, 1);

    /* ── GDO0 GPIO (configured as input with NEGEDGE interrupt type;
     *    the ISR handler is installed by radio.c via gpio_isr_handler_add) ── */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CC1101_GDO0_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);

    /* ── Reset & configure ── */
    cc1101_reset();

    uint8_t partnum = cc1101_read_status(CC1101_PARTNUM);
    uint8_t version = cc1101_read_status(CC1101_VERSION);
    gtw_console_log("CC1101 PARTNUM=0x%02X VERSION=0x%02X", partnum, version);

    cc1101_apply_config();

    /* Re-read after config for verification */
    partnum = cc1101_read_status(CC1101_PARTNUM);
    version = cc1101_read_status(CC1101_VERSION);

    if (partnum != 0x00 || version == 0x00) {
        gtw_console_log("CC1101 not found! PARTNUM=0x%02X VERSION=0x%02X",
                        partnum, version);
        ESP_LOGE(TAG, "CC1101 not found");
        return ESP_FAIL;
    }
    gtw_console_log("CC1101 init OK  PARTNUM=0x%02X VERSION=0x%02X",
                    partnum, version);

    /* Read back PATABLE for verification */
    uint8_t ptable[8];
    cc1101_read_burst_internal(CC1101_PATABLE, ptable, 8);
    gtw_console_log("PATABLE: %02X %02X %02X %02X %02X %02X %02X %02X",
                    ptable[0], ptable[1], ptable[2], ptable[3],
                    ptable[4], ptable[5], ptable[6], ptable[7]);

    return ESP_OK;
}
