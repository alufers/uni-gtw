#include "cosmo/cosmo.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#ifndef HOST_TEST
#include "esp_log.h"
static const char *COSMO_TAG = "cosmo";
#define COSMO_LOG(fmt, ...) ESP_LOGI(COSMO_TAG, fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define COSMO_LOG(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#endif

/* ── KeeLoq ─────────────────────────────────────────────────────────────── */

#define KEELOQ_KEY  0x442000760712425FULL
#define KEELOQ_NLF  0x3A5C742EUL

#define _kl_bit(x, n)  (((x) >> (n)) & 1U)
#define _kl_g5(x, a, b, c, d, e) \
    (_kl_bit(x,a) + _kl_bit(x,b)*2 + _kl_bit(x,c)*4 + _kl_bit(x,d)*8 + _kl_bit(x,e)*16)

static uint32_t keeloq_encrypt(uint32_t data, uint64_t key)
{
    uint32_t x = data;
    for (uint32_t r = 0; r < 528; r++)
        x = (x >> 1) ^ ((_kl_bit(x,0) ^ _kl_bit(x,16) ^
                          (uint32_t)_kl_bit(key, r & 63) ^
                          _kl_bit(KEELOQ_NLF, _kl_g5(x,1,9,20,26,31))) << 31);
    return x;
}

static uint32_t keeloq_decrypt(uint32_t data, uint64_t key)
{
    uint32_t x = data;
    for (uint32_t r = 0; r < 528; r++)
        x = (x << 1) ^ _kl_bit(x,31) ^ _kl_bit(x,15) ^
            (uint32_t)_kl_bit(key, (15 - r) & 63) ^
            _kl_bit(KEELOQ_NLF, _kl_g5(x,0,8,19,25,30));
    return x;
}

/* ── Packet layout ───────────────────────────────────────────────────────── *
 *
 * Byte indices in cosmo_raw_packet_t.data[0..8]:
 *
 *  [0..3]  KeeLoq encrypted block (little-endian 32-bit word, data[0]=LSB).
 *          After decryption (MSB → LSB):
 *            bit 31     : always 1
 *            bits[30:26]: command (5 bits)
 *            bits[25:24]: parity for extra_payload (2-way) / zero (1-way)
 *            bits[23:16]: low byte of serial
 *            bits[15:0] : 16-bit rolling counter
 *
 *  [4..6]  Serial, upper 3 bytes  (serial[31:8])
 *
 *  [7]     2-way: extra_payload byte
 *          1-way: serial_cmd_byte  (see below)
 *
 *  [8]     2-way: serial_cmd_byte  (non-zero → 2-way)
 *          1-way: 0x00
 *
 *  serial_cmd_byte format:  bits[7:5] = serial[2:0]  (= decrypted low byte & 7)
 *                           bits[4:0] = command (5 bits, redundant with encrypted)
 *
 *  Full serial (32 bits):
 *    serial = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | dec_serial_lo
 *    where dec_serial_lo = (dec >> 16) & 0xFF
 *
 * ─────────────────────────────────────────────────────────────────────────── */

/* ── Decode ──────────────────────────────────────────────────────────────── */

esp_err_t cosmo_decode(const cosmo_raw_packet_t *raw, cosmo_packet_t *out)
{
    memset(out, 0, sizeof(*out));
    out->rssi = raw->rssi;

    /* Proto: last byte non-zero ↔ 2-way */
    int is_2way = (raw->data[8] != 0);
    out->proto  = is_2way ? PROTO_COSMO_2WAY : PROTO_COSMO_1WAY;

    /* Decrypt the first 4 bytes (little-endian 32-bit word, data[0]=LSB) */
    uint32_t enc = (uint32_t)raw->data[0]          |
                   ((uint32_t)raw->data[1] << 8)   |
                   ((uint32_t)raw->data[2] << 16)  |
                   ((uint32_t)raw->data[3] << 24);
    uint32_t dec = keeloq_decrypt(enc, KEELOQ_KEY);

    /* Extract fields from the decrypted word */
    uint8_t dec_serial_lo = (dec >> 16) & 0xFF;
    out->counter = (uint16_t)(dec & 0xFFFF);
    uint8_t cmd_enc = (dec >> 26) & 0x1F;
    out->cmd     = (cosmo_cmd_t)cmd_enc;

    /* Assemble 32-bit serial */
    out->serial = ((uint32_t)raw->data[4] << 24) |
                  ((uint32_t)raw->data[5] << 16) |
                  ((uint32_t)raw->data[6] << 8)  |
                   dec_serial_lo;

    /* Extra payload (2-way only) */
    if (is_2way)
        out->extra_payload = raw->data[7];

    /* Verify unencrypted command byte */
    uint8_t serial_cmd_byte = is_2way ? raw->data[8] : raw->data[7];
    uint8_t cmd_clear       = serial_cmd_byte & 0x1F;

    if (cmd_enc != cmd_clear)
        return ESP_FAIL;   /* command mismatch */

    return ESP_OK;
}

/* ── Encode ──────────────────────────────────────────────────────────────── */

esp_err_t cosmo_encode(const cosmo_packet_t *pkt, cosmo_raw_packet_t *out)
{
    memset(out, 0, sizeof(*out));

    uint8_t dec_serial_lo = pkt->serial & 0xFF;

    /* Build the 32-bit word to encrypt */
    uint32_t plain = (1UL << 31)                            /* always-1 bit */
                   | ((uint32_t)(pkt->cmd & 0x1F) << 26)   /* 5-bit cmd */
                   /* parity bits[25:24] = 0 for now */
                   | ((uint32_t)dec_serial_lo       << 16)  /* serial low byte */
                   | pkt->counter;                          /* 16-bit counter */

    uint32_t enc = keeloq_encrypt(plain, KEELOQ_KEY);

    /* Little-endian byte order (data[0]=LSB) */
    out->data[0] =  enc        & 0xFF;
    out->data[1] = (enc >> 8)  & 0xFF;
    out->data[2] = (enc >> 16) & 0xFF;
    out->data[3] = (enc >> 24) & 0xFF;

    /* Serial upper 3 bytes */
    out->data[4] = (pkt->serial >> 24) & 0xFF;
    out->data[5] = (pkt->serial >> 16) & 0xFF;
    out->data[6] = (pkt->serial >> 8)  & 0xFF;

    /* serial_cmd_byte: serial[2:0] in top 3 bits, cmd in bottom 5 */
    uint8_t serial_cmd = ((dec_serial_lo & 0x07) << 5) | (pkt->cmd & 0x1F);

    if (pkt->proto == PROTO_COSMO_2WAY) {
        out->data[7] = pkt->extra_payload;
        out->data[8] = serial_cmd;
    } else {
        out->data[7] = serial_cmd;
        out->data[8] = 0x00;
    }

    return ESP_OK;
}

/* ── Formatting ──────────────────────────────────────────────────────────── */

const char *cosmo_cmd_name(cosmo_cmd_t cmd)
{
    switch (cmd) {
    case COSMO_BTN_NONE:              return "COSMO_BTN_NONE";
    case COSMO_BTN_STOP:              return "COSMO_BTN_STOP";
    case COSMO_BTN_UP:                return "COSMO_BTN_UP";
    case COSMO_BTN_UP_DOWN:           return "COSMO_BTN_UP_DOWN";
    case COSMO_BTN_DOWN:              return "COSMO_BTN_DOWN";
    case COSMO_BTN_STOP_DOWN:         return "COSMO_BTN_STOP_DOWN";
    case COSMO_BTN_STOP_HOLD:         return "COSMO_BTN_STOP_HOLD";
    case COSMO_BTN_PROG:              return "COSMO_BTN_PROG";
    case COSMO_BTN_STOP_UP:           return "COSMO_BTN_STOP_UP";
    case COSMO_BTN_OBSTRUCTION:       return "COSMO_BTN_OBSTRUCTION";
    case COSMO_BTN_FEEDBACK_BOTTOM:   return "COSMO_BTN_FEEDBACK_BOTTOM";
    case COSMO_BTN_FEEDBACK_TOP:      return "COSMO_BTN_FEEDBACK_TOP";
    case COSMO_BTN_FEEDBACK_COMFORT:  return "COSMO_BTN_FEEDBACK_COMFORT";
    case COSMO_BTN_FEEDBACK_PARTIAL:  return "COSMO_BTN_FEEDBACK_PARTIAL";
    case COSMO_BTN_REQUEST_FEEDBACK:  return "COSMO_BTN_REQUEST_FEEDBACK";
    case COSMO_BTN_TILT_INCREASE:     return "COSMO_BTN_TILT_INCREASE";
    case COSMO_BTN_TILT_DECREASE:     return "COSMO_BTN_TILT_DECREASE";
    case COSMO_BTN_SET_POSITION:      return "COSMO_BTN_SET_POSITION";
    case COSMO_BTN_DETAILED_FEEDBACK: return "COSMO_BTN_DETAILED_FEEDBACK";
    case COSMO_BTN_SET_TILT:          return "COSMO_BTN_SET_TILT";
    default:                          return "COSMO_BTN_UNKNOWN";
    }
}

size_t cosmo_packet_to_str(const cosmo_packet_t *pkt, char *buf, size_t len)
{
    const char *proto_name = (pkt->proto == PROTO_COSMO_2WAY)
                             ? "PROTO_COSMO_2WAY" : "PROTO_COSMO_1WAY";
    int n = snprintf(buf, len,
        "proto=%s cmd=%s(%d) serial=0x%08X counter=%u rssi=%d dBm",
        proto_name,
        cosmo_cmd_name(pkt->cmd), (int)pkt->cmd,
        (unsigned)pkt->serial,
        (unsigned)pkt->counter,
        (int)pkt->rssi);

    if (pkt->proto == PROTO_COSMO_2WAY && n > 0 && (size_t)n < len) {
        n += snprintf(buf + n, len - (size_t)n,
                      " extra=0x%02X", pkt->extra_payload);
    }
    return (n > 0) ? (size_t)n : 0;
}

void cosmo_packet_log(const cosmo_packet_t *pkt)
{
    char buf[256];
    cosmo_packet_to_str(pkt, buf, sizeof(buf));
    COSMO_LOG("PKT %s", buf);
}
