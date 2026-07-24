#pragma once
#include <stdint.h>
#include <stddef.h>
#include <time.h>

// Decode base32 string into raw bytes. Returns number of bytes written, or 0 on error.
size_t   totp_base32_decode(const char* input, uint8_t* output, size_t out_size);

// Generate a 6-digit TOTP code per RFC 6238.
uint32_t totp_generate(const uint8_t* secret, size_t secret_len, time_t now);

// Convenience: decode secret_b32, then generate code. Returns UINT32_MAX on decode error.
uint32_t totp_from_base32(const char* secret_b32, time_t now);

// Seconds remaining in the current 30s window.
uint8_t  totp_seconds_remaining(time_t now);
