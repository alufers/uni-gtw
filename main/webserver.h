#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "cosmo/cosmo.h"

void webserver_early_init(void);
void webserver_start(void);
void webserver_start_status_timer(void);
void gtw_console_log(const char *fmt, ...);
void webserver_ws_broadcast_json(const char *json);
void webserver_ws_send_json_to_fd(int fd, const char *json);

/**
 * Broadcast a packet_rx or packet_tx WebSocket message to all clients.
 * raw_bytes/raw_len: the raw CC1101 bytes before decoding.
 * valid: true if cosmo_decode succeeded.
 * pkt: decoded packet (ignored when valid=false).
 */
void webserver_ws_broadcast_packet(bool is_tx,
                                   const uint8_t *raw_bytes, int raw_len,
                                   bool valid, const cosmo_packet_t *pkt);
