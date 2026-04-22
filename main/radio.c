#include "radio.h"
#include "radio_ops.h"
#include "cc1101.h"
#include "sx1262.h"
#include "config.h"
#include "cosmo/cosmo.h"
#include "status_led.h"
#include "webserver.h"
#include "channel.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "radio";

/* ── Event queue ─────────────────────────────────────────────────────────── */

typedef enum {
    RADIO_EVT_RX,    /* IRQ edge on gpio_gdo0: packet ready */
    RADIO_EVT_TX,    /* TX command from application task    */
    RADIO_EVT_STOP,  /* Graceful shutdown signal            */
} radio_evt_type_t;

typedef struct {
    radio_evt_type_t type;
    cosmo_packet_t   pkt;   /* populated for RADIO_EVT_TX */
} radio_evt_t;

#define RADIO_QUEUE_DEPTH 8
#define RADIO_RX_CHANNEL  0

static QueueHandle_t     s_radio_queue       = NULL;
static TaskHandle_t      s_radio_task_handle = NULL;
static SemaphoreHandle_t s_stop_sem          = NULL;
static int               s_active_irq_gpio   = -1;
static bool              s_initialized       = false;
static bool              s_isr_installed     = false;
static radio_state_t     s_radio_state       = RADIO_STATE_NOT_CONFIGURED;

static const radio_ops_t *s_ops = NULL;

/* Snapshot of the config that is currently running in hardware */
static radio_hw_cfg_t s_active_cfg;

/* Must be called with config_lock() held. */
static void radio_hw_cfg_from_config(radio_hw_cfg_t *out)
{
    out->enabled     = g_config.radio.enabled;
    out->type        = g_config.radio.type;
    out->gpio_miso   = g_config.radio.gpio_miso;
    out->gpio_mosi   = g_config.radio.gpio_mosi;
    out->gpio_sck    = g_config.radio.gpio_sck;
    out->gpio_csn    = g_config.radio.gpio_csn;
    out->gpio_gdo0   = g_config.radio.gpio_gdo0;
    out->gpio_rst    = g_config.radio.gpio_rst;
    out->gpio_busy   = g_config.radio.gpio_busy;
    out->spi_freq_hz = g_config.radio.spi_freq_hz;
}

/* ── ISR ─────────────────────────────────────────────────────────────────── */

static void IRAM_ATTR radio_irq_isr(void *arg)
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
    uint8_t  buf[COSMO_RAW_PACKET_LEN];
    int8_t   rssi_dbm    = 0;
    int16_t  freq_off_khz = 0;

    esp_err_t err = s_ops->handle_rx_irq(buf, COSMO_RAW_PACKET_LEN,
                                         &rssi_dbm, &freq_off_khz);
    if (err != ESP_OK) return;  /* driver already reset RX internally */

    char bin[COSMO_RAW_PACKET_LEN * 9];
    bytes_to_bin(buf, COSMO_RAW_PACKET_LEN, bin);
    gtw_console_log("RX BIN: %s", bin);

    cosmo_raw_packet_t raw;
    memcpy(raw.data, buf, COSMO_RAW_PACKET_LEN);
    raw.rssi = rssi_dbm;

    cosmo_packet_t pkt;
    esp_err_t decode_err = cosmo_decode(&raw, &pkt);
    if (decode_err == ESP_OK) {
        char pkt_str[256];
        cosmo_packet_to_str(&pkt, pkt_str, sizeof(pkt_str));
        gtw_console_log("PKT %s freq_off=%+d kHz", pkt_str, (int)freq_off_khz);
        channel_update_from_packet(&pkt);
        status_led_on_rx();
    } else {
        gtw_console_log("RADIO: bad pkt  rssi=%d dBm", (int)rssi_dbm);
    }
    webserver_ws_broadcast_packet(false, buf, COSMO_RAW_PACKET_LEN,
                                  decode_err == ESP_OK,
                                  decode_err == ESP_OK ? &pkt : NULL);
}

/* ── TX handling ─────────────────────────────────────────────────────────── */

static void radio_do_tx(const cosmo_packet_t *pkt)
{
    int total = 1 + (int)pkt->repeat;
    status_led_on_tx();

    cosmo_raw_packet_t raw;
    for (int i = 0; i < total; i++) {
        if (i > 0)
            vTaskDelay(pdMS_TO_TICKS(400));

        cosmo_encode(pkt, &raw);

        char bin[COSMO_RAW_PACKET_LEN * 9];
        bytes_to_bin(raw.data, COSMO_RAW_PACKET_LEN, bin);
        gtw_console_log("TX BIN [%d/%d]: %s", i + 1, total, bin);

        s_ops->enter_idle();
        s_ops->set_channel(pkt->proto == PROTO_COSMO_2WAY ? 1 : 0);
        s_ops->transmit(raw.data, COSMO_RAW_PACKET_LEN);
    }

    s_ops->set_channel(RADIO_RX_CHANNEL);
    s_ops->enter_rx();

    char pkt_str[128];
    cosmo_packet_to_str(pkt, pkt_str, sizeof(pkt_str));
    gtw_console_log("RADIO: TX x%d  %s", total, pkt_str);

    webserver_ws_broadcast_packet(true, raw.data, COSMO_RAW_PACKET_LEN, true, pkt);
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
    if (s_active_irq_gpio >= 0) {
        gpio_isr_handler_remove(s_active_irq_gpio);
        s_active_irq_gpio = -1;
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

    if (s_ops) {
        s_ops->deinit();
        s_ops = NULL;
    }

    spi_bus_free(SPI2_HOST);

    s_initialized = false;
    s_radio_state = RADIO_STATE_NOT_CONFIGURED;
    memset(&s_active_cfg, 0, sizeof(s_active_cfg));
    ESP_LOGI(TAG, "Radio deinitialized");
}

static esp_err_t radio_do_init(const radio_hw_cfg_t *hw)
{
    if (!hw->enabled) {
        ESP_LOGI(TAG, "Radio not enabled in config, skipping init");
        s_radio_state = RADIO_STATE_NOT_CONFIGURED;
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_radio_queue = xQueueCreate(RADIO_QUEUE_DEPTH, sizeof(radio_evt_t));
    if (!s_radio_queue) {
        ESP_LOGE(TAG, "Failed to create radio queue");
        return ESP_ERR_NO_MEM;
    }

    /* ── SPI bus (owned by radio.c for the lifetime of this init) ── */
    spi_bus_config_t buscfg = {
        .sclk_io_num   = hw->gpio_sck,
        .mosi_io_num   = hw->gpio_mosi,
        .miso_io_num   = hw->gpio_miso,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        vQueueDelete(s_radio_queue);
        s_radio_queue = NULL;
        s_radio_state = RADIO_STATE_ERROR;
        return err;
    }

    /* ── Select driver ── */
    s_ops = (hw->type == radio_type_t_sx1262) ? sx1262_get_ops() : cc1101_get_ops();

    err = s_ops->init(hw, SPI2_HOST);
    if (err != ESP_OK) {
        s_ops = NULL;
        vQueueDelete(s_radio_queue);
        s_radio_queue = NULL;
        spi_bus_free(SPI2_HOST);
        s_radio_state = RADIO_STATE_ERROR;
        return err;
    }

    /* ── Install GPIO ISR service (idempotent) ── */
    if (!s_isr_installed) {
        gpio_install_isr_service(0);
        s_isr_installed = true;
    }
    s_active_irq_gpio = hw->gpio_gdo0;
    gpio_isr_handler_add(s_active_irq_gpio, radio_irq_isr, NULL);

    xTaskCreate(radio_task, "radio", 4096, NULL, 10, &s_radio_task_handle);
    s_active_cfg    = *hw;
    s_initialized   = true;
    s_radio_state   = RADIO_STATE_OK;
    ESP_LOGI(TAG, "Radio initialised, entering RX");
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

radio_state_t radio_get_state(void)
{
    return s_radio_state;
}

esp_err_t radio_init(void)
{
    radio_hw_cfg_t hw;
    config_lock();
    radio_hw_cfg_from_config(&hw);
    config_unlock();
    return radio_do_init(&hw);
}

esp_err_t radio_apply_config(void)
{
    radio_hw_cfg_t desired;
    config_lock();
    radio_hw_cfg_from_config(&desired);
    config_unlock();

    if (memcmp(&s_active_cfg, &desired, sizeof(desired)) == 0) {
        ESP_LOGI(TAG, "Radio config unchanged, skipping reinit");
        return s_initialized ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Radio config changed, reinitializing");
    radio_do_deinit();
    return radio_do_init(&desired);
}

esp_err_t radio_request_tx(const cosmo_packet_t *pkt)
{
    if (!s_initialized || !s_radio_queue) return ESP_ERR_INVALID_STATE;
    radio_evt_t evt = { .type = RADIO_EVT_TX, .pkt = *pkt };
    if (xQueueSend(s_radio_queue, &evt, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    return ESP_OK;
}
