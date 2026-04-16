#include "utils.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_random.h"
#include "psa/crypto.h"

static const char *TAG = "utils";

/* ── Base64 ──────────────────────────────────────────────────────────────── */

static const char B64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void utils_base64_encode(const uint8_t *src, size_t len, char *dst)
{
    size_t out = 0;
    size_t i   = 0;
    while (i < len) {
        uint32_t a = i < len ? src[i++] : 0;
        uint32_t b = i < len ? src[i++] : 0;
        uint32_t c = i < len ? src[i++] : 0;
        uint32_t t = (a << 16) | (b << 8) | c;
        dst[out++] = B64_TABLE[(t >> 18) & 0x3F];
        dst[out++] = B64_TABLE[(t >> 12) & 0x3F];
        dst[out++] = B64_TABLE[(t >>  6) & 0x3F];
        dst[out++] = B64_TABLE[(t >>  0) & 0x3F];
    }
    size_t mod = len % 3;
    if (mod == 1) { dst[out - 2] = '='; dst[out - 1] = '='; }
    else if (mod == 2) { dst[out - 1] = '='; }
    dst[out] = '\0';
}

/* Returns the 6-bit value for a base64 character, or -1 if invalid. */
static int b64_char_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return 0; /* padding */
    return -1;
}

bool utils_base64_decode(const char *src, size_t src_len,
                         uint8_t *dst, size_t *out_len)
{
    if (src_len % 4 != 0) return false;

    size_t out = 0;
    for (size_t i = 0; i < src_len; i += 4) {
        int v0 = b64_char_value(src[i]);
        int v1 = b64_char_value(src[i + 1]);
        int v2 = b64_char_value(src[i + 2]);
        int v3 = b64_char_value(src[i + 3]);
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) return false;

        dst[out++] = (uint8_t)(((unsigned)v0 << 2) | ((unsigned)v1 >> 4));
        if (src[i + 2] != '=')
            dst[out++] = (uint8_t)(((unsigned)v1 << 4) | ((unsigned)v2 >> 2));
        if (src[i + 3] != '=')
            dst[out++] = (uint8_t)(((unsigned)v2 << 6) | (unsigned)v3);
    }
    *out_len = out;
    return true;
}

/* ── PBKDF2-HMAC-SHA256 ──────────────────────────────────────────────────── */

#define SALT_LEN    16
#define HASH_LEN    32
#define KDF_ITERS   1000

/* Encoded sizes: base64 of SALT_LEN bytes → 24 chars, HASH_LEN bytes → 44 chars */
#define SALT_B64_LEN  24   /* ((SALT_LEN + 2) / 3) * 4 */
#define HASH_B64_LEN  44   /* ((HASH_LEN + 2) / 3) * 4 */
/* Stored format: "<24 chars>$<44 chars>\0" → 70 bytes */
#define STORED_LEN    (SALT_B64_LEN + 1 + HASH_B64_LEN + 1)

static bool pbkdf2_sha256(const uint8_t *salt, size_t salt_len,
                          const char *password, size_t pass_len,
                          uint8_t *out_hash, size_t hash_len)
{
    psa_status_t st;

    st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)st);
        return false;
    }

    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;

    st = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (st != PSA_SUCCESS) goto fail;

    st = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST,
                                          KDF_ITERS);
    if (st != PSA_SUCCESS) goto fail;

    st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT,
                                        salt, salt_len);
    if (st != PSA_SUCCESS) goto fail;

    st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                        (const uint8_t *)password, pass_len);
    if (st != PSA_SUCCESS) goto fail;

    st = psa_key_derivation_output_bytes(&op, out_hash, hash_len);
    if (st != PSA_SUCCESS) goto fail;

    psa_key_derivation_abort(&op);
    return true;

fail:
    ESP_LOGE(TAG, "PBKDF2 derivation failed: %d", (int)st);
    psa_key_derivation_abort(&op);
    return false;
}

bool utils_crypto_hash_password(const char *password,
                                char *out_buf, size_t out_buf_len)
{
    if (out_buf_len < STORED_LEN) return false;

    uint8_t salt[SALT_LEN];
    esp_fill_random(salt, sizeof(salt));

    uint8_t hash[HASH_LEN];
    if (!pbkdf2_sha256(salt, SALT_LEN, password, strlen(password), hash, HASH_LEN))
        return false;

    /* salt_b64 + "$" + hash_b64 */
    char salt_b64[SALT_B64_LEN + 1];
    char hash_b64[HASH_B64_LEN + 1];
    utils_base64_encode(salt, SALT_LEN, salt_b64);
    utils_base64_encode(hash, HASH_LEN, hash_b64);

    snprintf(out_buf, out_buf_len, "%s$%s", salt_b64, hash_b64);
    return true;
}

bool utils_crypto_verify_password(const char *password, const char *stored)
{
    if (!password || !stored || *stored == '\0') return false;

    /* Find the '$' separator */
    const char *sep = strchr(stored, '$');
    if (!sep) return false;

    size_t salt_b64_len = (size_t)(sep - stored);
    const char *hash_b64 = sep + 1;
    size_t hash_b64_len  = strlen(hash_b64);

    if (salt_b64_len != SALT_B64_LEN || hash_b64_len != HASH_B64_LEN)
        return false;

    /* Decode salt */
    uint8_t salt[SALT_LEN];
    size_t decoded_len = 0;
    if (!utils_base64_decode(stored, salt_b64_len, salt, &decoded_len))
        return false;
    if (decoded_len != SALT_LEN) return false;

    /* Decode stored hash */
    uint8_t stored_hash[HASH_LEN];
    if (!utils_base64_decode(hash_b64, hash_b64_len, stored_hash, &decoded_len))
        return false;
    if (decoded_len != HASH_LEN) return false;

    /* Recompute */
    uint8_t computed_hash[HASH_LEN];
    if (!pbkdf2_sha256(salt, SALT_LEN, password, strlen(password),
                       computed_hash, HASH_LEN))
        return false;

    /* Constant-time compare */
    uint8_t diff = 0;
    for (int i = 0; i < HASH_LEN; i++)
        diff |= stored_hash[i] ^ computed_hash[i];
    return diff == 0;
}
