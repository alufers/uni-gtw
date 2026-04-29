#include "sx1262.h"
#include "webserver.h"

#include <string.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sx1262";

/* ── Runtime state ───────────────────────────────────────────────────────── */

static spi_device_handle_t s_spi;
static int                 s_csn_gpio;
static int                 s_busy_gpio;
static int                 s_rst_gpio;

/* ── SX1262 opcode constants ─────────────────────────────────────────────── */

#define SX_SET_STANDBY          0x80
#define SX_SET_RX               0x82
#define SX_SET_TX               0x83
#define SX_SET_RF_FREQUENCY     0x86
#define SX_SET_CALIBRATE        0x89
#define SX_SET_PACKET_TYPE      0x8A
#define SX_SET_MODULATION_PARAMS 0x8B
#define SX_SET_PACKET_PARAMS    0x8C
#define SX_SET_TX_PARAMS        0x8E
#define SX_SET_DIO_IRQ_PARAMS   0x08
#define SX_CLEAR_IRQ_STATUS     0x02
#define SX_GET_IRQ_STATUS       0x12
#define SX_GET_RX_BUFFER_STATUS 0x13
#define SX_GET_PACKET_STATUS    0x14
#define SX_SET_PA_CONFIG        0x95
#define SX_SET_REGULATOR_MODE   0x96
#define SX_SET_DIO3_AS_TCXO     0x97
#define SX_SET_CALIBRATE_IMAGE  0x98
#define SX_SET_RX_TX_FALLBACK   0x93
#define SX_WRITE_REGISTER       0x0D
#define SX_READ_REGISTER        0x1D
#define SX_WRITE_BUFFER         0x0E
#define SX_READ_BUFFER          0x1E

/* Standby modes */
#define SX_STDBY_RC   0x00
#define SX_STDBY_XOSC 0x01

/* ── RF parameters ───────────────────────────────────────────────────────── */

/* Channel 0: 868.149902 MHz  (matches CC1101 base frequency)
 * Channel 1: 868.349853 MHz  (channel 0 + 199.951 kHz, CC1101 channel spacing)
 * Formula: reg = (uint32_t)((uint64_t)freq_hz * (1ULL<<25) / 32000000UL)    */
static const uint32_t sx1262_channel_freq[2] = {
    (uint32_t)((uint64_t)868149902ULL * (1ULL << 25) / 32000000UL),
    (uint32_t)((uint64_t)868349853ULL * (1ULL << 25) / 32000000UL),
};

/* ── Low-level SPI helpers ───────────────────────────────────────────────── */

static uint8_t sx1262_xfer(uint8_t byte)
{
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &byte,
        .flags     = SPI_TRANS_USE_RXDATA,
    };
    spi_device_polling_transmit(s_spi, &t);
    return t.rx_data[0];
}

/* Wait for BUSY to go low (chip ready).  Called before every SPI transaction. */
static void sx1262_wait_busy(void)
{
    int timeout = 10000;  /* 10 ms in 1-µs steps */
    while (gpio_get_level(s_busy_gpio) && --timeout > 0)
        esp_rom_delay_us(1);
    if (timeout == 0)
        ESP_LOGW(TAG, "SX1262 BUSY timeout");
}

/* Write command: opcode + params */
static void sx1262_cmd(uint8_t opcode, const uint8_t *params, int plen)
{
    sx1262_wait_busy();
    gpio_set_level(s_csn_gpio, 0);
    sx1262_xfer(opcode);
    for (int i = 0; i < plen; i++)
        sx1262_xfer(params[i]);
    gpio_set_level(s_csn_gpio, 1);
}

/* Read command: opcode → read rlen response bytes.
 * First returned byte is always the chip status byte; actual data follows. */
static void sx1262_read(uint8_t opcode, uint8_t *resp, int rlen)
{
    sx1262_wait_busy();
    gpio_set_level(s_csn_gpio, 0);
    sx1262_xfer(opcode);
    for (int i = 0; i < rlen; i++)
        resp[i] = sx1262_xfer(0x00);
    gpio_set_level(s_csn_gpio, 1);
}

/* Write registers: addr(2) + data */
static void sx1262_write_reg(uint16_t addr, const uint8_t *data, int len)
{
    sx1262_wait_busy();
    gpio_set_level(s_csn_gpio, 0);
    sx1262_xfer(SX_WRITE_REGISTER);
    sx1262_xfer((addr >> 8) & 0xFF);
    sx1262_xfer(addr & 0xFF);
    for (int i = 0; i < len; i++)
        sx1262_xfer(data[i]);
    gpio_set_level(s_csn_gpio, 1);
}

/* Write data buffer at offset */
static void sx1262_write_buf(uint8_t offset, const uint8_t *data, int len)
{
    sx1262_wait_busy();
    gpio_set_level(s_csn_gpio, 0);
    sx1262_xfer(SX_WRITE_BUFFER);
    sx1262_xfer(offset);
    for (int i = 0; i < len; i++)
        sx1262_xfer(data[i]);
    gpio_set_level(s_csn_gpio, 1);
}

/* Read data buffer: offset → NOP → data */
static void sx1262_read_buf(uint8_t offset, uint8_t *data, int len)
{
    sx1262_wait_busy();
    gpio_set_level(s_csn_gpio, 0);
    sx1262_xfer(SX_READ_BUFFER);
    sx1262_xfer(offset);
    sx1262_xfer(0x00);  /* NOP */
    for (int i = 0; i < len; i++)
        data[i] = sx1262_xfer(0x00);
    gpio_set_level(s_csn_gpio, 1);
}

/* ── SetRfFrequency helper ────────────────────────────────────────────────── */

static void sx1262_set_freq(uint32_t freq_reg)
{
    uint8_t p[4] = {
        (freq_reg >> 24) & 0xFF,
        (freq_reg >> 16) & 0xFF,
        (freq_reg >>  8) & 0xFF,
        (freq_reg >>  0) & 0xFF,
    };
    sx1262_cmd(SX_SET_RF_FREQUENCY, p, 4);
}

/* ── radio_ops_t implementation ──────────────────────────────────────────── */

static esp_err_t sx1262_ops_init(const radio_hw_cfg_t *hw, spi_host_device_t host)
{
    s_csn_gpio  = hw->gpio_csn;
    s_busy_gpio = hw->gpio_busy;
    s_rst_gpio  = hw->gpio_rst;

    /* ── CSN GPIO ── */
    gpio_reset_pin(s_csn_gpio);
    gpio_set_direction(s_csn_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(s_csn_gpio, 1);

    /* ── RST GPIO ── */
    if (s_rst_gpio >= 0) {
        gpio_reset_pin(s_rst_gpio);
        gpio_set_direction(s_rst_gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(s_rst_gpio, 0);
        esp_rom_delay_us(1000);  /* RST low 1 ms */
        gpio_set_level(s_rst_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(10));  /* wait 10 ms */
    }

    /* ── BUSY GPIO ── */
    if (s_busy_gpio >= 0) {
        gpio_reset_pin(s_busy_gpio);
        gpio_set_direction(s_busy_gpio, GPIO_MODE_INPUT);
        gpio_set_pull_mode(s_busy_gpio, GPIO_PULLDOWN_ONLY);
    }

    /* ── DIO1 GPIO — input, POSEDGE interrupt; ISR installed by radio.c ── */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << hw->gpio_gdo0,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    gpio_config(&io);

    /* ── SPI device ── */
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

    /* ── Wait for chip to be ready after reset ── */
    sx1262_wait_busy();

    /* ── 1. SetStandby(STDBY_RC) ── */
    uint8_t p1 = SX_STDBY_RC;
    sx1262_cmd(SX_SET_STANDBY, &p1, 1);

    /* ── 2. SetDio3AsTcxoCtrl(3.3V, 5ms) — enable 32 MHz TCXO on Heltec v4 ──
     * Voltage = 0x07 (3.3V), timeout = 5ms = 5000/15.625µs = 320 = 0x000140  */
    {
        uint8_t p[4] = { 0x07, 0x00, 0x01, 0x40 };
        sx1262_cmd(SX_SET_DIO3_AS_TCXO, p, 4);
    }

    /* ── 3. Calibrate(all = 0x7F) ── */
    {
        uint8_t p = 0x7F;
        sx1262_cmd(SX_SET_CALIBRATE, &p, 1);
        /* Calibration takes up to ~3ms; wait for BUSY */
        vTaskDelay(pdMS_TO_TICKS(5));
        sx1262_wait_busy();
    }

    /* ── 4. CalibrateImage(0xD7, 0xD8) — 863-870 MHz band ── */
    {
        uint8_t p[2] = { 0xD7, 0xD8 };
        sx1262_cmd(SX_SET_CALIBRATE_IMAGE, p, 2);
        sx1262_wait_busy();
    }

    /* ── 5. SetRegulatorMode(DC_DC + LDO = 0x01) ── */
    {
        uint8_t p = 0x01;
        sx1262_cmd(SX_SET_REGULATOR_MODE, &p, 1);
    }

    /* ── 6. SetRxTxFallbackMode(STDBY_XOSC = 0x30) ──
     * Keeps oscillator warm between packets for fast RX re-entry.          */
    {
        uint8_t p = 0x30;
        sx1262_cmd(SX_SET_RX_TX_FALLBACK, &p, 1);
    }

    /* ── 7. SetPacketType(FSK = 0x00) ── */
    {
        uint8_t p = 0x00;
        sx1262_cmd(SX_SET_PACKET_TYPE, &p, 1);
    }

    /* ── 8. SetRfFrequency (channel 0 = 868.149902 MHz) ── */
    sx1262_set_freq(sx1262_channel_freq[0]);

    /* ── 9. SetPaConfig(0x04, 0x07, 0x00, 0x01) — SX1262 max PA ── */
    {
        uint8_t p[4] = { 0x04, 0x07, 0x00, 0x01 };
        sx1262_cmd(SX_SET_PA_CONFIG, p, 4);
    }

    /* ── 10. SetTxParams(14 dBm, RAMP_200US = 0x04) ── */
    {
        uint8_t p[2] = { 14, 0x04 };
        sx1262_cmd(SX_SET_TX_PARAMS, p, 2);
    }

    /* ── 11. SetModulationParams (2-FSK, no Gaussian filtering)
     * BitRate reg (3 bytes) = 32 * FXTAL / bps = 32 * 32e6 / 2399
     * FDev reg (3 bytes)    = fdev_hz * 2^25 / FXTAL = 50781 * 2^25 / 32e6
     * BW = BW_467KHZ = 0x09  (≥ 2 * FDev + BitRate ≈ 104 kHz → 467 kHz ok)
     * PulseShape = NO_FILTER = 0x00  → plain 2-FSK                         */
    {
        uint32_t br  = (uint32_t)(32ULL * 32000000ULL / 2399);
        uint32_t fdev = (uint32_t)((uint64_t)50781ULL * (1ULL << 25) / 32000000UL);
        uint8_t p[8] = {
            (br   >> 16) & 0xFF, (br   >>  8) & 0xFF, br   & 0xFF,
            0x00,                /* PulseShape = NO_FILTER (2-FSK)           */
            0x09,                /* BW_467KHZ                                */
            (fdev >> 16) & 0xFF, (fdev >>  8) & 0xFF, fdev & 0xFF,
        };
        sx1262_cmd(SX_SET_MODULATION_PARAMS, p, 8);
    }

    /* ── 12. SetPacketParams (fixed 9-byte, 4-byte preamble, 0x2DD4 sync)
     * PreambleLength = 32 bits (4 bytes × 8)
     * PreambleDetector = 16-bit = 0x05
     * SyncWordLength = 16 bits (2 bytes)
     * AddrFilter = off = 0x00
     * PacketType = KNOWN_LEN (fixed) = 0x00
     * PayloadLength = 9
     * CRCType = OFF = 0x01
     * Whitening = off = 0x00                                                */
    {
        uint16_t pre = 32;  /* preamble bits */
        uint8_t  p[9] = {
            (pre >> 8) & 0xFF, pre & 0xFF,
            0x05,  /* PreambleDetectorLength = 16-bit                        */
            16,    /* SyncWordLength = 16 bits                               */
            0x00,  /* AddrComp = off                                         */
            0x00,  /* PacketType = fixed length                              */
            9,     /* PayloadLength = 9 bytes                                */
            0x01,  /* CRCType = off                                          */
            0x00,  /* Whitening = off                                        */
        };
        sx1262_cmd(SX_SET_PACKET_PARAMS, p, 9);
    }

    /* ── 13. WriteRegister: sync word 0x2DD4 at 0x06C0 ── */
    {
        uint8_t sw[8] = { 0x2D, 0xD4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        sx1262_write_reg(0x06C0, sw, 8);
    }

    /* ── 14. SetDioIrqParams: RX_DONE (bit 1) on DIO1, nothing else ── */
    {
        uint8_t p[8] = {
            0x00, 0x02,  /* GlobalIrqMask: RX_DONE                          */
            0x00, 0x02,  /* DIO1IrqMask:   RX_DONE                          */
            0x00, 0x00,  /* DIO2IrqMask:   none                              */
            0x00, 0x00,  /* DIO3IrqMask:   none                              */
        };
        sx1262_cmd(SX_SET_DIO_IRQ_PARAMS, p, 8);
    }

    /* ── 15. ClearIrqStatus ── */
    {
        uint8_t p[2] = { 0xFF, 0xFF };
        sx1262_cmd(SX_CLEAR_IRQ_STATUS, p, 2);
    }

    gtw_console_log("SX1262 init OK");

    /* ── 16. Enter continuous RX ── */
    {
        uint8_t p[3] = { 0xFF, 0xFF, 0xFF };  /* continuous */
        sx1262_cmd(SX_SET_RX, p, 3);
    }

    return ESP_OK;
}

static void sx1262_ops_deinit(void)
{
    if (!s_spi) return;
    /* Put chip in standby */
    uint8_t p = SX_STDBY_RC;
    sx1262_cmd(SX_SET_STANDBY, &p, 1);
    spi_bus_remove_device(s_spi);
    s_spi = NULL;
    if (s_rst_gpio  >= 0) gpio_reset_pin(s_rst_gpio);
    if (s_busy_gpio >= 0) gpio_reset_pin(s_busy_gpio);
    gpio_reset_pin(s_csn_gpio);
}

static void sx1262_ops_enter_idle(void)
{
    uint8_t p = SX_STDBY_RC;
    sx1262_cmd(SX_SET_STANDBY, &p, 1);
}

static void sx1262_ops_enter_rx(void)
{
    /* SetRfFrequency for channel 0, then SetRx(continuous) */
    sx1262_set_freq(sx1262_channel_freq[0]);
    uint8_t p[2] = { 0xFF, 0xFF };  /* use 3-byte timeout 0xFFFFFF */
    uint8_t pp[3] = { 0xFF, 0xFF, 0xFF };
    (void)p;
    sx1262_cmd(SX_SET_RX, pp, 3);
}

static void sx1262_ops_set_channel(uint8_t ch)
{
    if (ch < 2)
        sx1262_set_freq(sx1262_channel_freq[ch]);
}

static esp_err_t sx1262_ops_handle_rx_irq(uint8_t *buf, uint8_t len,
                                           int8_t *rssi_dbm,
                                           int16_t *freq_off_khz)
{
    /* Read and clear IRQ status */
    uint8_t irq_buf[3];
    sx1262_read(SX_GET_IRQ_STATUS, irq_buf, 3);
    uint16_t irq = ((uint16_t)irq_buf[1] << 8) | irq_buf[2];

    {
        uint8_t clr[2] = { 0xFF, 0xFF };
        sx1262_cmd(SX_CLEAR_IRQ_STATUS, clr, 2);
    }

    if (!(irq & 0x0002)) {
        /* RX_DONE bit not set — spurious or timeout IRQ */
        uint8_t p[3] = { 0xFF, 0xFF, 0xFF };
        sx1262_cmd(SX_SET_RX, p, 3);
        return ESP_FAIL;
    }

    /* Get buffer pointer */
    uint8_t rx_buf[3];
    sx1262_read(SX_GET_RX_BUFFER_STATUS, rx_buf, 3);
    /* rx_buf[0] = chip status, rx_buf[1] = payload_len, rx_buf[2] = offset */
    uint8_t payload_len = rx_buf[1];
    uint8_t offset      = rx_buf[2];

    if (payload_len < len) {
        ESP_LOGW(TAG, "RX payload too short: %u < %u", payload_len, len);
        uint8_t p[3] = { 0xFF, 0xFF, 0xFF };
        sx1262_cmd(SX_SET_RX, p, 3);
        return ESP_FAIL;
    }

    /* Read packet bytes from buffer */
    sx1262_read_buf(offset, buf, len);

    /* Get RSSI from packet status
     * Response: [chip_status, rx_status, rssi_sync_raw, rssi_avg_raw]
     * rssi_dbm = -rssi_sync_raw / 2                                        */
    uint8_t pkt_status[4];
    sx1262_read(SX_GET_PACKET_STATUS, pkt_status, 4);
    *rssi_dbm     = -(int8_t)(pkt_status[2] / 2);
    *freq_off_khz = 0;  /* SX1262 FSK does not expose freq offset easily     */

    /* Re-enter continuous RX */
    {
        uint8_t p[3] = { 0xFF, 0xFF, 0xFF };
        sx1262_cmd(SX_SET_RX, p, 3);
    }

    return ESP_OK;
}

static esp_err_t sx1262_ops_transmit(const uint8_t *data, uint8_t len)
{
    /* Put chip in standby with XOSC running for fast TX */
    uint8_t stdby = SX_STDBY_XOSC;
    sx1262_cmd(SX_SET_STANDBY, &stdby, 1);

    /* Write packet to buffer at offset 0 */
    sx1262_write_buf(0, data, len);

    /* Start TX (timeout=0 → single TX, chip returns to fallback mode after) */
    {
        uint8_t p[3] = { 0x00, 0x00, 0x00 };
        sx1262_cmd(SX_SET_TX, p, 3);
    }

    /* Wait for BUSY to go low (TX complete) — up to 500 ms */
    int timeout = 500;
    while (gpio_get_level(s_busy_gpio) && --timeout > 0)
        vTaskDelay(pdMS_TO_TICKS(1));

    if (timeout == 0)
        ESP_LOGW(TAG, "SX1262 TX timeout");

    {
        uint8_t clr[2] = { 0xFF, 0xFF };
        sx1262_cmd(SX_CLEAR_IRQ_STATUS, clr, 2);
    }

    return ESP_OK;
}

static const radio_ops_t sx1262_ops = {
    .init          = sx1262_ops_init,
    .deinit        = sx1262_ops_deinit,
    .enter_idle    = sx1262_ops_enter_idle,
    .enter_rx      = sx1262_ops_enter_rx,
    .set_channel   = sx1262_ops_set_channel,
    .handle_rx_irq = sx1262_ops_handle_rx_irq,
    .transmit      = sx1262_ops_transmit,
    .irq_edge      = GPIO_INTR_POSEDGE,
};

const radio_ops_t *sx1262_get_ops(void)
{
    return &sx1262_ops;
}
