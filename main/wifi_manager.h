#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

#define WIFI_NVS_NAMESPACE     "wifi"
#define WIFI_NVS_KEY_SSID      "ssid"
#define WIFI_NVS_KEY_PASS      "pass"
#define WIFI_AP_SSID           "UNI-GTW"
#define WIFI_MAX_BOOT_FAILURES 5
#define WIFI_SCAN_MAX_APS      20

typedef enum {
    WIFI_MGR_MODE_NONE = 0,
    WIFI_MGR_MODE_STA,
    WIFI_MGR_MODE_AP,
} wifi_mgr_mode_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t authmode; /* 0 = open */
} wifi_scan_result_t;

esp_err_t       wifi_manager_init(void);
esp_err_t       wifi_manager_set_credentials(const char *ssid, const char *password);
esp_err_t       wifi_manager_clear_credentials(void);
esp_err_t       wifi_manager_scan(wifi_scan_result_t *out, uint16_t *out_count);
wifi_mgr_mode_t wifi_manager_get_mode(void);
bool            wifi_manager_get_rssi(int8_t *out_rssi);
bool            wifi_manager_get_sta_ssid(char *out_ssid, size_t len);
esp_netif_t    *wifi_manager_get_sta_netif(void);
