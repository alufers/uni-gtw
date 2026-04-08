#include "webserver.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "json_generator.h"

static const char *TAG = "webserver";

/* ── Config ──────────────────────────────────────────────────────────────── */

#define MAX_WS_CLIENTS    8
#define CONSOLE_BUF_SIZE  512
#define HISTORY_BUF_SIZE  4096

/* ── State ───────────────────────────────────────────────────────────────── */

static httpd_handle_t   s_server   = NULL;
static int              s_ws_fds[MAX_WS_CLIENTS];
static SemaphoreHandle_t s_ws_mutex;

/* Rolling console history – protected by s_ws_mutex */
static char  s_history[HISTORY_BUF_SIZE];
static int   s_history_write = 0;
static bool  s_history_full  = false;

/* ── Embedded file handlers ──────────────────────────────────────────────── */

#define STATIC_FILE_HANDLER(sym, ctype)                                          \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_start");           \
    extern const uint8_t sym##_end[]   asm("_binary_" #sym "_end");             \
    static esp_err_t sym##_handler(httpd_req_t *req)                            \
    {                                                                            \
        httpd_resp_set_type(req, ctype);                                         \
        httpd_resp_send(req, (const char *)sym##_start,                          \
                        sym##_end - sym##_start);                                \
        return ESP_OK;                                                           \
    }

STATIC_FILE_HANDLER(index_html, "text/html")
STATIC_FILE_HANDLER(app_js,    "application/javascript")
STATIC_FILE_HANDLER(app_css,   "text/css")

/* ── History buffer ──────────────────────────────────────────────────────── */

/* Append bytes to the circular history buffer (caller holds s_ws_mutex). */
static void history_append_locked(const char *str, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        s_history[s_history_write] = str[i];
        s_history_write = (s_history_write + 1) % HISTORY_BUF_SIZE;
        if (s_history_write == 0)
            s_history_full = true;
    }
}

/*
 * Build a heap-allocated null-terminated string with the ordered history
 * content (oldest → newest). Returns NULL if nothing stored or OOM.
 * Caller must free the returned buffer.
 * Caller must NOT hold s_ws_mutex.
 */
static char *history_snapshot(size_t *out_len)
{
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);

    int   len  = s_history_full ? HISTORY_BUF_SIZE : s_history_write;
    int   tail = s_history_write;       /* oldest byte when full */

    if (len == 0) {
        xSemaphoreGive(s_ws_mutex);
        *out_len = 0;
        return NULL;
    }

    char *buf = malloc(len + 1);
    if (buf) {
        if (!s_history_full) {
            memcpy(buf, s_history, len);
        } else {
            int first = HISTORY_BUF_SIZE - tail;
            memcpy(buf, s_history + tail, first);
            memcpy(buf + first, s_history, tail);
        }
        buf[len] = '\0';
    }

    xSemaphoreGive(s_ws_mutex);

    *out_len = len;
    return buf;
}

/* ── WebSocket helpers ───────────────────────────────────────────────────── */

static void ws_remove_fd_locked(int fd)
{
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == fd)
            s_ws_fds[i] = -1;
    }
}

/* Callback invoked by httpd after the async history send completes. */
static void history_send_done(esp_err_t err, int socket, void *arg)
{
    (void)err;
    (void)socket;
    free(arg);          /* arg == malloc'd JSON buffer */
}

/* Queue an async send of the console history to the given fd.
 * Safe to call from within the httpd task (GET handler). */
static void ws_send_history_async(int fd)
{
    size_t history_len;
    char  *history = history_snapshot(&history_len);
    if (!history) return;

    /* Build JSON: {"cmd":"console","payload":"<history>"}.
     * Worst-case escaped payload is 6x (e.g. every char → \uXXXX). */
    size_t json_cap = history_len * 6 + 64;
    char  *json_buf = malloc(json_cap);
    if (!json_buf) { free(history); return; }

    json_gen_str_t jstr;
    json_gen_str_start(&jstr, json_buf, json_cap, NULL, NULL);
    json_gen_start_object(&jstr);
    json_gen_obj_set_string(&jstr, "cmd",     "console");
    json_gen_obj_set_string(&jstr, "payload", history);
    json_gen_end_object(&jstr);
    json_gen_str_end(&jstr);

    free(history);

    httpd_ws_frame_t pkt = {
        .final      = true,
        .fragmented = false,
        .type       = HTTPD_WS_TYPE_TEXT,
        .payload    = (uint8_t *)json_buf,
        .len        = strlen(json_buf),
    };

    /* httpd_ws_send_data_async deep-copies the frame struct (shallow payload
     * ptr) and invokes history_send_done once sent, which frees json_buf. */
    esp_err_t err = httpd_ws_send_data_async(s_server, fd, &pkt,
                                             history_send_done, json_buf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to queue history send: %d", err);
        free(json_buf);
    }
}

/* ── WebSocket handler ───────────────────────────────────────────────────── */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);

        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        bool added = false;
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (s_ws_fds[i] == -1) {
                s_ws_fds[i] = fd;
                added = true;
                break;
            }
        }
        xSemaphoreGive(s_ws_mutex);

        if (!added) {
            ESP_LOGW(TAG, "WS client list full, dropping fd=%d", fd);
            return ESP_OK;
        }
        ESP_LOGI(TAG, "WS client connected fd=%d", fd);

        /* Send buffered history to the new client. */
        ws_send_history_async(fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT};
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;

    if (frame.len > 0) {
        uint8_t *buf = calloc(1, frame.len + 1);
        if (!buf) return ESP_ERR_NO_MEM;
        frame.payload = buf;
        ret = httpd_ws_recv_frame(req, &frame, frame.len);
        free(buf);
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        ws_remove_fd_locked(fd);
        xSemaphoreGive(s_ws_mutex);
        ESP_LOGI(TAG, "WS client disconnected fd=%d", fd);
    }

    return ret;
}

/* ── gtw_console_log ─────────────────────────────────────────────────────── */

void gtw_console_log(const char *fmt, ...)
{
    char msg[CONSOLE_BUF_SIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    ESP_LOGI("gtw", "%s", msg);

    /* Append to rolling history (msg + newline). */
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    history_append_locked(msg, strlen(msg));
    history_append_locked("\n", 1);
    xSemaphoreGive(s_ws_mutex);

    /* Build JSON and broadcast to connected WS clients. */
    char json_buf[CONSOLE_BUF_SIZE + 64];
    json_gen_str_t jstr;
    json_gen_str_start(&jstr, json_buf, sizeof(json_buf), NULL, NULL);
    json_gen_start_object(&jstr);
    json_gen_obj_set_string(&jstr, "cmd",     "console");
    json_gen_obj_set_string(&jstr, "payload", msg);
    json_gen_end_object(&jstr);
    json_gen_str_end(&jstr);

    httpd_ws_frame_t ws_pkt = {
        .final      = true,
        .fragmented = false,
        .type       = HTTPD_WS_TYPE_TEXT,
        .payload    = (uint8_t *)json_buf,
        .len        = strlen(json_buf),
    };

    if (!s_server) return;

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == -1) continue;
        esp_err_t err = httpd_ws_send_frame_async(s_server, s_ws_fds[i], &ws_pkt);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "WS send failed fd=%d, removing", s_ws_fds[i]);
            s_ws_fds[i] = -1;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}


void webserver_early_init(void)
{
    s_ws_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        s_ws_fds[i] = -1;
}

/* ── webserver_start ─────────────────────────────────────────────────────── */

void webserver_start(void)
{


    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    static const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = index_html_handler,
    };
    static const httpd_uri_t uri_js = {
        .uri = "/app.js", .method = HTTP_GET, .handler = app_js_handler,
    };
    static const httpd_uri_t uri_css = {
        .uri = "/app.css", .method = HTTP_GET, .handler = app_css_handler,
    };
    static const httpd_uri_t uri_ws = {
        .uri          = "/ws",
        .method       = HTTP_GET,
        .handler      = ws_handler,
        .is_websocket = true,
    };

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_js);
    httpd_register_uri_handler(s_server, &uri_css);
    httpd_register_uri_handler(s_server, &uri_ws);

    ESP_LOGI(TAG, "HTTP server started");
}
