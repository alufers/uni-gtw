#include "webserver.h"
#include "channel.h"
#include "config.h"
#include "wifi_manager.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cJSON.h"
#include "cosmo/cosmo.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "radio.h"

static const char *TAG = "webserver";

extern radio_state_t g_radio_state;

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

static void history_append_locked(const char *str, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        s_history[s_history_write] = str[i];
        s_history_write = (s_history_write + 1) % HISTORY_BUF_SIZE;
        if (s_history_write == 0)
            s_history_full = true;
    }
}

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

typedef struct {
    httpd_handle_t hd;
    int            fd;
    char           json[];
} ws_send_work_t;

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
        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_WS_CLIENTS; i++)
            if (s_ws_fds[i] == w->fd) s_ws_fds[i] = -1;
        xSemaphoreGive(s_ws_mutex);
        httpd_sess_trigger_close(w->hd, w->fd);
    }
    free(w);
}

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

static void ws_send_history(int fd)
{
    size_t hist_len;
    char  *hist = history_snapshot(&hist_len);
    if (!hist) return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "console");
    cJSON_AddStringToObject(root, "payload", hist);
    free(hist);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        ws_queue_send(fd, json);
        free(json);
    }
}

/* ── WS broadcast / send helpers (public) ───────────────────────────────── */

void webserver_ws_broadcast_json(const char *json)
{
    if (!s_server) return;
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    int fds[MAX_WS_CLIENTS];
    int nfds = 0;
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (s_ws_fds[i] != -1) fds[nfds++] = s_ws_fds[i];
    xSemaphoreGive(s_ws_mutex);

    for (int i = 0; i < nfds; i++)
        ws_queue_send(fds[i], json);
}

void webserver_ws_send_json_to_fd(int fd, const char *json)
{
    ws_queue_send(fd, json);
}

/* ── Status broadcast ────────────────────────────────────────────────────── */

static void build_and_send_status(int fd /* -1 = broadcast */)
{
    wifi_mgr_mode_t mode = wifi_manager_get_mode();
    int8_t rssi;
    bool   has_rssi = wifi_manager_get_rssi(&rssi);
    time_t now      = time(NULL);
    int64_t uptime_us = esp_timer_get_time();
    int64_t uptime_s  = uptime_us / 1000000LL;

    char   sta_ssid[33] = {0};
    bool   has_ssid = wifi_manager_get_sta_ssid(sta_ssid, sizeof(sta_ssid));

    cJSON *root    = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "status");
    cJSON_AddNumberToObject(payload, "uptime",    (double)uptime_s);
    cJSON_AddNumberToObject(payload, "time",      (double)now);
    const char *radio_status_str =
        (g_radio_state == RADIO_STATE_OK)             ? "ok" :
        (g_radio_state == RADIO_STATE_ERROR)          ? "error" : "not_configured";

    cJSON_AddStringToObject(payload, "wifi_mode",     (mode == WIFI_MGR_MODE_AP) ? "ap" : "sta");
    cJSON_AddStringToObject(payload, "radio_status",  radio_status_str);
    if (has_rssi)
        cJSON_AddNumberToObject(payload, "wifi_rssi", rssi);
    else
        cJSON_AddNullToObject(payload, "wifi_rssi");
    if (has_ssid)
        cJSON_AddStringToObject(payload, "wifi_ssid", sta_ssid);
    else
        cJSON_AddNullToObject(payload, "wifi_ssid");
    cJSON_AddItemToObject(root, "payload", payload);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    if (fd == -1)
        webserver_ws_broadcast_json(json);
    else
        webserver_ws_send_json_to_fd(fd, json);
    free(json);
}

static void status_timer_cb(void *arg)
{
    (void)arg;
    build_and_send_status(-1);
}

void webserver_start_status_timer(void)
{
    esp_timer_handle_t t;
    esp_timer_create_args_t args = {
        .callback = status_timer_cb,
        .name     = "ws_status",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &t));
    ESP_ERROR_CHECK(esp_timer_start_periodic(t, 10ULL * 1000 * 1000));
}

/* ── WiFi scan task ──────────────────────────────────────────────────────── */

typedef struct {
    int fd;
} scan_task_arg_t;

static void scan_task(void *arg)
{
    scan_task_arg_t *a = (scan_task_arg_t *)arg;
    int fd = a->fd;
    free(a);

    wifi_scan_result_t results[WIFI_SCAN_MAX_APS];
    uint16_t count = WIFI_SCAN_MAX_APS;
    esp_err_t err  = wifi_manager_scan(results, &count);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "wifi_scan_result");
    cJSON *arr = cJSON_CreateArray();
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < count; i++) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "ssid", results[i].ssid);
            cJSON_AddNumberToObject(entry, "rssi", results[i].rssi);
            cJSON_AddNumberToObject(entry, "auth", results[i].authmode);
            cJSON_AddItemToArray(arr, entry);
        }
    }
    cJSON_AddItemToObject(root, "payload", arr);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        webserver_ws_send_json_to_fd(fd, json);
        free(json);
    }

    vTaskDelete(NULL);
}

/* ── Incoming WS message dispatch ───────────────────────────────────────── */

static void ws_dispatch(int fd, const char *text)
{
    cJSON *root = cJSON_Parse(text);
    if (!root) return;

    const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(root, "cmd"));
    if (!cmd) { cJSON_Delete(root); return; }

    if (strcmp(cmd, "create_channel") == 0) {
        const char *name      = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
        const char *proto_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "proto"));
        cosmo_proto_t proto = (proto_str && strcmp(proto_str, "2way") == 0)
                              ? PROTO_COSMO_2WAY : PROTO_COSMO_1WAY;
        channel_create(name ? name : "Unnamed", proto);

    } else if (strcmp(cmd, "delete_channel") == 0) {
        cJSON *serial_j = cJSON_GetObjectItem(root, "serial");
        if (!cJSON_IsNumber(serial_j)) { cJSON_Delete(root); return; }
        channel_delete((uint32_t)cJSON_GetNumberValue(serial_j));

    } else if (strcmp(cmd, "channel_cmd") == 0) {
        cJSON *serial_j = cJSON_GetObjectItem(root, "serial");
        if (!cJSON_IsNumber(serial_j)) { cJSON_Delete(root); return; }
        uint32_t serial = (uint32_t)cJSON_GetNumberValue(serial_j);

        const char *cmd_name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "cmd_name"));
        if (!cmd_name) { cJSON_Delete(root); return; }

        uint8_t extra_payload = 0;
        cJSON *extra_j = cJSON_GetObjectItem(root, "extra_payload");
        if (cJSON_IsNumber(extra_j))
            extra_payload = (uint8_t)(int)cJSON_GetNumberValue(extra_j);

        cosmo_cmd_t cosmo_cmd;
        if      (strcmp(cmd_name, "UP")               == 0) cosmo_cmd = COSMO_BTN_UP;
        else if (strcmp(cmd_name, "DOWN")             == 0) cosmo_cmd = COSMO_BTN_DOWN;
        else if (strcmp(cmd_name, "STOP")             == 0) cosmo_cmd = COSMO_BTN_STOP;
        else if (strcmp(cmd_name, "UP_DOWN")          == 0) cosmo_cmd = COSMO_BTN_UP_DOWN;
        else if (strcmp(cmd_name, "STOP_DOWN")        == 0) cosmo_cmd = COSMO_BTN_STOP_DOWN;
        else if (strcmp(cmd_name, "STOP_HOLD")        == 0) cosmo_cmd = COSMO_BTN_STOP_HOLD;
        else if (strcmp(cmd_name, "PROG")             == 0) cosmo_cmd = COSMO_BTN_PROG;
        else if (strcmp(cmd_name, "STOP_UP")          == 0) cosmo_cmd = COSMO_BTN_STOP_UP;
        else if (strcmp(cmd_name, "REQUEST_FEEDBACK") == 0) cosmo_cmd = COSMO_BTN_REQUEST_FEEDBACK;
        else if (strcmp(cmd_name, "REQUEST_POSITION") == 0) cosmo_cmd = COSMO_BTN_REQUEST_POSITION;
        else if (strcmp(cmd_name, "SET_POSITION")     == 0) cosmo_cmd = COSMO_BTN_SET_POSITION;
        else if (strcmp(cmd_name, "SET_TILT")         == 0) cosmo_cmd = COSMO_BTN_SET_TILT;
        else { cJSON_Delete(root); return; }

        channel_send_cmd(serial, cosmo_cmd, extra_payload);

    } else if (strcmp(cmd, "wifi_scan") == 0) {
        scan_task_arg_t *a = malloc(sizeof(*a));
        if (a) {
            a->fd = fd;
            if (xTaskCreate(scan_task, "wifi_scan", 4096, a, 5, NULL) != pdPASS)
                free(a);
        }

    } else if (strcmp(cmd, "wifi_set_credentials") == 0) {
        const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ssid"));
        const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(root, "password"));
        wifi_manager_set_credentials(ssid ? ssid : "", pass ? pass : "");
    }

    cJSON_Delete(root);
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

        ws_send_history(fd);
        channel_send_all(fd);
        build_and_send_status(fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;

    uint8_t *buf = NULL;
    if (frame.len > 0) {
        buf = calloc(1, frame.len + 1);
        if (!buf) return ESP_ERR_NO_MEM;
        frame.payload = buf;
        ret = httpd_ws_recv_frame(req, &frame, frame.len);
        if (ret != ESP_OK) {
            free(buf);
            return ret;
        }
    }

    if (frame.type == HTTPD_WS_TYPE_TEXT && buf) {
        ws_dispatch(httpd_req_to_sockfd(req), (char *)buf);
    } else if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        ws_remove_fd_locked(fd);
        xSemaphoreGive(s_ws_mutex);
        ESP_LOGI(TAG, "WS client disconnected fd=%d", fd);
    }

    free(buf);
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

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    history_append_locked(msg, strlen(msg));
    history_append_locked("\n", 1);
    int fds[MAX_WS_CLIENTS];
    int nfds = 0;
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (s_ws_fds[i] != -1) fds[nfds++] = s_ws_fds[i];
    xSemaphoreGive(s_ws_mutex);

    if (nfds == 0 || !s_server) return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "console");
    cJSON_AddStringToObject(root, "payload", msg);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    for (int i = 0; i < nfds; i++)
        ws_queue_send(fds[i], json);

    free(json);
}

/* ── Settings REST handlers ──────────────────────────────────────────────── */

static char *build_settings_json(void)
{
    gateway_config_t cfg;
    config_get(&cfg);

    cJSON *root   = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "hostname", cfg.hostname);

    cJSON *mqtt_j = cJSON_CreateObject();
    cJSON_AddBoolToObject  (mqtt_j, "enabled",  cfg.mqtt.enabled);
    cJSON_AddStringToObject(mqtt_j, "broker",   cfg.mqtt.broker);
    cJSON_AddNumberToObject(mqtt_j, "port",     cfg.mqtt.port);
    cJSON_AddStringToObject(mqtt_j, "username", cfg.mqtt.username);
    cJSON_AddStringToObject(mqtt_j, "password", cfg.mqtt.password);
    cJSON_AddItemToObject(root, "mqtt", mqtt_j);

    cJSON *radio_j = cJSON_CreateObject();
    cJSON_AddBoolToObject  (radio_j, "enabled",    cfg.radio.enabled);
    cJSON_AddNumberToObject(radio_j, "gpio_miso",  cfg.radio.gpio_miso);
    cJSON_AddNumberToObject(radio_j, "gpio_mosi",  cfg.radio.gpio_mosi);
    cJSON_AddNumberToObject(radio_j, "gpio_sck",   cfg.radio.gpio_sck);
    cJSON_AddNumberToObject(radio_j, "gpio_csn",   cfg.radio.gpio_csn);
    cJSON_AddNumberToObject(radio_j, "gpio_gdo0",  cfg.radio.gpio_gdo0);
    cJSON_AddNumberToObject(radio_j, "spi_freq_hz",cfg.radio.spi_freq_hz);
    cJSON_AddItemToObject(root, "radio", radio_j);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    char *json = build_settings_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_OK;
    }

    char *buf = malloc((size_t)req->content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_OK;
    }
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) { free(buf); return received == 0 ? ESP_OK : ESP_FAIL; }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_OK;
    }

    /* Hostname */
    const char *hostname = cJSON_GetStringValue(cJSON_GetObjectItem(root, "hostname"));
    if (hostname && hostname[0]) {
        config_set_hostname(hostname);
        mdns_hostname_set(hostname);
        esp_netif_t *sta = wifi_manager_get_sta_netif();
        if (sta) esp_netif_set_hostname(sta, hostname);
        ESP_LOGI(TAG, "Hostname updated to: %s", hostname);
    }

    /* MQTT */
    cJSON *mqtt_j = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt_j)) {
        gateway_config_t current;
        config_get(&current);

        cJSON *en = cJSON_GetObjectItem(mqtt_j, "enabled");
        bool enabled = cJSON_IsBool(en) ? cJSON_IsTrue(en) : current.mqtt.enabled;

        const char *broker = cJSON_GetStringValue(cJSON_GetObjectItem(mqtt_j, "broker"));
        if (!broker) broker = current.mqtt.broker;

        cJSON *port_j = cJSON_GetObjectItem(mqtt_j, "port");
        uint16_t port = cJSON_IsNumber(port_j)
                      ? (uint16_t)cJSON_GetNumberValue(port_j)
                      : current.mqtt.port;

        const char *username = cJSON_GetStringValue(cJSON_GetObjectItem(mqtt_j, "username"));
        if (!username) username = current.mqtt.username;

        const char *password = cJSON_GetStringValue(cJSON_GetObjectItem(mqtt_j, "password"));
        if (!password) password = current.mqtt.password;

        config_set_mqtt(broker, port, username, password, enabled);
    }

    /* Radio */
    cJSON *radio_j = cJSON_GetObjectItem(root, "radio");
    if (cJSON_IsObject(radio_j)) {
        gateway_config_t current;
        config_get(&current);

        cJSON *en = cJSON_GetObjectItem(radio_j, "enabled");
        bool enabled = cJSON_IsBool(en) ? cJSON_IsTrue(en) : current.radio.enabled;

#define GET_INT(key, field) \
        cJSON *_j_##field = cJSON_GetObjectItem(radio_j, key); \
        int field = cJSON_IsNumber(_j_##field) ? (int)cJSON_GetNumberValue(_j_##field) : current.radio.field;
        GET_INT("gpio_miso",   gpio_miso)
        GET_INT("gpio_mosi",   gpio_mosi)
        GET_INT("gpio_sck",    gpio_sck)
        GET_INT("gpio_csn",    gpio_csn)
        GET_INT("gpio_gdo0",   gpio_gdo0)
        GET_INT("spi_freq_hz", spi_freq_hz)
#undef GET_INT

        config_set_radio(enabled, gpio_miso, gpio_mosi, gpio_sck,
                         gpio_csn, gpio_gdo0, spi_freq_hz);

        /* Apply immediately — reinit hardware only if config actually changed */
        esp_err_t radio_err = radio_apply_config();
        if (radio_err == ESP_ERR_NOT_SUPPORTED)
            g_radio_state = RADIO_STATE_NOT_CONFIGURED;
        else if (radio_err != ESP_OK)
            g_radio_state = RADIO_STATE_ERROR;
        else
            g_radio_state = RADIO_STATE_OK;
        ESP_LOGI(TAG, "Radio state after apply: %d", (int)g_radio_state);
    }

    cJSON_Delete(root);

    /* Respond with the final saved state */
    char *json = build_settings_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    free(json);
    return ESP_OK;
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
    if (s_server) return; /* idempotent */

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
    static const httpd_uri_t uri_settings_get = {
        .uri    = "/api/settings",
        .method = HTTP_GET,
        .handler = settings_get_handler,
    };
    static const httpd_uri_t uri_settings_post = {
        .uri    = "/api/settings",
        .method = HTTP_POST,
        .handler = settings_post_handler,
    };

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_js);
    httpd_register_uri_handler(s_server, &uri_css);
    httpd_register_uri_handler(s_server, &uri_ws);
    httpd_register_uri_handler(s_server, &uri_settings_get);
    httpd_register_uri_handler(s_server, &uri_settings_post);

    ESP_LOGI(TAG, "HTTP server started");
}
