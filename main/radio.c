#include "radio.h"
#include "cc1101.h"
#include "cosmo.h"
#include "webserver.h"
#include "channel.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "radio";

/* ── Event queue ─────────────────────────────────────────────────────────── */

typedef enum {
    RADIO_EVT_RX,   /* GDO0 falling edge: packet in RX FIFO */
    RADIO_EVT_TX,   /* TX command from application task     */
} radio_evt_type_t;

typedef struct {
    radio_evt_type_t type;
    cosmo_packet_t   pkt;   /* populated for RADIO_EVT_TX */
} radio_evt_t;

#define RADIO_QUEUE_DEPTH 8

static QueueHandle_t s_radio_queue;

/* ── ISR ─────────────────────────────────────────────────────────────────── */

static void IRAM_ATTR gdo0_isr(void *arg)
{
    radio_evt_t evt = { .type = RADIO_EVT_RX };
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_radio_queue, &evt, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ── RX handling ─────────────────────────────────────────────────────────── */

static void radio_handle_rx(void)
{
    uint8_t rxbytes = cc1101_get_rxbytes();

    /* Check for RX FIFO overflow (bit 7) or insufficient data */
    if ((rxbytes & 0x80) || (rxbytes & 0x7F) < COSMO_RAW_PACKET_LEN + COSMO_STATUS_BYTES) {
        ESP_LOGW(TAG, "RX FIFO issue (RXBYTES=0x%02X), flushing", rxbytes);
        cc1101_enter_idle();
        cc1101_flush_rx();
        cc1101_enter_rx();
        return;
    }

    uint8_t buf[COSMO_RAW_PACKET_LEN + COSMO_STATUS_BYTES];
    cc1101_read_rxfifo(buf, sizeof(buf));

    /* Re-enter RX immediately to minimise gap */
    cc1101_enter_rx();

    /* Extract RSSI from first status byte (CC1101 datasheet §10.14.3) */
    uint8_t rssi_raw = buf[COSMO_RAW_PACKET_LEN];
    int8_t  rssi_dbm = (rssi_raw >= 128)
                       ? ((int8_t)(rssi_raw - 256) / 2) - 74
                       : (int8_t)(rssi_raw / 2) - 74;

    cosmo_raw_packet_t raw;
    memcpy(raw.data, buf, COSMO_RAW_PACKET_LEN);
    raw.rssi = rssi_dbm;

    cosmo_packet_t pkt;
    if (cosmo_decode(&raw, &pkt) == ESP_OK) {
        char pkt_str[256];
        cosmo_packet_to_str(&pkt, pkt_str, sizeof(pkt_str));
        gtw_console_log("PKT %s", pkt_str);
        channel_update_from_packet(&pkt);
    } else {
        /* Log raw hex for undecodable packets */
        char hex[COSMO_RAW_PACKET_LEN * 3 + 1];
        for (int i = 0; i < COSMO_RAW_PACKET_LEN; i++)
            snprintf(hex + i * 3, 4, "%02X ", raw.data[i]);
        hex[COSMO_RAW_PACKET_LEN * 3 - 1] = '\0';
        gtw_console_log("RADIO: bad pkt %s rssi=%d dBm", hex, (int)rssi_dbm);
    }
}

/* ── TX handling ─────────────────────────────────────────────────────────── */

static void radio_do_tx(const cosmo_packet_t *pkt)
{
    int total = 1 + (int)pkt->repeat;

    /* Go idle and flush before first TX */
    cc1101_enter_idle();
    cc1101_flush_tx();
    cc1101_flush_rx();  /* discard any noise that arrived during prep */

    for (int i = 0; i < total; i++) {
        if (i > 0)
            vTaskDelay(pdMS_TO_TICKS(50));

        cosmo_raw_packet_t raw;
        cosmo_encode(pkt, &raw);

        cc1101_enter_idle();
        cc1101_flush_tx();
        cc1101_write_txfifo(raw.data, COSMO_RAW_PACKET_LEN);
        cc1101_start_tx();

        /* Poll MARCSTATE until the chip leaves TX (goes IDLE after packet) */
        for (int w = 0; w < 30; w++) {          /* 30 × 5 ms = 150 ms max */
            vTaskDelay(pdMS_TO_TICKS(5));
            if (cc1101_get_marcstate() != 0x13)  /* 0x13 = TX */
                break;
        }
    }

    /* Return to RX; flush in case of any residual noise */
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
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t radio_init(void)
{
    s_radio_queue = xQueueCreate(RADIO_QUEUE_DEPTH, sizeof(radio_evt_t));
    if (!s_radio_queue) {
        ESP_LOGE(TAG, "Failed to create radio queue");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = cc1101_init();
    if (err != ESP_OK) return err;

    /* Install GDO0 ISR (service must be installed once per project) */
    gpio_install_isr_service(0);
    gpio_isr_handler_add(CC1101_GDO0_GPIO, gdo0_isr, NULL);

    xTaskCreate(radio_task, "radio", 4096, NULL, 10, NULL);

    cc1101_enter_rx();
    ESP_LOGI(TAG, "Radio initialised, entering RX");

    return ESP_OK;
}

esp_err_t radio_request_tx(const cosmo_packet_t *pkt)
{
    radio_evt_t evt = { .type = RADIO_EVT_TX, .pkt = *pkt };
    if (xQueueSend(s_radio_queue, &evt, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    return ESP_OK;
}
