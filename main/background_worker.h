#pragma once

/**
 * @brief Initialise the background worker task.
 *
 * Must be called after config_init() and channel_init().
 * Creates the debounce timer and the worker task which handles:
 *   - Deferred config+channel saves (debounced, 5 s)
 *   - Periodic motor position queries (every position_status_query_interval_s)
 */
void background_worker_init(void);

/**
 * @brief Schedule a deferred save (resets the 5 s debounce timer).
 *
 * Safe to call from any task or ISR context.
 */
void background_worker_notify_save(void);

/**
 * @brief Perform an immediate synchronous save and wait for it to finish.
 *
 * Blocks the calling task until the save is complete (up to 10 s).
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
