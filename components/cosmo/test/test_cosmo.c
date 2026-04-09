#include "unity.h"
#include "cosmo/cosmo.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Parse a hex string ("e439d15ab05ffb0024") into bytes. */
static void parse_hex(const char *hex, uint8_t *out, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned v;
        sscanf(hex + i * 2, "%02X", &v);
        out[i] = (uint8_t)v;
    }
}

/* ── Test: decode DOWN 2-way ────────────────────────────────────────────── *
 * Packet: e439d15a b05ffb 00 24
 * data[8]=0x24 → 2-way, cmd_clear=4=DOWN, counter=57.
 * Note: encrypted block is little-endian (data[0]=LSB).
 */
void test_down_2way(void)
{
    cosmo_raw_packet_t raw = { .rssi = -70 };
    parse_hex("e439d15ab05ffb0024", raw.data, 9);

    cosmo_packet_t pkt;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_decode(&raw, &pkt));
    TEST_ASSERT_EQUAL(PROTO_COSMO_2WAY, pkt.proto);
    TEST_ASSERT_EQUAL(COSMO_BTN_DOWN, pkt.cmd);
    TEST_ASSERT_EQUAL_UINT16(57, pkt.counter);
    /* Serial upper 3 bytes from raw[4..6] = b0 5f fb */
    TEST_ASSERT_EQUAL_UINT32(0xB05FFB, (pkt.serial >> 8) & 0xFFFFFF);
    TEST_ASSERT_EQUAL_INT8(-70, pkt.rssi);
}

/* ── Test: decode UP 2-way ──────────────────────────────────────────────── *
 * Packet: f93d692b b05ffb 00 22
 * data[8]=0x22 → 2-way, cmd_clear=2=UP.
 */
void test_up_2way(void)
{
    cosmo_raw_packet_t raw = { .rssi = -65 };
    parse_hex("f93d692bb05ffb0022", raw.data, 9);

    cosmo_packet_t pkt;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_decode(&raw, &pkt));
    TEST_ASSERT_EQUAL(PROTO_COSMO_2WAY, pkt.proto);
    TEST_ASSERT_EQUAL(COSMO_BTN_UP, pkt.cmd);
    TEST_ASSERT_EQUAL_UINT32(0xB05FFB, (pkt.serial >> 8) & 0xFFFFFF);
}

/* ── Test: decode STOP 1-way ────────────────────────────────────────────── *
 * Packet: 29f5f67f f8488a 21 00
 * data[8]=0x00 → 1-way, data[7]=0x21 serial_cmd_byte, cmd_clear=1=STOP.
 */
void test_stop_1way(void)
{
    cosmo_raw_packet_t raw = { .rssi = -60 };
    parse_hex("29f5f67ff8488a2100", raw.data, 9);

    cosmo_packet_t pkt;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_decode(&raw, &pkt));
    TEST_ASSERT_EQUAL(PROTO_COSMO_1WAY, pkt.proto);
    TEST_ASSERT_EQUAL(COSMO_BTN_STOP, pkt.cmd);
    TEST_ASSERT_EQUAL_UINT32(0xF8488A, (pkt.serial >> 8) & 0xFFFFFF);
}

/* ── Test: encode-decode roundtrip (1-way) ──────────────────────────────── */
void test_roundtrip_1way(void)
{
    cosmo_packet_t orig = {
        .proto   = PROTO_COSMO_1WAY,
        .cmd     = COSMO_BTN_UP,
        .counter = 1234,
        .serial  = 0xDEADBE42,
    };

    cosmo_raw_packet_t raw;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_encode(&orig, &raw));
    TEST_ASSERT_EQUAL_UINT8(0x00, raw.data[8]);  /* 1-way: data[8]=0 */

    cosmo_packet_t decoded;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_decode(&raw, &decoded));
    TEST_ASSERT_EQUAL(orig.proto,   decoded.proto);
    TEST_ASSERT_EQUAL(orig.cmd,     decoded.cmd);
    TEST_ASSERT_EQUAL_UINT16(orig.counter, decoded.counter);
    TEST_ASSERT_EQUAL_UINT32(orig.serial,  decoded.serial);
}

/* ── Test: encode-decode roundtrip (2-way) ──────────────────────────────── */
void test_roundtrip_2way(void)
{
    cosmo_packet_t orig = {
        .proto         = PROTO_COSMO_2WAY,
        .cmd           = COSMO_BTN_DOWN,
        .counter       = 999,
        .serial        = 0xB05FFB77,
        .extra_payload = 0xAB,
    };

    cosmo_raw_packet_t raw;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_encode(&orig, &raw));
    TEST_ASSERT_NOT_EQUAL(0, raw.data[8]);  /* 2-way: data[8] ≠ 0 */

    cosmo_packet_t decoded;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_decode(&raw, &decoded));
    TEST_ASSERT_EQUAL(orig.proto,         decoded.proto);
    TEST_ASSERT_EQUAL(orig.cmd,           decoded.cmd);
    TEST_ASSERT_EQUAL_UINT16(orig.counter,       decoded.counter);
    TEST_ASSERT_EQUAL_UINT32(orig.serial,        decoded.serial);
    TEST_ASSERT_EQUAL_UINT8(orig.extra_payload, decoded.extra_payload);
}

/* ── Test: corrupted command byte returns ESP_FAIL ──────────────────────── */
void test_bad_packet(void)
{
    cosmo_raw_packet_t raw = { .rssi = 0 };
    parse_hex("e439d15ab05ffb0024", raw.data, 9);
    raw.data[8] ^= 0x03;  /* flip cmd bits in serial_cmd_byte */

    cosmo_packet_t pkt;
    TEST_ASSERT_EQUAL(ESP_FAIL, cosmo_decode(&raw, &pkt));
}

/* ── Test: cmd name lookup ──────────────────────────────────────────────── */
void test_cmd_name(void)
{
    TEST_ASSERT_EQUAL_STRING("COSMO_BTN_UP",   cosmo_cmd_name(COSMO_BTN_UP));
    TEST_ASSERT_EQUAL_STRING("COSMO_BTN_DOWN", cosmo_cmd_name(COSMO_BTN_DOWN));
    TEST_ASSERT_EQUAL_STRING("COSMO_BTN_STOP", cosmo_cmd_name(COSMO_BTN_STOP));
    TEST_ASSERT_EQUAL_STRING("COSMO_BTN_UNKNOWN", cosmo_cmd_name((cosmo_cmd_t)31));
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_down_2way);
    RUN_TEST(test_up_2way);
    RUN_TEST(test_stop_1way);
    RUN_TEST(test_roundtrip_1way);
    RUN_TEST(test_roundtrip_2way);
    RUN_TEST(test_bad_packet);
    RUN_TEST(test_cmd_name);
    return UNITY_END();
}
