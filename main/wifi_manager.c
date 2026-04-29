#include "wifi_manager.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "webserver.h"

static const char *TAG = "wifi_mgr";

static volatile wifi_mgr_mode_t s_mode               = WIFI_MGR_MODE_NONE;
static volatile bool            s_has_ever_connected  = false;
static volatile int             s_boot_fail_count     = 0;
static volatile bool            s_scanning            = false;
static char                     s_ssid[33];
static char                     s_pass[65];
static EventGroupHandle_t       s_scan_eg;
#define SCAN_DONE_BIT BIT0

/* Both netifs created upfront to avoid dynamic creation during APSTA transitions */
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;

/* ── NVS helpers ──────────────────────────────────────────────────────────── */

static esp_err_t nvs_load_credentials(char *ssid, char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t ssid_len = 33, pass_len = 65;
    err = nvs_get_str(h, WIFI_NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_OK)
        err = nvs_get_str(h, WIFI_NVS_KEY_PASS, pass, &pass_len);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK)
        err = nvs_set_str(h, WIFI_NVS_KEY_PASS, pass);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t wifi_manager_clear_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, WIFI_NVS_KEY_SSID);
    nvs_erase_key(h, WIFI_NVS_KEY_PASS);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ── Mode helpers ─────────────────────────────────────────────────────────── */

static esp_err_t start_ap_mode(void)
{
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = WIFI_AP_SSID,
            .ssid_len       = sizeof(WIFI_AP_SSID) - 1,
            .channel        = 1,
            .authmode       = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_mode = WIFI_MGR_MODE_AP;
    ESP_LOGI(TAG, "AP mode started: SSID=%s", WIFI_AP_SSID);
    gtw_console_log("WiFi: AP mode, SSID: %s (connect to configure)", WIFI_AP_SSID);
    return ESP_OK;
}

static esp_err_t start_sta_mode(const char *ssid, const char *pass)
{
    wifi_config_t sta_cfg = {0};
    snprintf((char *)sta_cfg.sta.ssid,     sizeof(sta_cfg.sta.ssid)+1,     "%s", ssid);
    snprintf((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password)+1, "%s", pass);
    sta_cfg.sta.threshold.authmode = (strlen(pass) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_mode = WIFI_MGR_MODE_STA;
    ESP_LOGI(TAG, "STA mode started: SSID=%s", ssid);
    return ESP_OK;
}

/* ── Event handler ────────────────────────────────────────────────────────── */

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            if (!s_scanning) {
                ESP_LOGI(TAG, "STA started, connecting to \"%s\"...", s_ssid);
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "STA connected");
            s_boot_fail_count    = 0;
            s_has_ever_connected = true;
            /* If we were in APSTA (AP fallback + new creds), drop to STA only */
            {
                wifi_mode_t mode;
                if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_APSTA) {
                    esp_wifi_set_mode(WIFI_MODE_STA);
                    s_mode = WIFI_MGR_MODE_STA;
                }
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *info =
                (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "STA disconnected (reason=%d)", info->reason);
            if (!s_has_ever_connected) {
                s_boot_fail_count++;
                ESP_LOGW(TAG, "Boot failure count: %d/%d",
                         s_boot_fail_count, WIFI_MAX_BOOT_FAILURES);
                if (s_boot_fail_count >= WIFI_MAX_BOOT_FAILURES) {
                    ESP_LOGW(TAG, "Too many failures, switching to AP mode");
                    gtw_console_log("WiFi: %d failed attempts, switching to AP mode",
                                    s_boot_fail_count);
                    esp_wifi_stop();
                    start_ap_mode();
                    return;
                }
            }
            esp_wifi_connect();
            break;
        }

        case WIFI_EVENT_SCAN_DONE:
            xEventGroupSetBits(s_scan_eg, SCAN_DONE_BIT);
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", ip_str);
        gtw_console_log("uni-gtw ready. IP: %s", ip_str);

        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&sntp_cfg);
        ESP_LOGI(TAG, "SNTP initialized");
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

esp_err_t wifi_manager_init(void)
{
    s_scan_eg = xEventGroupCreate();

    /* esp_netif_init() and esp_event_loop_create_default() must be called
     * by the caller (app_main) before webserver_start(), so they are NOT
     * called here. */

    /* Create both netifs upfront; avoids dynamic creation during APSTA transitions */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL, NULL));

    char ssid[33] = {0}, pass[65] = {0};
    if (nvs_load_credentials(ssid, pass) == ESP_OK && strlen(ssid) > 0) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
        snprintf(s_pass, sizeof(s_pass), "%s", pass);
        ESP_LOGI(TAG, "Loaded credentials for \"%s\"", s_ssid);
        return start_sta_mode(s_ssid, s_pass);
    }

    ESP_LOGI(TAG, "No saved credentials, starting AP mode");
    return start_ap_mode();
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32) return ESP_ERR_INVALID_ARG;
    if (!password || strlen(password) > 63)               return ESP_ERR_INVALID_ARG;

    esp_err_t err = nvs_save_credentials(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_save_credentials failed: %s", esp_err_to_name(err));
        /* Non-fatal — proceed anyway */
    }

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", password);
    s_has_ever_connected = false;
    s_boot_fail_count    = 0;

    wifi_config_t sta_cfg = {0};
    snprintf((char *)sta_cfg.sta.ssid,     sizeof(sta_cfg.sta.ssid),     "%s", ssid);
    snprintf((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), "%s", password);
    sta_cfg.sta.threshold.authmode = strlen(password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    if (s_mode == WIFI_MGR_MODE_AP) {
        /* Set config before switching mode: mode change fires WIFI_EVENT_STA_START
         * which immediately calls esp_wifi_connect(), so the config must be in
         * place before that event fires. */
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        /* STA_START event handler calls esp_wifi_connect() — no need to here. */
        /* s_mode will be confirmed STA once STA_CONNECTED fires and drops APSTA */
    } else {
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        esp_wifi_connect();
    }

    gtw_console_log("WiFi: connecting to \"%s\"...", ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_scan(wifi_scan_result_t *out, uint16_t *out_count)
{
    xEventGroupClearBits(s_scan_eg, SCAN_DONE_BIT);

    /* Need STA capability to scan; if AP-only, temporarily switch to APSTA */
    wifi_mode_t mode_before;
    esp_wifi_get_mode(&mode_before);
    bool switched_to_apsta = false;
    if (mode_before == WIFI_MODE_AP) {
        s_scanning = true; /* suppress auto-connect on STA_START */
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        switched_to_apsta = true;
    }

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        if (switched_to_apsta) {
            esp_wifi_set_mode(WIFI_MODE_AP);
            s_scanning = false;
        }
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_scan_eg, SCAN_DONE_BIT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(8000));
    if (!(bits & SCAN_DONE_BIT)) {
        if (switched_to_apsta) {
            esp_wifi_set_mode(WIFI_MODE_AP);
            s_scanning = false;
        }
        return ESP_ERR_TIMEOUT;
    }

    uint16_t count = WIFI_SCAN_MAX_APS;
    wifi_ap_record_t *records = calloc(count, sizeof(wifi_ap_record_t));
    if (!records) {
        if (switched_to_apsta) {
            esp_wifi_set_mode(WIFI_MODE_AP);
            s_scanning = false;
        }
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < count; i++) {
            snprintf(out[i].ssid, sizeof(out[i].ssid), "%s",
                     (char *)records[i].ssid);
            out[i].rssi     = records[i].rssi;
            out[i].authmode = (uint8_t)records[i].authmode;
        }
        *out_count = count;
    }
    free(records);

    if (switched_to_apsta) {
        esp_wifi_set_mode(WIFI_MODE_AP);
        s_scanning = false;
    }

    return err;
}

wifi_mgr_mode_t wifi_manager_get_mode(void)
{
    return s_mode;
}

bool wifi_manager_get_rssi(int8_t *out_rssi)
{
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        *out_rssi = info.rssi;
        return true;
    }
    return false;
}

bool wifi_manager_get_sta_ssid(char *out_ssid, size_t len)
{
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        snprintf(out_ssid, len, "%s", (char *)info.ssid);
        return true;
    }
    return false;
}

esp_netif_t *wifi_manager_get_sta_netif(void)
{
    return s_sta_netif;
}
