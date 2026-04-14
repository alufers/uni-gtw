#pragma once

/**
 * @brief Initialise the background worker task.
 *
 * Must be called after config_init() and channel_init().
 * Creates two FreeRTOS timers (callbacks run in the timer daemon task):
 *   - Deferred config+channel saves (one-shot debounce, 5 s)
 *   - Periodic motor position queries (auto-reload, every 10 s check)
 */
void background_worker_init(void);

/**
 * @brief Schedule a deferred save (resets the 5 s debounce timer).
 *
 * Safe to call from any task or ISR context.
 */
void background_worker_notify_save(void);

/**
 * @brief Perform an immediate synchronous save on the calling task.
 *
 * Cancels the debounce timer and calls config_do_save() directly.
 * Use before a planned reboot so dirty state is not lost.
 */
void background_worker_save_now(void);

/**
 * @brief Inhibit automatic position queries for 45 seconds.
 *
 * Call this whenever a user-initiated command is sent (via WS or MQTT)
 * to avoid flooding a motor that is actively being controlled.
 */
void background_worker_inhibit_position_query(void);
