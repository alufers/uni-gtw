#include "config.h"
#include "background_worker.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG        = "config";
static const char *CONFIG_PATH = "/littlefs/config.json";

/* ── Global state ────────────────────────────────────────────────────────── */

struct gateway_config_t g_config;
SemaphoreHandle_t       g_config_mutex;

/* ── Default hostname ────────────────────────────────────────────────────── */

static void set_default_hostname(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[32];
    snprintf(buf, sizeof(buf), "uni-gtw%02x%02x", mac[4], mac[5]);
    sstr_free(g_config.hostname);
    g_config.hostname = sstr(buf);
}

/* ── File I/O ────────────────────────────────────────────────────────────── */

void config_do_save(void)
{
    /* Marshal JSON under the mutex (fast, in-memory), then write outside. */
    config_lock();
    sstr_t json = sstr_new();
    json_marshal_gateway_config_t(&g_config, json);
    config_unlock();

    FILE *f = fopen(CONFIG_PATH, "w");
    if (f) {
        fputs(sstr_cstr(json), f);
        fclose(f);
        ESP_LOGI(TAG, "Config saved");
    } else {
        ESP_LOGW(TAG, "Failed to open config for writing");
    }
    sstr_free(json);
}

static void do_load(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No config file found, using defaults");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 65536) { fclose(f); return; }

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[size] = '\0';

    /* Unmarshal into g_config under the mutex.
     * Use sstr_of() (LONG type), not sstr_ref() (REF type): json.gen.c's
     * inlined SSTR_CSTR_ macro only handles SHORT/LONG, so a REF sstr
     * returns garbage when used as unmarshal input. */
    config_lock();
    sstr_t in = sstr_of(buf, (size_t)size);
    int rc = json_unmarshal_gateway_config_t(in, &g_config);
    sstr_free(in);
    config_unlock();

    free(buf);

    if (rc != 0) {
        ESP_LOGW(TAG, "Config JSON parse error (rc=%d)", rc);
        return;
    }

    config_lock();
    /* Runtime-only fields are not persisted: reset state_type on load. */
    for (int i = 0; i < g_config.channels_len; i++)
        g_config.channels[i].state_type = channel_state_type_t_reported;

    ESP_LOGI(TAG, "Loaded config: hostname=%s, %d channels",
             sstr_cstr(g_config.hostname), g_config.channels_len);
    config_unlock();
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void config_init(void)
{
    g_config_mutex = xSemaphoreCreateMutex();

    gateway_config_t_init(&g_config);

    /* Set defaults */
    g_config.mqtt.port                       = 1883;
    g_config.mqtt.ha_discovery_enabled       = 1;
    g_config.position_status_query_interval_s = 60;
    g_config.gpio_status_led                 = -1;

    g_config.language             = language_t_en;
    g_config.web_password_enabled = 0;
    g_config.web_password         = sstr("");

    /* String defaults — sstr() allocates a new sstr_t from a C string literal. */
    g_config.mqtt.ha_prefix   = sstr("homeassistant");
    g_config.mqtt.mqtt_prefix = sstr("unigtw");

    /* Radio GPIO defaults */
    g_config.radio.enabled    = 0;
    g_config.radio.gpio_miso  = CONFIG_RADIO_DEFAULT_MISO;
    g_config.radio.gpio_mosi  = CONFIG_RADIO_DEFAULT_MOSI;
    g_config.radio.gpio_sck   = CONFIG_RADIO_DEFAULT_SCK;
    g_config.radio.gpio_csn   = CONFIG_RADIO_DEFAULT_CSN;
    g_config.radio.gpio_gdo0  = CONFIG_RADIO_DEFAULT_GDO0;
    g_config.radio.spi_freq_hz = CONFIG_RADIO_DEFAULT_SPI_FREQ;

    set_default_hostname();
    do_load();
}

void config_mark_dirty(void)
{
    background_worker_notify_save();
}

void config_save_now(void)
{
    background_worker_save_now();
}
