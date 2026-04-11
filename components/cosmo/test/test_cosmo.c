#include "unity.h"
#include "cosmo/cosmo.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Parse a hex string into bytes. */
static void parse_hex(const char *hex, uint8_t *out, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned v;
        sscanf(hex + i * 2, "%02X", &v);
        out[i] = (uint8_t)v;
    }
}

/* ── Test: decode FEEDBACK_PARTIAL 2-way — RX capture ───────────────────────
 * BIN: 11011100 10001000 10000111 11011110 10011100 00000100 00011100 00100111 00001101
 * PKT proto=PROTO_COSMO_2WAY cmd=COSMO_BTN_FEEDBACK_PARTIAL(13)
 *     serial=0x9C041C00 counter=98 rssi=-52 dBm extra=0x27
 */
void test_feedback_partial_2way(void)
{
    cosmo_raw_packet_t raw = { .rssi = -52 };
    parse_hex("DC8887DE9C041C270D", raw.data, 9);

    cosmo_packet_t pkt;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_decode(&raw, &pkt));
    TEST_ASSERT_EQUAL(PROTO_COSMO_2WAY,          pkt.proto);
    TEST_ASSERT_EQUAL(COSMO_BTN_FEEDBACK_PARTIAL, pkt.cmd);
    TEST_ASSERT_EQUAL_UINT32(0x9C041C00,          pkt.serial);
    TEST_ASSERT_EQUAL_UINT16(98,                  pkt.counter);
    TEST_ASSERT_EQUAL_UINT8(0x27,                 pkt.extra_payload);
    TEST_ASSERT_EQUAL_INT8(-52,                   pkt.rssi);
}

/* ── Test: decode STOP_UP 2-way — RX capture ────────────────────────────────
 * BIN: 11111100 00001110 01011101 00110100 10110000 01011111 11111011 00000000 00101000
 * PKT proto=PROTO_COSMO_2WAY cmd=COSMO_BTN_STOP_UP(8)
 *     serial=0xB05FFB20 counter=1238 rssi=-53 dBm extra=0x00
 */
void test_stop_up_2way(void)
{
    cosmo_raw_packet_t raw = { .rssi = -53 };
    parse_hex("FC0E5D34B05FFB0028", raw.data, 9);

    cosmo_packet_t pkt;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_decode(&raw, &pkt));
    TEST_ASSERT_EQUAL(PROTO_COSMO_2WAY,   pkt.proto);
    TEST_ASSERT_EQUAL(COSMO_BTN_STOP_UP,  pkt.cmd);
    TEST_ASSERT_EQUAL_UINT32(0xB05FFB20,  pkt.serial);
    TEST_ASSERT_EQUAL_UINT16(1238,        pkt.counter);
    TEST_ASSERT_EQUAL_UINT8(0x00,         pkt.extra_payload);
    TEST_ASSERT_EQUAL_INT8(-53,           pkt.rssi);
}

/* ── Test: encode REQUEST_FEEDBACK 2-way — matches TX capture ───────────────
 * TX BIN: 11010001 11100000 10001011 10111100 10011100 00000100 00011100 00000000 00010000
 * PKT proto=PROTO_COSMO_2WAY cmd=COSMO_BTN_REQUEST_FEEDBACK(16)
 *     serial=0x9C041C00 counter=109 extra=0x00
 */
void test_request_feedback_encode(void)
{
    cosmo_packet_t orig = {
        .proto         = PROTO_COSMO_2WAY,
        .cmd           = COSMO_BTN_REQUEST_FEEDBACK,
        .counter       = 109,
        .serial        = 0x9C041C00,
        .extra_payload = 0x00,
    };

    cosmo_raw_packet_t raw;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_encode(&orig, &raw));

    uint8_t expected[COSMO_RAW_PACKET_LEN];
    parse_hex("D1E08BBC9C041C0010", expected, COSMO_RAW_PACKET_LEN);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, raw.data, COSMO_RAW_PACKET_LEN);

    /* Also verify it decodes back correctly */
    cosmo_packet_t decoded;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_decode(&raw, &decoded));
    TEST_ASSERT_EQUAL(orig.proto,              decoded.proto);
    TEST_ASSERT_EQUAL(orig.cmd,                decoded.cmd);
    TEST_ASSERT_EQUAL_UINT32(orig.serial,      decoded.serial);
    TEST_ASSERT_EQUAL_UINT16(orig.counter,     decoded.counter);
    TEST_ASSERT_EQUAL_UINT8(orig.extra_payload, decoded.extra_payload);
}

/* ── Test: corrupted packet → ESP_FAIL ──────────────────────────────────────
 * Flip two bits in the last byte of the FEEDBACK_PARTIAL capture.
 */
void test_bad_packet(void)
{
    cosmo_raw_packet_t raw = { .rssi = 0 };
    parse_hex("DC8887DE9C041C270D", raw.data, 9);
    raw.data[8] ^= 0x03;

    cosmo_packet_t pkt;
    TEST_ASSERT_EQUAL(ESP_FAIL, cosmo_decode(&raw, &pkt));
}

/* ── Test: roundtrip DOWN 2-way ─────────────────────────────────────────────
 * RADIO: TX x4  proto=PROTO_COSMO_2WAY cmd=COSMO_BTN_DOWN(4)
 *        serial=0x552E2804 counter=2 rssi=0 dBm extra=0x00
 *
 * The original serial 0x552E2804 had the lower 5 bits set (0x04); those bits
 * are not stored in the packet (27-bit serial, bits 31–5 only).  The corrected
 * value 0x552E2800 is used here so encode → decode round-trips cleanly.
 */
void test_down_2way_roundtrip(void)
{
    cosmo_packet_t orig = {
        .proto         = PROTO_COSMO_2WAY,
        .cmd           = COSMO_BTN_DOWN,
        .counter       = 2,
        .serial        = 0x552E2800,   /* 0x552E2804 with lower 5 bits zeroed */
        .extra_payload = 0x00,
    };

    cosmo_raw_packet_t raw;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_encode(&orig, &raw));

    cosmo_packet_t decoded;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_decode(&raw, &decoded));
    TEST_ASSERT_EQUAL(orig.proto,                decoded.proto);
    TEST_ASSERT_EQUAL(orig.cmd,                  decoded.cmd);
    TEST_ASSERT_EQUAL_UINT32(orig.serial,        decoded.serial);
    TEST_ASSERT_EQUAL_UINT16(orig.counter,       decoded.counter);
    TEST_ASSERT_EQUAL_UINT8(orig.extra_payload,  decoded.extra_payload);
}

/* ── Test: cmd name lookup ──────────────────────────────────────────────── */
void test_cmd_name(void)
{
    TEST_ASSERT_EQUAL_STRING("COSMO_BTN_UP",               cosmo_cmd_name(COSMO_BTN_UP));
    TEST_ASSERT_EQUAL_STRING("COSMO_BTN_DOWN",             cosmo_cmd_name(COSMO_BTN_DOWN));
    TEST_ASSERT_EQUAL_STRING("COSMO_BTN_STOP",             cosmo_cmd_name(COSMO_BTN_STOP));
    TEST_ASSERT_EQUAL_STRING("COSMO_BTN_REQUEST_POSITION", cosmo_cmd_name(COSMO_BTN_REQUEST_POSITION));
    TEST_ASSERT_EQUAL_STRING("COSMO_BTN_REQUEST_FEEDBACK", cosmo_cmd_name(COSMO_BTN_REQUEST_FEEDBACK));
    TEST_ASSERT_EQUAL_STRING("COSMO_BTN_FEEDBACK_PARTIAL", cosmo_cmd_name(COSMO_BTN_FEEDBACK_PARTIAL));
    TEST_ASSERT_EQUAL_STRING("COSMO_BTN_UNKNOWN",          cosmo_cmd_name((cosmo_cmd_t)31));
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_feedback_partial_2way);
    RUN_TEST(test_stop_up_2way);
    RUN_TEST(test_request_feedback_encode);
    RUN_TEST(test_bad_packet);
    RUN_TEST(test_down_2way_roundtrip);
    RUN_TEST(test_cmd_name);
    return UNITY_END();
}
