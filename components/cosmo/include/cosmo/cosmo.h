#pragma once

#include <stdint.h>
#include <stddef.h>

#ifndef HOST_TEST
#include "esp_err.h"
#else
typedef int esp_err_t;
#define ESP_OK   0
#define ESP_FAIL (-1)
#endif

/* ── Raw packet (CC1101 output) ──────────────────────────────────────────── */

#define COSMO_RAW_PACKET_LEN 9
/* Status bytes appended by CC1101 (APPEND_STATUS=1 by default) */
#define COSMO_STATUS_BYTES   2

typedef struct {
    uint8_t data[COSMO_RAW_PACKET_LEN];
    int8_t  rssi;   /* dBm, valid for received packets */
} cosmo_raw_packet_t;

/* ── Decoded packet ──────────────────────────────────────────────────────── */

typedef enum {
    PROTO_COSMO_1WAY = 0,
    PROTO_COSMO_2WAY = 1,
} cosmo_proto_t;

typedef enum {
    COSMO_BTN_NONE              = 0,
    COSMO_BTN_STOP              = 1,
    COSMO_BTN_UP                = 2,
    COSMO_BTN_UP_DOWN           = 3,
    COSMO_BTN_DOWN              = 4,
    COSMO_BTN_STOP_DOWN         = 5,
    COSMO_BTN_STOP_HOLD         = 6,
    COSMO_BTN_PROG              = 7,
    COSMO_BTN_STOP_UP           = 8,
    COSMO_BTN_OBSTRUCTION       = 9,
    COSMO_BTN_FEEDBACK_BOTTOM   = 10,
    COSMO_BTN_FEEDBACK_TOP      = 11,
    COSMO_BTN_FEEDBACK_COMFORT  = 12,
    COSMO_BTN_FEEDBACK_PARTIAL  = 13,
    COSMO_BTN_REQUEST_FEEDBACK  = 16,
    COSMO_BTN_TILT_INCREASE     = 17,
    COSMO_BTN_TILT_DECREASE     = 18,
    COSMO_BTN_SET_POSITION      = 19,
    COSMO_BTN_DETAILED_FEEDBACK = 21,
    COSMO_BTN_SET_TILT          = 23,
} cosmo_cmd_t;

typedef struct {
    cosmo_proto_t proto;
    cosmo_cmd_t   cmd;
    uint16_t      counter;
    uint32_t      serial;
    uint8_t       extra_payload;  /* valid for PROTO_COSMO_2WAY only */
    int8_t        rssi;           /* copied from raw packet */
    uint8_t       repeat;         /* TX only: extra repeat count (0 = send once) */
} cosmo_packet_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t cosmo_decode(const cosmo_raw_packet_t *raw, cosmo_packet_t *out);
esp_err_t cosmo_encode(const cosmo_packet_t *pkt, cosmo_raw_packet_t *out);
size_t    cosmo_packet_to_str(const cosmo_packet_t *pkt, char *buf, size_t len);
void      cosmo_packet_log(const cosmo_packet_t *pkt);
const char *cosmo_cmd_name(cosmo_cmd_t cmd);
