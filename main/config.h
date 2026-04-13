#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "channel.h"

#define CONFIG_HOSTNAME_MAX      64
#define CONFIG_MQTT_HOST_MAX     128
#define CONFIG_MQTT_USER_MAX     64
#define CONFIG_MQTT_PASS_MAX     128
#define CONFIG_MQTT_PREFIX_MAX   64
#define CONFIG_MQTT_HA_PREFIX_MAX 64

/* Default CC1101 pin assignments and SPI speed */
#define CONFIG_RADIO_DEFAULT_MISO      19
#define CONFIG_RADIO_DEFAULT_MOSI      23
#define CONFIG_RADIO_DEFAULT_SCK       18
#define CONFIG_RADIO_DEFAULT_CSN        5
#define CONFIG_RADIO_DEFAULT_GDO0      15
#define CONFIG_RADIO_DEFAULT_SPI_FREQ  500000

typedef struct {
    char hostname[CONFIG_HOSTNAME_MAX];

    struct {
        bool     enabled;
        char     broker[CONFIG_MQTT_HOST_MAX];
        uint16_t port;
        char     username[CONFIG_MQTT_USER_MAX];
        char     password[CONFIG_MQTT_PASS_MAX];
        bool     ha_discovery_enabled;                    /* default: true */
        char     ha_prefix[CONFIG_MQTT_HA_PREFIX_MAX];   /* default: "homeassistant" */
        char     mqtt_prefix[CONFIG_MQTT_PREFIX_MAX];    /* default: "unigtw" */
    } mqtt;

    struct {
        bool enabled;
        int  gpio_miso;
        int  gpio_mosi;
        int  gpio_sck;
        int  gpio_csn;
        int  gpio_gdo0;
        int  spi_freq_hz;
    } radio;
} gateway_config_t;

/* Must be called after LittleFS is mounted */
void config_init(void);

/* Channel persistence — called by channel.c */
void config_load_channels(cosmo_channel_t *out, int *out_count);
void config_save_channels(const cosmo_channel_t *arr, int count); /* debounced */

/* Force an immediate synchronous save (bypasses the debounce timer).
 * Use before a planned reboot so dirty state is not lost. */
void config_save_now(void);

/* Runtime config access */
void      config_get(gateway_config_t *out);
esp_err_t config_set_hostname(const char *hostname);
esp_err_t config_set_mqtt(const char *broker, uint16_t port,
                          const char *username, const char *password,
                          bool enabled,
                          bool ha_discovery_enabled,
                          const char *ha_prefix,
                          const char *mqtt_prefix);
esp_err_t config_set_radio(bool enabled,
                           int gpio_miso, int gpio_mosi, int gpio_sck,
                           int gpio_csn, int gpio_gdo0, int spi_freq_hz);
