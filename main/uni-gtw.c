#include "freertos/FreeRTOS.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "channel.h"
#include "config.h"
#include "esp_littlefs.h"
#include "radio.h"
#include "webserver.h"
#include "wifi_manager.h"

static const char *TAG = "uni-gtw";

radio_state_t g_radio_state = RADIO_STATE_NOT_CONFIGURED;

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_vfs_littlefs_conf_t conf = {
        .base_path             = "/littlefs",
        .partition_label       = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount            = false,
    };

    ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)",
                     esp_err_to_name(ret));
        }
        return;
    }

    /* Load persisted config (hostname, mqtt, radio, channels) */
    config_init();

    /* Init network stack before anything that opens sockets */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    webserver_early_init();
    channel_init();
    webserver_start();

    /* Start WiFi — netifs are created here */
    wifi_manager_init();

    /* Apply hostname to STA netif and mDNS */
    gateway_config_t cfg;
    config_get(&cfg);

    esp_netif_t *sta_netif = wifi_manager_get_sta_netif();
    if (sta_netif)
        esp_netif_set_hostname(sta_netif, cfg.hostname);

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(cfg.hostname);
    mdns_instance_name_set("uni-gtw RF Gateway");
    mdns_service_add(NULL, "_http",    "_tcp", 80, NULL, 0);
    mdns_service_add(NULL, "_uni_gtw", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS started: %s.local", cfg.hostname);

    /* Radio init — result sets the three-state global */
    esp_err_t radio_err = radio_init();
    if (radio_err == ESP_ERR_NOT_SUPPORTED)
        g_radio_state = RADIO_STATE_NOT_CONFIGURED;
    else if (radio_err != ESP_OK)
        g_radio_state = RADIO_STATE_ERROR;
    else
        g_radio_state = RADIO_STATE_OK;

    webserver_start_status_timer();
}
