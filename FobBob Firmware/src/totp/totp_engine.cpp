#include "totp_engine.h"
#include "mbedtls/md.h"
#include <string.h>
#include <ctype.h>

static const char B32_ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

size_t totp_base32_decode(const char* input, uint8_t* output, size_t out_size) {
    size_t out_len = 0;
    uint32_t buf = 0;
    int bits = 0;

    for (size_t i = 0; input[i] != '\0'; i++) {
        char c = toupper((unsigned char)input[i]);
        if (c == '=' || c == ' ') continue;

        const char* p = strchr(B32_ALPHA, c);
        if (!p) continue;

        buf = (buf << 5) | (uint32_t)(p - B32_ALPHA);
        bits += 5;

        if (bits >= 8) {
            bits -= 8;
            if (out_len < out_size) {
                output[out_len++] = (uint8_t)((buf >> bits) & 0xFF);
            }
        }
    }
    return out_len;
}

uint32_t totp_generate(const uint8_t* secret, size_t secret_len, time_t now) {
    uint64_t T = (uint64_t)now / 30;

    uint8_t T_bytes[8];
    for (int i = 7; i >= 0; i--) {
        T_bytes[i] = (uint8_t)(T & 0xFF);
        T >>= 8;
    }

    uint8_t digest[20];
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);

    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, info, 1);
    mbedtls_md_hmac_starts(&ctx, secret, secret_len);
    mbedtls_md_hmac_update(&ctx, T_bytes, sizeof(T_bytes));
    mbedtls_md_hmac_finish(&ctx, digest);
    mbedtls_md_free(&ctx);

    int offset = digest[19] & 0x0F;
    uint32_t code = ((uint32_t)(digest[offset]     & 0x7F) << 24)
                  | ((uint32_t)(digest[offset + 1] & 0xFF) << 16)
                  | ((uint32_t)(digest[offset + 2] & 0xFF) << 8)
                  |  (uint32_t)(digest[offset + 3] & 0xFF);

    return code % 1000000;
}

uint32_t totp_from_base32(const char* secret_b32, time_t now) {
    uint8_t secret[64];
    size_t len = totp_base32_decode(secret_b32, secret, sizeof(secret));
    if (len == 0) return UINT32_MAX;
    return totp_generate(secret, len, now);
}

uint8_t totp_seconds_remaining(time_t now) {
    return (uint8_t)(30 - ((uint64_t)now % 30));
}
