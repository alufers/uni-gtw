#include "status_led.h"
#include "config.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "status_led";

/* ── LEDC config ─────────────────────────────────────────────────────────── */

#define LEDC_MODE_SEL    LEDC_LOW_SPEED_MODE
#define LEDC_CH          LEDC_CHANNEL_0
#define LEDC_TMR         LEDC_TIMER_0
#define LEDC_DUTY_RES    LEDC_TIMER_13_BIT
#define LEDC_FREQ_HZ     5000

#define DUTY_FULL   8191u              /* 13-bit max — ~100%       */
#define DUTY_STEADY (DUTY_FULL / 8u)   /* ~1024  — solid state     */
#define DUTY_OFF    0u

/* Breathe half-periods (one fade up or one fade down) */
#define BREATHE_SLOW_HALF_MS  1500   /* 3 s total period — not connected */
#define BREATHE_FAST_HALF_MS   300   /* 0.6 s total period — AP mode     */
#define PULSE_MS               100   /* duration of TX/RX brightness override */

/* ── State ───────────────────────────────────────────────────────────────── */

typedef enum {
    LED_WIFI_DISCONNECTED = 0,   /* breathe slowly */
    LED_WIFI_AP,                 /* breathe quickly */
    LED_WIFI_CONNECTED,          /* solid 1/8 brightness */
} led_wifi_state_t;

static int               s_gpio           = -1;
static bool              s_fade_installed = false;
static led_wifi_state_t  s_wifi_state     = LED_WIFI_DISCONNECTED;
static bool              s_pulsing        = false;
static bool              s_breathe_up     = true;

static SemaphoreHandle_t  s_mutex         = NULL;
static esp_timer_handle_t s_breathe_timer = NULL;
static esp_timer_handle_t s_pulse_timer   = NULL;

/* ── LEDC helpers ────────────────────────────────────────────────────────── */

static void led_fade(uint32_t duty, int ms)
{
    /* Stop any in-progress fade before starting a new one.  On ESP32-C3 the
     * fade ISR keeps overriding the duty register until explicitly stopped;
     * calling ledc_fade_stop() here is safe even when no fade is running
     * (it just returns ESP_ERR_INVALID_STATE which we ignore). */
    ledc_fade_stop(LEDC_MODE_SEL, LEDC_CH);
    ledc_set_fade_time_and_start(LEDC_MODE_SEL, LEDC_CH, duty, ms, LEDC_FADE_NO_WAIT);
}

static void led_set(uint32_t duty)
{
    /* Must stop the fade ISR before a direct duty write or the ISR will
     * immediately overwrite the value on its next tick. */
    ledc_fade_stop(LEDC_MODE_SEL, LEDC_CH);
    ledc_set_duty(LEDC_MODE_SEL, LEDC_CH, duty);
    ledc_update_duty(LEDC_MODE_SEL, LEDC_CH);
}

/* ── Breathe helpers ─────────────────────────────────────────────────────── */

static int breathe_half_ms(led_wifi_state_t state)
{
    return (state == LED_WIFI_AP) ? BREATHE_FAST_HALF_MS : BREATHE_SLOW_HALF_MS;
}

/* Restart breathing from current position by fading to 0 first, then up.
 * Uses a smooth fade instead of an instant cut-to-black. */
static void restart_breathing(led_wifi_state_t state)
{
    int half_ms = breathe_half_ms(state);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_breathe_up = true;   /* timer callback will go UP after the fade-to-0 completes */
    xSemaphoreGive(s_mutex);

    esp_timer_stop(s_breathe_timer);
    led_fade(DUTY_OFF, half_ms);   /* smooth fade to 0 over one half-period */
    esp_timer_start_once(s_breathe_timer, (uint64_t)half_ms * 1000);
}

/* Apply the current base (non-pulse) LED pattern.  Call without s_mutex. */
static void apply_base_state(led_wifi_state_t state)
{
    if (s_gpio < 0) return;

    if (state == LED_WIFI_CONNECTED) {
        esp_timer_stop(s_breathe_timer);
        led_fade(DUTY_STEADY, 200);
    } else {
        restart_breathing(state);
    }
}

/* ── Breathe timer callback ──────────────────────────────────────────────── */

static void breathe_timer_cb(void *arg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Skip the step if a pulse is in progress or we've switched to steady */
    if (s_pulsing || s_wifi_state == LED_WIFI_CONNECTED || s_gpio < 0) {
        xSemaphoreGive(s_mutex);
        return;
    }

    uint32_t        target   = s_breathe_up ? DUTY_FULL : DUTY_OFF;
    led_wifi_state_t state   = s_wifi_state;
    s_breathe_up             = !s_breathe_up;
    xSemaphoreGive(s_mutex);

    int half_ms = breathe_half_ms(state);
    led_fade(target, half_ms);
    esp_timer_start_once(s_breathe_timer, (uint64_t)half_ms * 1000);
}

/* ── Pulse restore timer callback ────────────────────────────────────────── */

static void pulse_timer_cb(void *arg)
{
    led_wifi_state_t state;
    bool breathe_up;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pulsing  = false;
    state      = s_wifi_state;
    breathe_up = s_breathe_up;
    xSemaphoreGive(s_mutex);

    if (s_gpio < 0) return;

    if (state == LED_WIFI_CONNECTED) {
        esp_timer_stop(s_breathe_timer);
        led_fade(DUTY_STEADY, 200);
        return;
    }

    /* Resume breathing in the pre-set direction rather than restarting from
     * zero.  on_rx sets s_breathe_up=true  (LED dimmed → fade UP next).
     *         on_tx sets s_breathe_up=false (LED bright → fade DOWN next).
     * This avoids an instant black-cut followed by a 1.5 s dark period. */
    int half_ms = breathe_half_ms(state);
    uint32_t target = breathe_up ? DUTY_FULL : DUTY_OFF;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_breathe_up = !breathe_up;   /* flip so the next timer step goes the other way */
    xSemaphoreGive(s_mutex);

    esp_timer_stop(s_breathe_timer);
    led_fade(target, half_ms);
    esp_timer_start_once(s_breathe_timer, (uint64_t)half_ms * 1000);
}

/* ── WiFi / IP event handler ─────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    led_wifi_state_t new_state;

    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        new_state = LED_WIFI_CONNECTED;
    } else if (base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_START:
            new_state = LED_WIFI_AP;
            break;
        case WIFI_EVENT_STA_START:
        case WIFI_EVENT_STA_DISCONNECTED:
        case WIFI_EVENT_AP_STOP:
            new_state = LED_WIFI_DISCONNECTED;
            break;
        default:
            return;
        }
    } else {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed  = (new_state != s_wifi_state);
    bool pulsing  = s_pulsing;
    s_wifi_state  = new_state;
    xSemaphoreGive(s_mutex);

    if (changed && !pulsing)
        apply_base_state(new_state);
}

/* ── Hardware init helpers ───────────────────────────────────────────────── */

static void led_hw_init(int gpio)
{
    ledc_timer_config_t tmr = {
        .speed_mode      = LEDC_MODE_SEL,
        .timer_num       = LEDC_TMR,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tmr));

    ledc_channel_config_t ch = {
        .speed_mode = LEDC_MODE_SEL,
        .channel    = LEDC_CH,
        .timer_sel  = LEDC_TMR,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = gpio,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));

    if (!s_fade_installed) {
        ESP_ERROR_CHECK(ledc_fade_func_install(0));
        s_fade_installed = true;
    }

    s_gpio = gpio;
    ESP_LOGI(TAG, "LEDC configured on GPIO %d", gpio);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void status_led_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    esp_timer_create_args_t ba = { .callback = breathe_timer_cb, .name = "led_breathe" };
    esp_timer_create_args_t pa = { .callback = pulse_timer_cb,   .name = "led_pulse"   };
    ESP_ERROR_CHECK(esp_timer_create(&ba, &s_breathe_timer));
    ESP_ERROR_CHECK(esp_timer_create(&pa, &s_pulse_timer));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    config_lock();
    int gpio = g_config.gpio_status_led;
    config_unlock();

    if (gpio >= 0) {
        led_hw_init(gpio);
        apply_base_state(LED_WIFI_DISCONNECTED);
    }

    ESP_LOGI(TAG, "Status LED init (gpio=%d)", gpio);
}

void status_led_apply_config(void)
{
    config_lock();
    int new_gpio = g_config.gpio_status_led;
    config_unlock();

    if (new_gpio == s_gpio) return;

    /* Freeze timers before touching hardware */
    esp_timer_stop(s_breathe_timer);
    esp_timer_stop(s_pulse_timer);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pulsing    = false;
    s_breathe_up = true;
    led_wifi_state_t state = s_wifi_state;
    xSemaphoreGive(s_mutex);

    if (s_gpio >= 0) {
        ledc_stop(LEDC_MODE_SEL, LEDC_CH, 0);
        gpio_reset_pin(s_gpio);
        s_gpio = -1;
    }

    if (new_gpio >= 0) {
        led_hw_init(new_gpio);
        apply_base_state(state);
    }
}

void status_led_on_rx(void)
{
    if (s_gpio < 0) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool already_pulsing = s_pulsing;
    s_pulsing    = true;
    s_breathe_up = true;   /* LED going dark → restore by fading UP */
    xSemaphoreGive(s_mutex);

    if (!already_pulsing) {
        /* First packet in this burst: stop breathe and start the dim fade. */
        esp_timer_stop(s_breathe_timer);
        led_set(DUTY_OFF);
    }
    /* Every packet (including bursts): extend the restore deadline so the LED
     * stays dark for 100 ms after the *last* packet, not the first. */
    esp_timer_stop(s_pulse_timer);
    esp_timer_start_once(s_pulse_timer, (uint64_t)PULSE_MS * 1000);
}

void status_led_on_tx(void)
{
    if (s_gpio < 0) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool already_pulsing = s_pulsing;
    s_pulsing    = true;
    s_breathe_up = false;  /* LED going bright → restore by fading DOWN */
    xSemaphoreGive(s_mutex);

    if (!already_pulsing) {
        esp_timer_stop(s_breathe_timer);
        led_fade(DUTY_FULL, 20);
    }
    esp_timer_stop(s_pulse_timer);
    esp_timer_start_once(s_pulse_timer, (uint64_t)PULSE_MS * 1000);
}
