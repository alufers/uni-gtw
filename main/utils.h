#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "cosmo/cosmo.h"

/* ── Time ────────────────────────────────────────────────────────────────── */

/**
 * Returns true if the system clock has been synchronised via NTP.
 * Uses a threshold of 2020-01-01 00:00:00 UTC.
 */
bool utils_time_is_valid(void);

/* ── Base64 ──────────────────────────────────────────────────────────────── */

/**
 * Encode @p len bytes from @p src into base64 and write NUL-terminated result
 * to @p dst.  @p dst must have room for at least ((len + 2) / 3) * 4 + 1 bytes.
 */
void utils_base64_encode(const uint8_t *src, size_t len, char *dst);

/**
 * Decode a base64 string @p src of @p src_len characters into @p dst.
 * *@p out_len is set to the number of decoded bytes.
 * Returns true on success, false if @p src contains invalid characters.
 * @p dst must have room for at least (src_len / 4) * 3 bytes.
 */
bool utils_base64_decode(const char *src, size_t src_len,
                         uint8_t *dst, size_t *out_len);

/* ── Password crypto (PBKDF2-HMAC-SHA256) ───────────────────────────────── */

/**
 * Hash @p password with a random 16-byte salt using PBKDF2-HMAC-SHA256 and
 * write the stored representation to @p out_buf (at least 80 bytes):
 *   base64(16-byte salt) + "$" + base64(32-byte hash)
 *
 * Returns true on success.
 */
bool utils_crypto_hash_password(const char *password,
                                char *out_buf, size_t out_buf_len);

/**
 * Verify @p password against a stored hash string produced by
 * utils_crypto_hash_password().  Returns true if the password matches.
 */
bool utils_crypto_verify_password(const char *password, const char *stored);

/* ── Cosmo command helpers ───────────────────────────────────────────────── */

/**
 * Map a channel_cmd_name_t integer (from the WS protocol schema) to the
 * corresponding cosmo_cmd_t.  Returns false for unknown values.
 */
bool utils_cmd_name_to_cosmo(int cmd_name, cosmo_cmd_t *out);

/**
 * Parse a command string (e.g. "UP", "STOP_DOWN") or decimal/hex integer
 * into the corresponding cosmo_cmd_t.  Case-insensitive.
 * Returns false if the string is not recognised.
 */
bool utils_str_to_cosmo_cmd(const char *s, cosmo_cmd_t *out);
