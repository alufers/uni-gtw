#include "background_worker.h"
#include "channel.h"
#include "config.h"
#include "radio.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static const char *TAG = "bg_worker";

/* ── Timer handles ───────────────────────────────────────────────────────── */

static TimerHandle_t s_save_timer  = NULL; /* one-shot, 5 s debounce        */
static TimerHandle_t s_query_timer = NULL; /* auto-reload, CHECK_INTERVAL_MS */

/* ── Inhibit state ───────────────────────────────────────────────────────── */

#define INHIBIT_DURATION_S 45

static time_t s_inhibit_until = 0; /* epoch second; 0 = not inhibited */

/* ── Wakeup period for the query timer ───────────────────────────────────── */

/* The query timer fires every CHECK_INTERVAL_MS to check for due channels. */
#define CHECK_INTERVAL_MS 10000

/* Log free heap every N query-timer ticks (10 s each → every 60 s). */
#define HEAP_LOG_INTERVAL_TICKS 6

/* ── Position query logic ────────────────────────────────────────────────── */

static void maybe_query_position(void)
{
    if (!utils_time_is_valid())
        return;
    time_t now = time(NULL);

    if (now < s_inhibit_until)
        return;

    uint32_t best_serial = 0;
    int      best_idx    = -1;
    char     best_name[64] = {0};

    config_lock();

    uint16_t interval = (uint16_t)g_config.position_status_query_interval_s;
    if (interval == 0) {
        config_unlock();
        return;
    }

    /* Find the eligible channel least-recently queried. */
    time_t best_ts = now; /* only entries older than interval qualify */

    for (int i = 0; i < g_config.channels_len; i++) {
        struct cosmo_channel_t *ch = &g_config.channels[i];
        if (!ch->bidirectional_feedback)
            continue;

        time_t ch_ts = (time_t)ch->last_position_query_ts;
        /* Qualify if never queried OR enough time has elapsed since last query */
        if ((ch_ts == 0 || now - ch_ts >= (time_t)interval) &&
            (best_serial == 0 || ch_ts < best_ts)) {
            best_ts     = ch_ts;
            best_serial = ch->serial;
            best_idx    = i;
            snprintf(best_name, sizeof(best_name), "%s", sstr_cstr(ch->name));
        }
    }

    if (best_serial != 0)
        g_config.channels[best_idx].last_position_query_ts = (int64_t)now;

    config_unlock();

    if (best_serial != 0) {
        ESP_LOGI(TAG, "Auto-querying position for channel '%s'", best_name);
        channel_send_cmd(best_serial, COSMO_BTN_REQUEST_POSITION, 0);
    }
}

/* ── Timer callbacks (run in FreeRTOS timer daemon task) ─────────────────── */

static void save_timer_cb(TimerHandle_t t)
{
    (void)t;
    config_do_save();
}

static void query_timer_cb(TimerHandle_t t)
{
    (void)t;
    static int s_tick = 0;
    if (++s_tick >= HEAP_LOG_INTERVAL_TICKS) {
        s_tick = 0;
        ESP_LOGI(TAG, "Free heap: %u bytes (min ever: %u)",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size());
    }

    if (radio_get_state() != RADIO_STATE_OK)
        return;
    maybe_query_position();
    channel_check_timeouts();
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void background_worker_init(void)
{
    s_save_timer = xTimerCreate("cfg_save", pdMS_TO_TICKS(5000),
                                pdFALSE, NULL, save_timer_cb);

    s_query_timer = xTimerCreate("pos_query", pdMS_TO_TICKS(CHECK_INTERVAL_MS),
                                 pdTRUE, NULL, query_timer_cb);
    if (s_query_timer)
        xTimerStart(s_query_timer, 0);
}

void background_worker_notify_save(void)
{
    if (s_save_timer)
        xTimerReset(s_save_timer, 0);
}

void background_worker_save_now(void)
{
    /* Stop debounce timer to avoid a duplicate save */
    if (s_save_timer)
        xTimerStop(s_save_timer, 0);

    config_do_save();
}

void background_worker_inhibit_position_query(void)
{
    time_t now = time(NULL);
    if (now > 0)
        s_inhibit_until = now + INHIBIT_DURATION_S;
}
