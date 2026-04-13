#include "background_worker.h"
#include "config.h"
#include "channel.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "bg_worker";

/* ── Task / timer handles ────────────────────────────────────────────────── */

static TaskHandle_t  s_task       = NULL;
static TimerHandle_t s_save_timer = NULL;

/* Set to the caller's task handle before kicking the task for a sync save. */
static TaskHandle_t  s_save_waiter = NULL;

/* Set to true when the debounce timer fires (save is due). */
static volatile bool s_save_pending = false;

/* ── Inhibit state ───────────────────────────────────────────────────────── */

#define INHIBIT_DURATION_S 45

static time_t s_inhibit_until = 0; /* epoch second; 0 = not inhibited */

/* ── Per-channel query tracking ──────────────────────────────────────────── */

typedef struct {
    uint32_t serial;
    time_t   ts; /* last time we sent REQUEST_POSITION; 0 = never */
} query_track_t;

static query_track_t s_query_track[CHANNEL_MAX_COUNT];

/* ── Wakeup period for the worker loop ───────────────────────────────────── */

/* The task wakes at least every CHECK_INTERVAL_MS to check position queries. */
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

static void maybe_query_position(void)
{
    gateway_config_t *cfg = malloc(sizeof(gateway_config_t));
    if (!cfg) {
        ESP_LOGW(TAG, "maybe_query_position: out of memory");
        return;
    }
    config_get(cfg);

    uint16_t interval = cfg->position_status_query_interval_s;
    free(cfg);

    if (interval == 0)
        return;

    time_t now = time(NULL);
    if (now > 0 && now < s_inhibit_until)
        return;

    cosmo_channel_t *channels = malloc(CHANNEL_MAX_COUNT * sizeof(cosmo_channel_t));
    if (!channels) {
        ESP_LOGW(TAG, "maybe_query_position: out of memory");
        return;
    }
    int count = channel_snapshot(channels);

    /* Find the eligible channel least-recently queried */
    int    best_idx = -1;
    time_t best_ts  = now; /* initialise to "now" — only entries older than interval qualify */

    for (int i = 0; i < count; i++) {
        if (channels[i].proto != PROTO_COSMO_2WAY)
            continue;
        if (!channels[i].bidirectional_feedback)
            continue;

        time_t ch_ts = get_query_ts(channels[i].serial);
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
                 channels[best_idx].name);
        channel_send_cmd(channels[best_idx].serial, COSMO_BTN_REQUEST_POSITION, 0);
        set_query_ts(channels[best_idx].serial, now);
    }

    free(channels);
}

/* ── Worker task ─────────────────────────────────────────────────────────── */

static void worker_task_fn(void *arg)
{
    while (1) {
        /* Block until notified or timeout */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CHECK_INTERVAL_MS));

        if (s_save_pending) {
            s_save_pending = false;
            config_do_save();
            /* Wake synchronous waiter if present */
            TaskHandle_t waiter = s_save_waiter;
            s_save_waiter = NULL;
            if (waiter)
                xTaskNotifyGive(waiter);
        }

        maybe_query_position();
    }
}

/* ── Timer callback ──────────────────────────────────────────────────────── */

static void save_timer_cb(TimerHandle_t t)
{
    (void)t;
    s_save_pending = true;
    if (s_task)
        xTaskNotifyGive(s_task);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void background_worker_init(void)
{
    memset(s_query_track, 0, sizeof(s_query_track));

    s_save_timer = xTimerCreate("cfg_save", pdMS_TO_TICKS(5000),
                                pdFALSE, NULL, save_timer_cb);

    xTaskCreate(worker_task_fn, "bg_worker", 6144, NULL, 5, &s_task);
}

void background_worker_notify_save(void)
{
    if (s_save_timer)
        xTimerReset(s_save_timer, 0);
}

void background_worker_save_now(void)
{
    if (!s_task) {
        /* Worker not yet initialised — call directly */
        config_do_save();
        return;
    }
    /* Stop debounce timer to avoid a duplicate save */
    if (s_save_timer)
        xTimerStop(s_save_timer, 0);

    s_save_pending  = true;
    s_save_waiter   = xTaskGetCurrentTaskHandle();
    xTaskNotifyGive(s_task);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
}

void background_worker_inhibit_position_query(void)
{
    time_t now = time(NULL);
    if (now > 0)
        s_inhibit_until = now + INHIBIT_DURATION_S;
}
