#include "radio.h"
#include "cc1101.h"
#include "config.h"
#include "cosmo.h"
#include "webserver.h"
#include "channel.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "radio";

/* ── Event queue ─────────────────────────────────────────────────────────── */

typedef enum {
    RADIO_EVT_RX,    /* GDO0 falling edge: packet in RX FIFO */
    RADIO_EVT_TX,    /* TX command from application task     */
    RADIO_EVT_STOP,  /* Graceful shutdown signal             */
} radio_evt_type_t;

typedef struct {
    radio_evt_type_t type;
    cosmo_packet_t   pkt;   /* populated for RADIO_EVT_TX */
} radio_evt_t;

#define RADIO_QUEUE_DEPTH 8

static QueueHandle_t     s_radio_queue      = NULL;
static TaskHandle_t      s_radio_task_handle = NULL;
static SemaphoreHandle_t s_stop_sem         = NULL;
static int               s_active_gdo0      = -1;
static bool              s_initialized      = false;
static bool              s_isr_installed    = false;

/* Snapshot of the config that is currently running in hardware */
typedef struct {
    bool enabled;
    int  gpio_miso, gpio_mosi, gpio_sck, gpio_csn, gpio_gdo0, spi_freq_hz;
} radio_hw_cfg_t;

static radio_hw_cfg_t s_active_cfg;

static void radio_hw_cfg_from_gw(radio_hw_cfg_t *out, const gateway_config_t *cfg)
{
    out->enabled     = cfg->radio.enabled;
    out->gpio_miso   = cfg->radio.gpio_miso;
    out->gpio_mosi   = cfg->radio.gpio_mosi;
    out->gpio_sck    = cfg->radio.gpio_sck;
    out->gpio_csn    = cfg->radio.gpio_csn;
    out->gpio_gdo0   = cfg->radio.gpio_gdo0;
    out->spi_freq_hz = cfg->radio.spi_freq_hz;
}

/* ── ISR ─────────────────────────────────────────────────────────────────── */

static void IRAM_ATTR gdo0_isr(void *arg)
{
    radio_evt_t evt = { .type = RADIO_EVT_RX };
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_radio_queue, &evt, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ── Binary dump helper ──────────────────────────────────────────────────── */

static void bytes_to_bin(const uint8_t *data, int len, char *out)
{
    int n = 0;
    for (int i = 0; i < len; i++) {
        if (i > 0) out[n++] = ' ';
        for (int b = 7; b >= 0; b--)
            out[n++] = ((data[i] >> b) & 1) ? '1' : '0';
    }
    out[n] = '\0';
}

/* ── RX handling ─────────────────────────────────────────────────────────── */

static void radio_handle_rx(void)
{
    uint8_t rxbytes = cc1101_get_rxbytes();

    if ((rxbytes & 0x80) || (rxbytes & 0x7F) < COSMO_RAW_PACKET_LEN + COSMO_STATUS_BYTES) {
        ESP_LOGW(TAG, "RX FIFO issue (RXBYTES=0x%02X), flushing", rxbytes);
        cc1101_enter_idle();
        cc1101_flush_rx();
        cc1101_set_channel(1);
        cc1101_enter_rx();
        return;
    }

    uint8_t buf[COSMO_RAW_PACKET_LEN + COSMO_STATUS_BYTES];
    cc1101_read_rxfifo(buf, sizeof(buf));

    int8_t  freqest      = cc1101_get_freqest();
    int16_t freq_off_khz = (int16_t)(((int32_t)freqest * 26000) / 16384);

    cc1101_set_channel(1);
    cc1101_enter_rx();

    uint8_t rssi_raw = buf[COSMO_RAW_PACKET_LEN];
    int8_t  rssi_dbm = (rssi_raw >= 128)
                       ? ((int8_t)(rssi_raw - 256) / 2) - 74
                       : (int8_t)(rssi_raw / 2) - 74;

    cosmo_raw_packet_t raw;
    memcpy(raw.data, buf, COSMO_RAW_PACKET_LEN);
    raw.rssi = rssi_dbm;

    char bin[COSMO_RAW_PACKET_LEN * 9];
    bytes_to_bin(raw.data, COSMO_RAW_PACKET_LEN, bin);
    gtw_console_log("RX BIN: %s", bin);

    cosmo_packet_t pkt;
    if (cosmo_decode(&raw, &pkt) == ESP_OK) {
        char pkt_str[256];
        cosmo_packet_to_str(&pkt, pkt_str, sizeof(pkt_str));
        gtw_console_log("PKT %s freq_off=%+d kHz", pkt_str, (int)freq_off_khz);
        channel_update_from_packet(&pkt);
    } else {
        gtw_console_log("RADIO: bad pkt  rssi=%d dBm", (int)rssi_dbm);
    }
}

/* ── TX handling ─────────────────────────────────────────────────────────── */

static void radio_do_tx(const cosmo_packet_t *pkt)
{
    int total = 1 + (int)pkt->repeat;

    cc1101_enter_idle();
    cc1101_flush_tx();
    cc1101_flush_rx();
    cc1101_set_channel(pkt->proto == PROTO_COSMO_2WAY ? 1 : 0);

    for (int i = 0; i < total; i++) {
        if (i > 0)
            vTaskDelay(pdMS_TO_TICKS(400));

        cosmo_raw_packet_t raw;
        cosmo_encode(pkt, &raw);

        char bin[COSMO_RAW_PACKET_LEN * 9];
        bytes_to_bin(raw.data, COSMO_RAW_PACKET_LEN, bin);
        gtw_console_log("TX BIN [%d/%d]: %s", i + 1, total, bin);

        cc1101_enter_idle();
        cc1101_flush_tx();
        cc1101_write_txfifo(raw.data, COSMO_RAW_PACKET_LEN);
        cc1101_start_tx();

        for (int w = 0; w < 30; w++) {
            vTaskDelay(pdMS_TO_TICKS(5));
            if (cc1101_get_marcstate() != 0x13)
                break;
        }
    }

    cc1101_enter_idle();
    cc1101_set_channel(1);
    cc1101_flush_rx();
    cc1101_enter_rx();

    char pkt_str[128];
    cosmo_packet_to_str(pkt, pkt_str, sizeof(pkt_str));
    gtw_console_log("RADIO: TX x%d  %s", total, pkt_str);
}

/* ── Radio task ──────────────────────────────────────────────────────────── */

static void radio_task(void *arg)
{
    radio_evt_t evt;
    while (1) {
        if (xQueueReceive(s_radio_queue, &evt, portMAX_DELAY) != pdTRUE)
            continue;

        switch (evt.type) {
        case RADIO_EVT_RX:
            radio_handle_rx();
            break;
        case RADIO_EVT_TX:
            radio_do_tx(&evt.pkt);
            break;
        case RADIO_EVT_STOP:
            ESP_LOGI(TAG, "Radio task stopping");
            if (s_stop_sem) xSemaphoreGive(s_stop_sem);
            vTaskDelete(NULL);
            return; /* unreachable */
        }
    }
}

/* ── Internal init / deinit ──────────────────────────────────────────────── */

static void radio_do_deinit(void)
{
    if (!s_initialized) return;

    /* Remove ISR first so no new RX events enter the queue */
    if (s_active_gdo0 >= 0) {
        gpio_isr_handler_remove(s_active_gdo0);
        s_active_gdo0 = -1;
    }

    /* Signal task to stop and wait for it to acknowledge */
    if (s_radio_queue && s_radio_task_handle) {
        s_stop_sem = xSemaphoreCreateBinary();
        radio_evt_t stop = { .type = RADIO_EVT_STOP };
        if (xQueueSend(s_radio_queue, &stop, pdMS_TO_TICKS(500)) == pdTRUE && s_stop_sem)
            xSemaphoreTake(s_stop_sem, pdMS_TO_TICKS(5000));
        if (s_stop_sem) { vSemaphoreDelete(s_stop_sem); s_stop_sem = NULL; }
        s_radio_task_handle = NULL;
    }

    if (s_radio_queue) { vQueueDelete(s_radio_queue); s_radio_queue = NULL; }

    cc1101_deinit();

    s_initialized = false;
    memset(&s_active_cfg, 0, sizeof(s_active_cfg));
    ESP_LOGI(TAG, "Radio deinitialized");
}

static esp_err_t radio_do_init(const gateway_config_t *cfg)
{
    if (!cfg->radio.enabled) {
        ESP_LOGI(TAG, "Radio not enabled in config, skipping init");
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_radio_queue = xQueueCreate(RADIO_QUEUE_DEPTH, sizeof(radio_evt_t));
    if (!s_radio_queue) {
        ESP_LOGE(TAG, "Failed to create radio queue");
        return ESP_ERR_NO_MEM;
    }

    cc1101_config_t cc_cfg = {
        .gpio_miso   = cfg->radio.gpio_miso,
        .gpio_mosi   = cfg->radio.gpio_mosi,
        .gpio_sck    = cfg->radio.gpio_sck,
        .gpio_csn    = cfg->radio.gpio_csn,
        .gpio_gdo0   = cfg->radio.gpio_gdo0,
        .spi_freq_hz = cfg->radio.spi_freq_hz,
    };

    esp_err_t err = cc1101_init(&cc_cfg);
    if (err != ESP_OK) {
        vQueueDelete(s_radio_queue);
        s_radio_queue = NULL;
        return err;
    }

    if (!s_isr_installed) {
        gpio_install_isr_service(0);
        s_isr_installed = true;
    }
    s_active_gdo0 = cfg->radio.gpio_gdo0;
    gpio_isr_handler_add(s_active_gdo0, gdo0_isr, NULL);

    xTaskCreate(radio_task, "radio", 4096, NULL, 10, &s_radio_task_handle);

    cc1101_enter_rx();
    radio_hw_cfg_from_gw(&s_active_cfg, cfg);
    s_initialized = true;
    ESP_LOGI(TAG, "Radio initialised, entering RX");
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t radio_init(void)
{
    gateway_config_t cfg;
    config_get(&cfg);
    return radio_do_init(&cfg);
}

esp_err_t radio_apply_config(void)
{
    gateway_config_t cfg;
    config_get(&cfg);

    radio_hw_cfg_t desired;
    radio_hw_cfg_from_gw(&desired, &cfg);

    if (memcmp(&s_active_cfg, &desired, sizeof(desired)) == 0) {
        ESP_LOGI(TAG, "Radio config unchanged, skipping reinit");
        return s_initialized ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Radio config changed, reinitializing");
    radio_do_deinit();
    return radio_do_init(&cfg);
}

esp_err_t radio_request_tx(const cosmo_packet_t *pkt)
{
    if (!s_initialized || !s_radio_queue) return ESP_ERR_INVALID_STATE;
    radio_evt_t evt = { .type = RADIO_EVT_TX, .pkt = *pkt };
    if (xQueueSend(s_radio_queue, &evt, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    return ESP_OK;
}
