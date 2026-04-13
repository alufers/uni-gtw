#include "cosmo/cosmo.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef HOST_TEST
#include "esp_log.h"
static const char *COSMO_TAG = "cosmo";
#define COSMO_LOG(fmt, ...) ESP_LOGI(COSMO_TAG, fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define COSMO_LOG(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#endif

/* ── KeeLoq ─────────────────────────────────────────────────────────────── */

#define KEELOQ_KEY 0x442000760712425FULL
#define KEELOQ_NLF 0x3A5C742EUL

#define _kl_bit(x, n) (((x) >> (n)) & 1U)
#define _kl_g5(x, a, b, c, d, e)                                               \
  (_kl_bit(x, a) + _kl_bit(x, b) * 2 + _kl_bit(x, c) * 4 + _kl_bit(x, d) * 8 + \
   _kl_bit(x, e) * 16)

static uint32_t keeloq_encrypt(uint32_t data, uint64_t key) {
  uint32_t x = data;
  for (uint32_t r = 0; r < 528; r++)
    x = (x >> 1) ^
        ((_kl_bit(x, 0) ^ _kl_bit(x, 16) ^ (uint32_t)_kl_bit(key, r & 63) ^
          _kl_bit(KEELOQ_NLF, _kl_g5(x, 1, 9, 20, 26, 31)))
         << 31);
  return x;
}

static uint32_t keeloq_decrypt(uint32_t data, uint64_t key) {
  uint32_t x = data;
  for (uint32_t r = 0; r < 528; r++)
    x = (x << 1) ^ _kl_bit(x, 31) ^ _kl_bit(x, 15) ^
        (uint32_t)_kl_bit(key, (15 - r) & 63) ^
        _kl_bit(KEELOQ_NLF, _kl_g5(x, 0, 8, 19, 25, 30));
  return x;
}

/* ── Decode ──────────────────────────────────────────────────────────────── */

esp_err_t cosmo_decode(const cosmo_raw_packet_t *raw, cosmo_packet_t *out) {
  memset(out, 0, sizeof(*out));
  out->rssi = raw->rssi;

  /* Proto: 1-way = 8-byte OTA packet
   *        2-way = 9-byte OTA packet Extra byte added at index 7 */
  int is_2way = (raw->data[8] != 0);
  uint8_t last_byte = is_2way ? raw->data[8] : raw->data[7];
  out->proto = is_2way ? PROTO_COSMO_2WAY : PROTO_COSMO_1WAY;

  uint8_t cmd_clear = last_byte & 0b00011111;
  out->cmd = cmd_clear;
  /* Decrypt the first 4 bytes (little-endian 32-bit word, data[0]=LSB) */
  uint32_t enc = (uint32_t)raw->data[0] | ((uint32_t)raw->data[1] << 8) |
                 ((uint32_t)raw->data[2] << 16) |
                 ((uint32_t)raw->data[3] << 24);
  uint32_t dec = keeloq_decrypt(enc, KEELOQ_KEY);

  /* Extract fields from the decrypted word */
  uint8_t byte0 = (dec >> 24) & 0xFF;
  uint8_t byte1 = (dec >> 16) & 0xFF;
  out->counter = (uint16_t)(dec & 0xFFFF);

  /* Assemble 32-bit serial */
  out->serial = ((uint32_t)raw->data[4] << 24) |
                ((uint32_t)raw->data[5] << 16) | ((uint32_t)raw->data[6] << 8) |
                (last_byte & 0b11100000);

  /* Extra payload (2-way only) */
  if (is_2way)
    out->extra_payload = raw->data[7];

  /* Verify serial_cmd_byte (data[8] for 2-way, data[7] for 1-way):
   * bits[5:3] = serial_lo[2:0], bits[2:0] = cmd low 3 bits */

  uint8_t byte1_expected = last_byte >> 6 | raw->data[6] << 2;

  if (byte1_expected != byte1) {
    COSMO_LOG("encrypted[1] does not match (enc=%u expected=%u)", byte1,
              byte1_expected);
    return ESP_FAIL;
  }

  uint8_t byte0_expected = last_byte << 2;
  if (is_2way) {
    uint8_t extra_payload = raw->data[7];
    do {
      if ((extra_payload & 1) != 0) {
        byte0_expected++;
      }
      extra_payload >>= 1;
    } while (extra_payload != 0);
  }


  if (byte0 != byte0_expected) {
    COSMO_LOG("encrypted[0] mismatch (enc=%u expected=%u)", byte0,
              byte0_expected);
    return ESP_FAIL;
  }

  return ESP_OK;
}

/* ── Encode ──────────────────────────────────────────────────────────────── */

esp_err_t cosmo_encode(const cosmo_packet_t *pkt, cosmo_raw_packet_t *out) {
  memset(out, 0, sizeof(*out));

  /* The serial field is 27 bits (bits 31–5 of the uint32); the lower 5 bits
   * are not stored in the packet and will read back as zero on decode. */
  if (pkt->serial & 0x1FU) {
    COSMO_LOG("WARNING: serial 0x%08X has lower 5 bits set (0x%02X); "
              "they are not transmitted and will be lost on decode",
              (unsigned)pkt->serial, (unsigned)(pkt->serial & 0x1FU));
  }

  /* Serial upper 3 bytes */
  out->data[4] = (pkt->serial >> 24) & 0xFF;
  out->data[5] = (pkt->serial >> 16) & 0xFF;
  out->data[6] = (pkt->serial >> 8) & 0xFF;

  uint8_t last_byte = (pkt->serial & 0b11100000) | pkt->cmd;

  uint8_t byte0 = last_byte << 2;



  if (pkt->proto == PROTO_COSMO_2WAY) {
    uint8_t extra_payload_copy = pkt->extra_payload;
    do {
      if ((extra_payload_copy & 1) != 0) {
        byte0++;
      }
      extra_payload_copy >>= 1;
    } while (extra_payload_copy != 0);
  }


  uint8_t byte1 = last_byte >> 6 | out->data[6] << 2;
  /* Build the 32-bit word to encrypt */
  uint32_t plain = ((uint32_t)(byte0) << 24) /* 5-bit cmd */
                   |
                   ((uint32_t)(byte1) << 16) /*  */
                   | pkt->counter; /* 16-bit counter */

  uint32_t enc = keeloq_encrypt(plain, KEELOQ_KEY);

  /* Little-endian byte order (data[0]=LSB) */
  out->data[0] = enc & 0xFF;
  out->data[1] = (enc >> 8) & 0xFF;
  out->data[2] = (enc >> 16) & 0xFF;
  out->data[3] = (enc >> 24) & 0xFF;

  if (pkt->proto == PROTO_COSMO_2WAY) {
    out->data[7] = pkt->extra_payload;
    out->data[8] = last_byte;
  } else {
    out->data[7] = last_byte;
    out->data[8] = 0x00;
  }

  return ESP_OK;
}

/* ── Formatting ──────────────────────────────────────────────────────────── */

const char *cosmo_cmd_name(cosmo_cmd_t cmd) {
  switch (cmd) {
  case COSMO_BTN_REQUEST_POSITION:
    return "COSMO_BTN_REQUEST_POSITION";
  case COSMO_BTN_STOP:
    return "COSMO_BTN_STOP";
  case COSMO_BTN_UP:
    return "COSMO_BTN_UP";
  case COSMO_BTN_UP_DOWN:
    return "COSMO_BTN_UP_DOWN";
  case COSMO_BTN_DOWN:
    return "COSMO_BTN_DOWN";
  case COSMO_BTN_STOP_DOWN:
    return "COSMO_BTN_STOP_DOWN";
  case COSMO_BTN_STOP_HOLD:
    return "COSMO_BTN_STOP_HOLD";
  case COSMO_BTN_PROG:
    return "COSMO_BTN_PROG";
  case COSMO_BTN_STOP_UP:
    return "COSMO_BTN_STOP_UP";
  case COSMO_BTN_FEEDBACK_OBSTRUCTION:
    return "COSMO_BTN_FEEDBACK_OBSTRUCTION";
  case COSMO_BTN_FEEDBACK_BOTTOM:
    return "COSMO_BTN_FEEDBACK_BOTTOM";
  case COSMO_BTN_FEEDBACK_TOP:
    return "COSMO_BTN_FEEDBACK_TOP";
  case COSMO_BTN_FEEDBACK_COMFORT:
    return "COSMO_BTN_FEEDBACK_COMFORT";
  case COSMO_BTN_FEEDBACK_PARTIAL:
    return "COSMO_BTN_FEEDBACK_PARTIAL";
  case COSMO_BTN_FEEDBACK_IN_MOTION:
    return "COSMO_BTN_FEEDBACK_IN_MOTION";
  case COSMO_BTN_REQUEST_FEEDBACK:
    return "COSMO_BTN_REQUEST_FEEDBACK";
  case COSMO_BTN_TILT_INCREASE:
    return "COSMO_BTN_TILT_INCREASE";
  case COSMO_BTN_TILT_DECREASE:
    return "COSMO_BTN_TILT_DECREASE";
  case COSMO_BTN_SET_POSITION:
    return "COSMO_BTN_SET_POSITION";
  case COSMO_BTN_DETAILED_FEEDBACK:
    return "COSMO_BTN_DETAILED_FEEDBACK";
  case COSMO_BTN_SET_TILT:
    return "COSMO_BTN_SET_TILT";
  default:
    return "COSMO_BTN_UNKNOWN";
  }
}

size_t cosmo_packet_to_str(const cosmo_packet_t *pkt, char *buf, size_t len) {
  const char *proto_name = (pkt->proto == PROTO_COSMO_2WAY)
                               ? "PROTO_COSMO_2WAY"
                               : "PROTO_COSMO_1WAY";
  int n = snprintf(
      buf, len, "proto=%s cmd=%s(%d) serial=0x%08X counter=%u rssi=%d dBm",
      proto_name, cosmo_cmd_name(pkt->cmd), (int)pkt->cmd,
      (unsigned)pkt->serial, (unsigned)pkt->counter, (int)pkt->rssi);

  if (pkt->proto == PROTO_COSMO_2WAY && n > 0 && (size_t)n < len) {
    n +=
        snprintf(buf + n, len - (size_t)n, " extra=0x%02X", pkt->extra_payload);
  }
  return (n > 0) ? (size_t)n : 0;
}

void cosmo_packet_log(const cosmo_packet_t *pkt) {
  char buf[256];
  cosmo_packet_to_str(pkt, buf, sizeof(buf));
  COSMO_LOG("PKT %s", buf);
}
