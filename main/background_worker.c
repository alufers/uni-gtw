#include "background_worker.h"
#include "config.h"
#include "channel.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static const char *TAG = "bg_worker";

/* ── Timer handles ───────────────────────────────────────────────────────── */

static TimerHandle_t s_save_timer  = NULL; /* one-shot, 5 s debounce        */
static TimerHandle_t s_query_timer = NULL; /* auto-reload, CHECK_INTERVAL_MS */

/* ── Inhibit state ───────────────────────────────────────────────────────── */

#define INHIBIT_DURATION_S 45

static time_t s_inhibit_until = 0; /* epoch second; 0 = not inhibited */

/* ── Per-channel query tracking ──────────────────────────────────────────── */

typedef struct {
    uint32_t serial;
    time_t   ts; /* last time we sent REQUEST_POSITION; 0 = never */
} query_track_t;

static query_track_t s_query_track[CHANNEL_MAX_COUNT];

/* ── Wakeup period for the query timer ───────────────────────────────────── */

/* The query timer fires every CHECK_INTERVAL_MS to check for due channels. */
#define CHECK_INTERVAL_MS 10000

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static time_t get_query_ts(uint32_t serial)
{
    for (int i = 0; i < CHANNEL_MAX_COUNT; i++) {
        if (s_query_track[i].serial == serial)
            return s_query_track[i].ts;
    }
    return 0;
}

static void set_query_ts(uint32_t serial, time_t ts)
{
    /* Update existing entry */
    for (int i = 0; i < CHANNEL_MAX_COUNT; i++) {
        if (s_query_track[i].serial == serial) {
            s_query_track[i].ts = ts;
            return;
        }
    }
    /* Insert into first free slot */
    for (int i = 0; i < CHANNEL_MAX_COUNT; i++) {
        if (s_query_track[i].serial == 0) {
            s_query_track[i].serial = serial;
            s_query_track[i].ts     = ts;
            return;
        }
    }
}

/* ── Position query logic ────────────────────────────────────────────────── */

/* Lightweight per-channel info collected under lock for position querying. */
typedef struct {
    uint32_t serial;
    bool     is_2way;
    bool     bidirectional_feedback;
    char     name[64];
} ch_query_info_t;

static void maybe_query_position(void)
{
    config_lock();
    uint16_t interval = (uint16_t)g_config.position_status_query_interval_s;
    config_unlock();

    if (interval == 0)
        return;

    time_t now = time(NULL);
    if (now > 0 && now < s_inhibit_until)
        return;

    /* Collect minimal channel info under lock (no heap allocation needed). */
    ch_query_info_t infos[CHANNEL_MAX_COUNT];
    int count = 0;

    config_lock();
    for (int i = 0; i < g_config.channels_len && i < CHANNEL_MAX_COUNT; i++) {
        struct cosmo_channel_t *ch = &g_config.channels[i];
        infos[count].serial               = ch->serial;
        infos[count].is_2way              = sstr_compare_c(ch->proto, "2way") == 0;
        infos[count].bidirectional_feedback = ch->bidirectional_feedback != 0;
        snprintf(infos[count].name, sizeof(infos[count].name),
                 "%s", sstr_cstr(ch->name));
        count++;
    }
    config_unlock();

    /* Find the eligible channel least-recently queried. */
    int    best_idx = -1;
    time_t best_ts  = now; /* only entries older than interval qualify */

    for (int i = 0; i < count; i++) {
        if (!infos[i].is_2way)
            continue;
        if (!infos[i].bidirectional_feedback)
            continue;

        time_t ch_ts = get_query_ts(infos[i].serial);
        /* Qualify if never queried OR enough time has elapsed since last query */
        if (ch_ts == 0 || now - ch_ts >= (time_t)interval) {
            if (best_idx < 0 || ch_ts < best_ts) {
                best_ts  = ch_ts;
                best_idx = i;
            }
        }
    }

    if (best_idx >= 0) {
        ESP_LOGI(TAG, "Auto-querying position for channel '%s'",
                 infos[best_idx].name);
        channel_send_cmd(infos[best_idx].serial, COSMO_BTN_REQUEST_POSITION, 0);
        set_query_ts(infos[best_idx].serial, now);
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
    maybe_query_position();
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void background_worker_init(void)
{
    memset(s_query_track, 0, sizeof(s_query_track));

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
