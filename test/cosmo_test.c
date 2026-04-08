#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#ifndef HOST_TEST
#define HOST_TEST
#endif
#include "../main/cosmo.h"

/* ── Test helpers ──────────────────────────────────────────────────────────── */

static int s_pass = 0;
static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); s_pass++; } \
    else      { printf("  FAIL: %s (line %d)\n", msg, __LINE__); s_fail++; } \
} while (0)

#define CHECK_EQ_U32(a, b, msg) do { \
    uint32_t _a = (a), _b = (b); \
    if (_a == _b) { printf("  PASS: %s (0x%08X)\n", msg, _a); s_pass++; } \
    else { printf("  FAIL: %s — got 0x%08X, expected 0x%08X (line %d)\n", \
                  msg, _a, _b, __LINE__); s_fail++; } \
} while (0)

#define CHECK_EQ_U16(a, b, msg) do { \
    uint16_t _a = (a), _b = (b); \
    if (_a == _b) { printf("  PASS: %s (%u)\n", msg, _a); s_pass++; } \
    else { printf("  FAIL: %s — got %u, expected %u (line %d)\n", \
                  msg, (unsigned)_a, (unsigned)_b, __LINE__); s_fail++; } \
} while (0)

#define CHECK_EQ_INT(a, b, msg) do { \
    int _a = (a), _b = (b); \
    if (_a == _b) { printf("  PASS: %s (%d)\n", msg, _a); s_pass++; } \
    else { printf("  FAIL: %s — got %d, expected %d (line %d)\n", \
                  msg, _a, _b, __LINE__); s_fail++; } \
} while (0)

/* Parse a hex string like "e439d15ab05ffb0024" into bytes. */
static void parse_hex(const char *hex, uint8_t *out, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned v;
        sscanf(hex + i * 2, "%02X", &v);
        out[i] = (uint8_t)v;
    }
}

/* ── Packet examples ────────────────────────────────────────────────────────
 *
 * All known-good received packets (9 data bytes only, no status bytes).
 *
 * STOP 2-way:  e439d15a b05ffb 00 24   counter=57
 * UP   2-way:  f93d692b b05ffb 00 22   counter=?
 * STOP 1-way:  29f5f67f f8488a 21 00   counter=?
 *
 * ─────────────────────────────────────────────────────────────────────────── */

/* ── Test: decode DOWN 2-way ────────────────────────────────────────────── *
 * Packet hex: e439d15a b05ffb 00 24
 * data[8]=0x24≠0 → 2-way, cmd_clear=4=DOWN, counter=57
 * Note: encrypted block is little-endian (data[0]=LSB).
 */
static void test_down_2way(void)
{
    printf("\n[test_down_2way]\n");

    cosmo_raw_packet_t raw = { .rssi = -70 };
    parse_hex("e439d15ab05ffb0024", raw.data, 9);

    cosmo_packet_t pkt;
    esp_err_t err = cosmo_decode(&raw, &pkt);

    CHECK(err == ESP_OK, "decode returns ESP_OK");
    CHECK_EQ_INT((int)pkt.proto, (int)PROTO_COSMO_2WAY, "proto=2WAY");
    CHECK_EQ_INT((int)pkt.cmd,   (int)COSMO_BTN_DOWN,   "cmd=DOWN");
    CHECK_EQ_U16(pkt.counter, 57, "counter=57");
    /* Serial upper 3 bytes from raw[4..6]: b0 5f fb → serial[31:8]=0xB05FFB */
    CHECK_EQ_U32((pkt.serial >> 8) & 0xFFFFFF, 0xB05FFB, "serial[31:8]=0xB05FFB");
    CHECK_EQ_INT((int)pkt.rssi, -70, "rssi copied");
}

/* ── Test: decode UP 2-way ──────────────────────────────────────────────── */
static void test_up_2way(void)
{
    printf("\n[test_up_2way]\n");

    cosmo_raw_packet_t raw = { .rssi = -65 };
    parse_hex("f93d692bb05ffb0022", raw.data, 9);

    cosmo_packet_t pkt;
    esp_err_t err = cosmo_decode(&raw, &pkt);

    CHECK(err == ESP_OK, "decode returns ESP_OK");
    CHECK_EQ_INT((int)pkt.proto, (int)PROTO_COSMO_2WAY, "proto=2WAY");
    CHECK_EQ_INT((int)pkt.cmd,   (int)COSMO_BTN_UP,     "cmd=UP");
    CHECK_EQ_U32((pkt.serial >> 8) & 0xFFFFFF, 0xB05FFB, "serial[31:8]=0xB05FFB");
}

/* ── Test: decode STOP 1-way ────────────────────────────────────────────── */
static void test_stop_1way(void)
{
    printf("\n[test_stop_1way]\n");

    cosmo_raw_packet_t raw = { .rssi = -60 };
    parse_hex("29f5f67ff8488a2100", raw.data, 9);

    cosmo_packet_t pkt;
    esp_err_t err = cosmo_decode(&raw, &pkt);

    CHECK(err == ESP_OK, "decode returns ESP_OK");
    CHECK_EQ_INT((int)pkt.proto, (int)PROTO_COSMO_1WAY, "proto=1WAY");
    CHECK_EQ_INT((int)pkt.cmd,   (int)COSMO_BTN_STOP,   "cmd=STOP");
    CHECK_EQ_U32((pkt.serial >> 8) & 0xFFFFFF, 0xF8488A, "serial[31:8]=0xF8488A");
}

/* ── Test: encode-decode roundtrip ─────────────────────────────────────── */
static void test_roundtrip(void)
{
    printf("\n[test_roundtrip]\n");

    cosmo_packet_t orig = {
        .proto         = PROTO_COSMO_1WAY,
        .cmd           = COSMO_BTN_UP,
        .counter       = 1234,
        .serial        = 0xDEADBE42,
        .extra_payload = 0,
        .rssi          = 0,
    };

    cosmo_raw_packet_t raw;
    esp_err_t err = cosmo_encode(&orig, &raw);
    CHECK(err == ESP_OK, "encode returns ESP_OK");

    cosmo_packet_t decoded;
    err = cosmo_decode(&raw, &decoded);
    CHECK(err == ESP_OK, "decode returns ESP_OK");
    CHECK_EQ_INT((int)decoded.proto,   (int)orig.proto,   "proto roundtrip");
    CHECK_EQ_INT((int)decoded.cmd,     (int)orig.cmd,     "cmd roundtrip");
    CHECK_EQ_U16(decoded.counter,      orig.counter,      "counter roundtrip");
    CHECK_EQ_U32(decoded.serial,       orig.serial,       "serial roundtrip");
}

/* ── Test: encode-decode roundtrip 2-way ──────────────────────────────── */
static void test_roundtrip_2way(void)
{
    printf("\n[test_roundtrip_2way]\n");

    cosmo_packet_t orig = {
        .proto         = PROTO_COSMO_2WAY,
        .cmd           = COSMO_BTN_DOWN,
        .counter       = 999,
        .serial        = 0xB05FFB77,
        .extra_payload = 0xAB,
        .rssi          = 0,
    };

    cosmo_raw_packet_t raw;
    esp_err_t err = cosmo_encode(&orig, &raw);
    CHECK(err == ESP_OK, "encode returns ESP_OK");
    CHECK(raw.data[8] != 0, "2-way: data[8] non-zero");

    cosmo_packet_t decoded;
    err = cosmo_decode(&raw, &decoded);
    CHECK(err == ESP_OK, "decode returns ESP_OK");
    CHECK_EQ_INT((int)decoded.proto,         (int)orig.proto,         "proto roundtrip");
    CHECK_EQ_INT((int)decoded.cmd,           (int)orig.cmd,           "cmd roundtrip");
    CHECK_EQ_U16(decoded.counter,            orig.counter,            "counter roundtrip");
    CHECK_EQ_U32(decoded.serial,             orig.serial,             "serial roundtrip");
    CHECK_EQ_INT((int)decoded.extra_payload, (int)orig.extra_payload, "extra_payload roundtrip");
}

/* ── Test: bad packet (command mismatch) → ESP_FAIL ────────────────────── */
static void test_bad_packet(void)
{
    printf("\n[test_bad_packet]\n");

    cosmo_raw_packet_t raw = { .rssi = 0 };
    /* All zeros — decrypted cmd will not match serial_cmd_byte cmd bits */
    memset(raw.data, 0, sizeof(raw.data));
    /* Corrupt a known-good packet by flipping the command bits in serial_cmd_byte */
    parse_hex("e439d15ab05ffb0024", raw.data, 9);
    raw.data[8] ^= 0x03;  /* flip low bits of serial_cmd_byte cmd field */

    cosmo_packet_t pkt;
    esp_err_t err = cosmo_decode(&raw, &pkt);
    CHECK(err == ESP_FAIL, "corrupted packet returns ESP_FAIL");
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== cosmo unit tests ===\n");

    test_down_2way();
    test_up_2way();
    test_stop_1way();
    test_roundtrip();
    test_roundtrip_2way();
    test_bad_packet();

    printf("\n=== Results: %d passed, %d failed ===\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
