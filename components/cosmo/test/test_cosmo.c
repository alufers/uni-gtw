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

/* ── Test: decode STOP 2-way — factory remote capture ───────────────────────
 * Packet: 16dc45f9 b05ffb 00 21
 * Decrypted block: 0x84ec0489
 *   bit31=1, cmd=STOP(1), serial_lo=0xEC, counter=0x0489
 * serial_cmd (data[8]) = ((0xEC&7)<<3)|(1&7) = (4<<3)|1 = 0x21
 */
void test_stop_2way_factory(void)
{
    cosmo_raw_packet_t raw = { .rssi = -70 };
    parse_hex("16dc45f9b05ffb0021", raw.data, 9);

    cosmo_packet_t pkt;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_decode(&raw, &pkt));
    TEST_ASSERT_EQUAL(PROTO_COSMO_2WAY, pkt.proto);
    TEST_ASSERT_EQUAL(COSMO_BTN_STOP, pkt.cmd);
    TEST_ASSERT_EQUAL_UINT32(0xB05FFBEC, pkt.serial);
    TEST_ASSERT_EQUAL_UINT16(0x0489, pkt.counter);
    TEST_ASSERT_EQUAL_UINT8(0x00, pkt.extra_payload);
    TEST_ASSERT_EQUAL_INT8(-70, pkt.rssi);
}

/* ── Test: decode STOP 2-way — software-generated capture ───────────────────
 * Packet: 915ad257 c601e5 00 01
 * Decrypted block: 0x84a00087
 *   bit31=1, cmd=STOP(1), serial_lo=0xA0, counter=0x0087
 * serial_cmd (data[8]) = ((0xA0&7)<<3)|(1&7) = (0<<3)|1 = 0x01
 */
void test_stop_2way_sw(void)
{
    cosmo_raw_packet_t raw = { .rssi = -65 };
    parse_hex("915ad257c601e50001", raw.data, 9);

    cosmo_packet_t pkt;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_decode(&raw, &pkt));
    TEST_ASSERT_EQUAL(PROTO_COSMO_2WAY, pkt.proto);
    TEST_ASSERT_EQUAL(COSMO_BTN_STOP, pkt.cmd);
    TEST_ASSERT_EQUAL_UINT32(0xC601E5A0, pkt.serial);
    TEST_ASSERT_EQUAL_UINT16(0x0087, pkt.counter);
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
    TEST_ASSERT_EQUAL_UINT8(0x00, raw.data[8]);  /* 1-way: data[8]=0x00 */

    cosmo_packet_t decoded;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_decode(&raw, &decoded));
    TEST_ASSERT_EQUAL(orig.proto,            decoded.proto);
    TEST_ASSERT_EQUAL(orig.cmd,              decoded.cmd);
    TEST_ASSERT_EQUAL_UINT16(orig.counter,   decoded.counter);
    TEST_ASSERT_EQUAL_UINT32(orig.serial,    decoded.serial);
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
    TEST_ASSERT_EQUAL_UINT8(0xAB, raw.data[7]);  /* 2-way: data[7]=extra_payload */
    TEST_ASSERT_NOT_EQUAL(0, raw.data[8]);        /* data[8]=serial_cmd */

    cosmo_packet_t decoded;
    TEST_ASSERT_EQUAL(ESP_OK, cosmo_decode(&raw, &decoded));
    TEST_ASSERT_EQUAL(orig.proto,               decoded.proto);
    TEST_ASSERT_EQUAL(orig.cmd,                 decoded.cmd);
    TEST_ASSERT_EQUAL_UINT16(orig.counter,      decoded.counter);
    TEST_ASSERT_EQUAL_UINT32(orig.serial,       decoded.serial);
    TEST_ASSERT_EQUAL_UINT8(orig.extra_payload, decoded.extra_payload);
}

/* ── Test: corrupted serial_cmd_byte → ESP_FAIL ─────────────────────────── */
void test_bad_packet(void)
{
    cosmo_raw_packet_t raw = { .rssi = 0 };
    parse_hex("16dc45f9b05ffb0021", raw.data, 9);
    raw.data[8] ^= 0x03;  /* flip two bits in serial_cmd_byte */

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
    RUN_TEST(test_stop_2way_factory);
    RUN_TEST(test_stop_2way_sw);
    RUN_TEST(test_roundtrip_1way);
    RUN_TEST(test_roundtrip_2way);
    RUN_TEST(test_bad_packet);
    RUN_TEST(test_cmd_name);
    return UNITY_END();
}
