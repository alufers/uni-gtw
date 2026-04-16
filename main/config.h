#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "json.gen.h"

/* ── Default CC1101 pin assignments and SPI speed ────────────────────────── */

#define CONFIG_RADIO_DEFAULT_MISO      19
#define CONFIG_RADIO_DEFAULT_MOSI      23
#define CONFIG_RADIO_DEFAULT_SCK       18
#define CONFIG_RADIO_DEFAULT_CSN        5
#define CONFIG_RADIO_DEFAULT_GDO0      15
#define CONFIG_RADIO_DEFAULT_SPI_FREQ  500000

/* ── Global config state ─────────────────────────────────────────────────── */

/* The single source of truth for all configuration and channel state.
 * Always take g_config_mutex before accessing any field. */
extern struct gateway_config_t g_config;
extern SemaphoreHandle_t g_config_mutex;

#define config_lock()   xSemaphoreTake(g_config_mutex, portMAX_DELAY)
#define config_unlock() xSemaphoreGive(g_config_mutex)

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Must be called after LittleFS is mounted. */
void config_init(void);

/* Perform an immediate synchronous config save (called by background_worker). */
void config_do_save(void);

/* Schedule a debounced save via the background worker. */
void config_mark_dirty(void);

/* Force an immediate synchronous save (bypasses the debounce timer).
 * Use before a planned reboot so dirty state is not lost. */
void config_save_now(void);
