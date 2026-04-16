#include "freertos/FreeRTOS.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "background_worker.h"
#include "channel.h"
#include "config.h"
#include "esp_littlefs.h"
#include "mqtt.h"
#include "radio.h"
#include "status_led.h"
#include "webserver.h"
#include "wifi_manager.h"

static const char *TAG = "uni-gtw";

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

    /* Background worker: deferred saves + periodic position queries */
    background_worker_init();

    /* Init network stack before anything that opens sockets */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Status LED — must be after event loop, before wifi_manager_init */
    status_led_init();

    webserver_early_init();
    channel_init();
    webserver_start();

    /* Start WiFi — netifs are created here */
    wifi_manager_init();

    /* Apply hostname to STA netif and mDNS */
    char hostname[64];
    config_lock();
    snprintf(hostname, sizeof(hostname), "%s", sstr_cstr(g_config.hostname));
    config_unlock();

    esp_netif_t *sta_netif = wifi_manager_get_sta_netif();
    if (sta_netif)
        esp_netif_set_hostname(sta_netif, hostname);

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(hostname);
    mdns_instance_name_set("uni-gtw RF Gateway");
    mdns_service_add(NULL, "_http",    "_tcp", 80, NULL, 0);
    mdns_service_add(NULL, "_uni_gtw", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS started: %s.local", hostname);

    /* Radio init */
    radio_init();

    /* MQTT init */
    mqtt_init();

    webserver_start_status_timer();
}
