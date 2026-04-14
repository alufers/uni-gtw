#pragma once

/**
 * Status LED driver — LEDC-based breathing/pulsing indicator.
 *
 * GPIO and enable/disable are read from gateway_config_t.gpio_status_led.
 * gpio_status_led == -1 means "no LED / disabled" — all calls are no-ops.
 *
 * Behaviour:
 *   - Not connected to WiFi  → breathe slowly (3 s period)
 *   - AP mode                → breathe quickly (0.6 s period)
 *   - Connected to WiFi      → solid at 1/3 brightness
 *   - RX packet              → dim to 1/10 brightness for 100 ms, then restore
 *   - TX packet              → full brightness for 100 ms, then restore
 *
 * No FreeRTOS task is created.  All transitions use esp_timer + ledc_set_fade_time_and_start.
 */

/**
 * Initialise the module.  Must be called after esp_event_loop_create_default()
 * and config_init(), but before wifi_manager_init() so that WiFi events are
 * captured from the start.
 */
void status_led_init(void);

/**
 * Re-apply configuration after a settings change.  Handles GPIO change and
 * enable/disable without requiring a reboot.
 */
void status_led_apply_config(void);

/** Signal that a radio packet was successfully received. */
void status_led_on_rx(void);

/** Signal that a radio packet is being transmitted. */
void status_led_on_tx(void);
