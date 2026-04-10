#pragma once

void webserver_early_init(void);
void webserver_start(void);
void webserver_start_status_timer(void);
void gtw_console_log(const char *fmt, ...);
void webserver_ws_broadcast_json(const char *json);
void webserver_ws_send_json_to_fd(int fd, const char *json);
