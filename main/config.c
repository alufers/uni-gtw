#include "config.h"
#include "channel.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG        = "config";
static const char *CONFIG_PATH = "/littlefs/config.json";

static SemaphoreHandle_t s_mutex;
static TimerHandle_t     s_save_timer;
static TaskHandle_t      s_save_task;
static TaskHandle_t      s_save_waiter = NULL; /* task to notify after save */

static gateway_config_t s_config;
static cosmo_channel_t  s_channels[CHANNEL_MAX_COUNT];
static int              s_channel_count = 0;

/* ── Default hostname ────────────────────────────────────────────────────── */

static void set_default_hostname(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_config.hostname, sizeof(s_config.hostname),
             "uni-gtw%02x%02x", mac[4], mac[5]);
}

/* ── File I/O ────────────────────────────────────────────────────────────── */

static void do_save(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = s_channel_count;
    cosmo_channel_t snap[CHANNEL_MAX_COUNT];
    memcpy(snap, s_channels, (size_t)count * sizeof(cosmo_channel_t));
    gateway_config_t cfg = s_config;
    xSemaphoreGive(s_mutex);

    cJSON *root = cJSON_CreateObject();

    cJSON_AddNumberToObject(root, "version",  1);
    cJSON_AddStringToObject(root, "hostname", cfg.hostname);

    cJSON *mqtt_j = cJSON_CreateObject();
    cJSON_AddBoolToObject  (mqtt_j, "enabled",  cfg.mqtt.enabled);
    cJSON_AddStringToObject(mqtt_j, "broker",   cfg.mqtt.broker);
    cJSON_AddNumberToObject(mqtt_j, "port",     cfg.mqtt.port);
    cJSON_AddStringToObject(mqtt_j, "username", cfg.mqtt.username);
    cJSON_AddStringToObject(mqtt_j, "password", cfg.mqtt.password);
    cJSON_AddItemToObject(root, "mqtt", mqtt_j);

    cJSON *radio_j = cJSON_CreateObject();
    cJSON_AddBoolToObject  (radio_j, "enabled",    cfg.radio.enabled);
    cJSON_AddNumberToObject(radio_j, "gpio_miso",  cfg.radio.gpio_miso);
    cJSON_AddNumberToObject(radio_j, "gpio_mosi",  cfg.radio.gpio_mosi);
    cJSON_AddNumberToObject(radio_j, "gpio_sck",   cfg.radio.gpio_sck);
    cJSON_AddNumberToObject(radio_j, "gpio_csn",   cfg.radio.gpio_csn);
    cJSON_AddNumberToObject(radio_j, "gpio_gdo0",  cfg.radio.gpio_gdo0);
    cJSON_AddNumberToObject(radio_j, "spi_freq_hz",cfg.radio.spi_freq_hz);
    cJSON_AddItemToObject(root, "radio", radio_j);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *ch = cJSON_CreateObject();
        cJSON_AddStringToObject(ch, "name",                snap[i].name);
        cJSON_AddStringToObject(ch, "proto",               snap[i].proto == PROTO_COSMO_2WAY ? "2way" : "1way");
        cJSON_AddNumberToObject(ch, "serial",              (double)(uint32_t)snap[i].serial);
        cJSON_AddNumberToObject(ch, "counter",             (double)snap[i].counter);
        cJSON_AddBoolToObject  (ch, "force_tilt_support",  snap[i].force_tilt_support);
        cJSON_AddItemToArray(arr, ch);
    }
    cJSON_AddItemToObject(root, "channels", arr);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (str) {
        FILE *f = fopen(CONFIG_PATH, "w");
        if (f) {
            fputs(str, f);
            fclose(f);
            ESP_LOGI(TAG, "Config saved (%d channels)", count);
        } else {
            ESP_LOGW(TAG, "Failed to open config for writing");
        }
        free(str);
    }
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

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "Config JSON parse error");
        return;
    }

    cJSON *ver_j = cJSON_GetObjectItem(root, "version");
    int version = cJSON_IsNumber(ver_j) ? (int)cJSON_GetNumberValue(ver_j) : 0;
    ESP_LOGI(TAG, "Config version: %d", version);

    const char *hn = cJSON_GetStringValue(cJSON_GetObjectItem(root, "hostname"));
    if (hn && hn[0])
        snprintf(s_config.hostname, sizeof(s_config.hostname), "%s", hn);

    cJSON *mqtt_j = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt_j)) {
        cJSON *en = cJSON_GetObjectItem(mqtt_j, "enabled");
        if (cJSON_IsBool(en))
            s_config.mqtt.enabled = cJSON_IsTrue(en);
        const char *broker = cJSON_GetStringValue(cJSON_GetObjectItem(mqtt_j, "broker"));
        if (broker)
            snprintf(s_config.mqtt.broker, sizeof(s_config.mqtt.broker), "%s", broker);
        cJSON *port_j = cJSON_GetObjectItem(mqtt_j, "port");
        if (cJSON_IsNumber(port_j))
            s_config.mqtt.port = (uint16_t)cJSON_GetNumberValue(port_j);
        const char *user = cJSON_GetStringValue(cJSON_GetObjectItem(mqtt_j, "username"));
        if (user)
            snprintf(s_config.mqtt.username, sizeof(s_config.mqtt.username), "%s", user);
        const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(mqtt_j, "password"));
        if (pass)
            snprintf(s_config.mqtt.password, sizeof(s_config.mqtt.password), "%s", pass);
    }

    cJSON *radio_j = cJSON_GetObjectItem(root, "radio");
    if (cJSON_IsObject(radio_j)) {
        cJSON *en = cJSON_GetObjectItem(radio_j, "enabled");
        if (cJSON_IsBool(en))
            s_config.radio.enabled = cJSON_IsTrue(en);
#define LOAD_INT(field, key) { cJSON *j = cJSON_GetObjectItem(radio_j, key); if (cJSON_IsNumber(j)) s_config.radio.field = (int)cJSON_GetNumberValue(j); }
        LOAD_INT(gpio_miso,   "gpio_miso")
        LOAD_INT(gpio_mosi,   "gpio_mosi")
        LOAD_INT(gpio_sck,    "gpio_sck")
        LOAD_INT(gpio_csn,    "gpio_csn")
        LOAD_INT(gpio_gdo0,   "gpio_gdo0")
        LOAD_INT(spi_freq_hz, "spi_freq_hz")
#undef LOAD_INT
    }

    cJSON *arr = cJSON_GetObjectItem(root, "channels");
    int count = 0;
    if (cJSON_IsArray(arr)) {
        cJSON *item;
        cJSON_ArrayForEach(item, arr) {
            if (count >= CHANNEL_MAX_COUNT) break;
            const char *name      = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
            const char *proto_str = cJSON_GetStringValue(cJSON_GetObjectItem(item, "proto"));
            cJSON *serial_j  = cJSON_GetObjectItem(item, "serial");
            cJSON *counter_j = cJSON_GetObjectItem(item, "counter");
            if (!name || !proto_str || !cJSON_IsNumber(serial_j) || !cJSON_IsNumber(counter_j))
                continue;
            cosmo_channel_t *ch = &s_channels[count++];
            memset(ch, 0, sizeof(*ch));
            snprintf(ch->name, sizeof(ch->name), "%s", name);
            ch->proto             = (strcmp(proto_str, "2way") == 0) ? PROTO_COSMO_2WAY : PROTO_COSMO_1WAY;
            ch->serial            = (uint32_t)cJSON_GetNumberValue(serial_j);
            ch->serial           &= ~0x1F;
            ch->counter           = (uint16_t)cJSON_GetNumberValue(counter_j);
            ch->state             = CHANNEL_STATE_UNKNOWN;
            ch->position          = -1;
            cJSON *fts_j          = cJSON_GetObjectItem(item, "force_tilt_support");
            ch->force_tilt_support = cJSON_IsTrue(fts_j);
        }
    }
    s_channel_count = count;

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded config: hostname=%s, %d channels", s_config.hostname, count);
}

/* ── Save debounce ───────────────────────────────────────────────────────── */

static void save_task_fn(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        do_save();
        TaskHandle_t waiter = s_save_waiter;
        s_save_waiter = NULL;
        if (waiter) xTaskNotifyGive(waiter);
    }
}

static void save_timer_cb(TimerHandle_t t)
{
    if (s_save_task)
        xTaskNotifyGive(s_save_task);
}

static void mark_dirty(void)
{
    if (s_save_timer)
        xTimerReset(s_save_timer, 0);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void config_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    memset(&s_config, 0, sizeof(s_config));
    s_config.mqtt.port       = 1883;
    s_config.radio.enabled   = false;
    s_config.radio.gpio_miso = CONFIG_RADIO_DEFAULT_MISO;
    s_config.radio.gpio_mosi = CONFIG_RADIO_DEFAULT_MOSI;
    s_config.radio.gpio_sck  = CONFIG_RADIO_DEFAULT_SCK;
    s_config.radio.gpio_csn  = CONFIG_RADIO_DEFAULT_CSN;
    s_config.radio.gpio_gdo0 = CONFIG_RADIO_DEFAULT_GDO0;
    s_config.radio.spi_freq_hz = CONFIG_RADIO_DEFAULT_SPI_FREQ;

    set_default_hostname();
    do_load();

    s_save_timer = xTimerCreate("cfg_save", pdMS_TO_TICKS(5000),
                                pdFALSE, NULL, save_timer_cb);
    xTaskCreate(save_task_fn, "cfg_save", 4096, NULL, 5, &s_save_task);
}

void config_save_now(void)
{
    if (!s_save_task) return;
    /* Stop the debounce timer so it doesn't trigger a redundant save later */
    if (s_save_timer) xTimerStop(s_save_timer, 0);
    /* Register ourselves as the waiter, kick the save task, then block until
     * it completes.  do_save() runs on the save task's own 4 KB stack, avoiding
     * a stack overflow from deep httpd/LittleFS call chains. */
    s_save_waiter = xTaskGetCurrentTaskHandle();
    xTaskNotifyGive(s_save_task);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
}

void config_load_channels(cosmo_channel_t *out, int *out_count)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = s_channel_count;
    memcpy(out, s_channels, (size_t)count * sizeof(cosmo_channel_t));
    xSemaphoreGive(s_mutex);
    *out_count = count;
}

void config_save_channels(const cosmo_channel_t *arr, int count)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_channel_count = count;
    if (count > 0)
        memcpy(s_channels, arr, (size_t)count * sizeof(cosmo_channel_t));
    xSemaphoreGive(s_mutex);
    mark_dirty();
}

void config_get(gateway_config_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_config;
    xSemaphoreGive(s_mutex);
}

esp_err_t config_set_hostname(const char *hostname)
{
    if (!hostname || !hostname[0]) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_config.hostname, sizeof(s_config.hostname), "%s", hostname);
    xSemaphoreGive(s_mutex);
    mark_dirty();
    return ESP_OK;
}

esp_err_t config_set_mqtt(const char *broker, uint16_t port,
                          const char *username, const char *password,
                          bool enabled)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.mqtt.enabled = enabled;
    s_config.mqtt.port    = port ? port : 1883;
    snprintf(s_config.mqtt.broker,   sizeof(s_config.mqtt.broker),   "%s", broker   ? broker   : "");
    snprintf(s_config.mqtt.username, sizeof(s_config.mqtt.username), "%s", username ? username : "");
    snprintf(s_config.mqtt.password, sizeof(s_config.mqtt.password), "%s", password ? password : "");
    xSemaphoreGive(s_mutex);
    mark_dirty();
    return ESP_OK;
}

esp_err_t config_set_radio(bool enabled,
                           int gpio_miso, int gpio_mosi, int gpio_sck,
                           int gpio_csn, int gpio_gdo0, int spi_freq_hz)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.radio.enabled    = enabled;
    s_config.radio.gpio_miso  = gpio_miso;
    s_config.radio.gpio_mosi  = gpio_mosi;
    s_config.radio.gpio_sck   = gpio_sck;
    s_config.radio.gpio_csn   = gpio_csn;
    s_config.radio.gpio_gdo0  = gpio_gdo0;
    s_config.radio.spi_freq_hz = spi_freq_hz ? spi_freq_hz : CONFIG_RADIO_DEFAULT_SPI_FREQ;
    xSemaphoreGive(s_mutex);
    mark_dirty();
    return ESP_OK;
}
