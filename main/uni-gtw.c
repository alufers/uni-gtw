#include "freertos/FreeRTOS.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "channel.h"
#include "esp_littlefs.h"
#include "radio.h"
#include "webserver.h"
#include "wifi_manager.h"

static const char *TAG = "uni-gtw";

bool g_radio_ok = false;

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

    /* Init network stack before anything that opens sockets */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    webserver_early_init();
    channel_init();
    webserver_start();
    wifi_manager_init();
    g_radio_ok = (radio_init() == ESP_OK);
    webserver_start_status_timer();
}
