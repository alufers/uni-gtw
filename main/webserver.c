#include "webserver.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "webserver";

/* ── Config ──────────────────────────────────────────────────────────────── */

#define MAX_WS_CLIENTS    8
#define CONSOLE_BUF_SIZE  512
#define HISTORY_BUF_SIZE  4096

/* ── State ───────────────────────────────────────────────────────────────── */

static httpd_handle_t    s_server   = NULL;
static int               s_ws_fds[MAX_WS_CLIENTS];
static SemaphoreHandle_t s_ws_mutex;

/* Rolling console history – protected by s_ws_mutex */
static char  s_history[HISTORY_BUF_SIZE];
static int   s_history_write = 0;
static bool  s_history_full  = false;

/* ── JSON helpers ────────────────────────────────────────────────────────── */

/*
 * Append JSON-escaped `src` into `dst` (max cap-1 bytes, always NUL-terminated).
 * Escapes \n \r \t \" \\ and control characters as \uXXXX.
 */
static size_t json_escape(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (; *src && n + 7 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if      (c == '"')  { dst[n++] = '\\'; dst[n++] = '"';  }
        else if (c == '\\') { dst[n++] = '\\'; dst[n++] = '\\'; }
        else if (c == '\n') { dst[n++] = '\\'; dst[n++] = 'n';  }
        else if (c == '\r') { dst[n++] = '\\'; dst[n++] = 'r';  }
        else if (c == '\t') { dst[n++] = '\\'; dst[n++] = 't';  }
        else if (c < 0x20)  { n += snprintf(dst + n, cap - n, "\\u%04X", c); }
        else                { dst[n++] = (char)c; }
    }
    dst[n] = '\0';
    return n;
}

/* Build {"cmd":"console","payload":"<escaped msg>"} into buf. */
static size_t build_console_json(char *buf, size_t cap, const char *msg)
{
    size_t n = 0;
    n += snprintf(buf + n, cap - n, "{\"cmd\":\"console\",\"payload\":\"");
    n += json_escape(buf + n, cap - n, msg);
    n += snprintf(buf + n, cap - n, "\"}");
    return n;
}

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
 * Build a heap-allocated null-terminated string with ordered history
 * content (oldest → newest).  Returns NULL if nothing stored or OOM.
 * Caller must free().  Caller must NOT hold s_ws_mutex.
 */
static char *history_snapshot(size_t *out_len)
{
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);

    int len  = s_history_full ? HISTORY_BUF_SIZE : s_history_write;
    int tail = s_history_write;

    if (len == 0) {
        xSemaphoreGive(s_ws_mutex);
        *out_len = 0;
        return NULL;
    }

    char *buf = malloc((size_t)len + 1);
    if (buf) {
        if (!s_history_full) {
            memcpy(buf, s_history, (size_t)len);
        } else {
            int first = HISTORY_BUF_SIZE - tail;
            memcpy(buf, s_history + tail, (size_t)first);
            memcpy(buf + first, s_history, (size_t)tail);
        }
        buf[len] = '\0';
    }

    xSemaphoreGive(s_ws_mutex);
    *out_len = (size_t)len;
    return buf;
}

/* ── WebSocket async-send helpers ────────────────────────────────────────── */

/*
 * Work struct queued to the httpd task via httpd_queue_work.
 * The JSON payload is stored as a flexible array member to avoid
 * an extra allocation per send.
 */
typedef struct {
    httpd_handle_t hd;
    int            fd;
    char           json[]; /* NUL-terminated JSON payload */
} ws_send_work_t;

/*
 * Runs in the httpd task context.  Sends the frame and frees memory.
 * On error, triggers session close so ws_handler can clean up the fd.
 */
static void ws_send_work(void *arg)
{
    ws_send_work_t *w = arg;
    httpd_ws_frame_t pkt = {
        .final      = true,
        .fragmented = false,
        .type       = HTTPD_WS_TYPE_TEXT,
        .payload    = (uint8_t *)w->json,
        .len        = strlen(w->json),
    };
    esp_err_t err = httpd_ws_send_frame_async(w->hd, w->fd, &pkt);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WS send fd=%d failed (%d), closing session", w->fd, err);
        /* Remove from client list immediately */
        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_WS_CLIENTS; i++)
            if (s_ws_fds[i] == w->fd) s_ws_fds[i] = -1;
        xSemaphoreGive(s_ws_mutex);
        /* Let the httpd tear down the socket */
        httpd_sess_trigger_close(w->hd, w->fd);
    }
    free(w);
}

/*
 * Allocate a work item and queue it to the httpd task.
 * json must be NUL-terminated; it is copied into the work item.
 */
static void ws_queue_send(int fd, const char *json)
{
    if (!s_server) return;
    size_t jlen = strlen(json);
    ws_send_work_t *w = malloc(sizeof(*w) + jlen + 1);
    if (!w) return;
    w->hd = s_server;
    w->fd = fd;
    memcpy(w->json, json, jlen + 1);
    if (httpd_queue_work(s_server, ws_send_work, w) != ESP_OK)
        free(w);
}

/* ── WebSocket helpers ───────────────────────────────────────────────────── */

static void ws_remove_fd_locked(int fd)
{
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (s_ws_fds[i] == fd) s_ws_fds[i] = -1;
}

/*
 * Send the console history to a newly connected client.
 * Called from ws_handler (httpd task), queues additional httpd work.
 */
static void ws_send_history(int fd)
{
    size_t hist_len;
    char  *hist = history_snapshot(&hist_len);
    if (!hist) return;

    size_t json_cap = hist_len * 6 + 64;
    char  *json = malloc(json_cap);
    if (json) {
        build_console_json(json, json_cap, hist);
        ws_queue_send(fd, json);
        free(json);
    }
    free(hist);
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

        /* Queue history replay to this client */
        ws_send_history(fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
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

    /* Append to history and snapshot active fds under the mutex */
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    history_append_locked(msg, strlen(msg));
    history_append_locked("\n", 1);
    int fds[MAX_WS_CLIENTS];
    int nfds = 0;
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (s_ws_fds[i] != -1) fds[nfds++] = s_ws_fds[i];
    xSemaphoreGive(s_ws_mutex);

    if (nfds == 0 || !s_server) return;

    /* Build JSON once, then queue a send work item per connected client */
    size_t json_cap = strlen(msg) * 6 + 64;
    char  *json = malloc(json_cap);
    if (!json) return;
    build_console_json(json, json_cap, msg);

    for (int i = 0; i < nfds; i++)
        ws_queue_send(fds[i], json);

    free(json);
}

/* ── Early init (before WiFi) ────────────────────────────────────────────── */

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
