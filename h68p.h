// h68p.h — signed detection-plugin container (H68P).
//
// Layout: header (signed fields) | payload | secp256k1 signature (64 bytes).
// device_serial must match pico_get_unique_board_id() (8 bytes OTP).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define H68P_MAGIC          0x50383648u /* 'H68P' LE */
#define H68P_VERSION        1u
#define H68P_API_ABI        1u
#define H68P_SERIAL_LEN     8u
#define H68P_SIG_LEN        64u
#define H68P_HASH_LEN       32u
#define H68P_PUBKEY_LEN     64u /* uncompressed X||Y, no 0x04 prefix */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t api_abi;
    uint32_t flags;
    uint32_t code_size;     // payload bytes following header
    uint32_t entry_off;     // byte offset into payload of Thumb entry
    uint32_t bss_size;      // Secure BSS the plugin expects (informational)
    uint32_t rollback;      // anti-downgrade counter
    uint8_t  device_serial[H68P_SERIAL_LEN];
    uint8_t  payload_sha256[H68P_HASH_LEN];
    uint32_t hdr_crc32;     // CRC of header bytes before this field
} h68p_header_t;

_Static_assert(sizeof(h68p_header_t) == 76u, "h68p_header_t size");

// Maximum payload that fits one 512 KiB flash slot after header+sig.
#define H68P_MAX_PAYLOAD \
    (512u * 1024u - (uint32_t)sizeof(h68p_header_t) - H68P_SIG_LEN)

typedef enum {
    H68P_OK = 0,
    H68P_ERR_MAGIC,
    H68P_ERR_VERSION,
    H68P_ERR_ABI,
    H68P_ERR_SIZE,
    H68P_ERR_CRC,
    H68P_ERR_HASH,
    H68P_ERR_SERIAL,
    H68P_ERR_SIG,
    H68P_ERR_ENTRY,
} h68p_status_t;

typedef struct {
    const h68p_header_t *hdr;
    const uint8_t *payload;
    const uint8_t *sig;     // 64 bytes r||s
    uint32_t total_bytes;
} h68p_view_t;

uint32_t h68p_crc32(const void *data, size_t len);
void h68p_sha256(const void *data, size_t len, uint8_t out[H68P_HASH_LEN]);

// Parse a contiguous blob already in memory (flash XIP or SRAM).
h68p_status_t h68p_parse(const uint8_t *blob, uint32_t blob_len, h68p_view_t *out);

// Verify header fields + payload hash. Does not check serial or signature.
h68p_status_t h68p_check_integrity(const h68p_view_t *v);

// Compare header device_serial to OTP unique id (8 bytes).
h68p_status_t h68p_check_serial(const h68p_view_t *v, const uint8_t serial[H68P_SERIAL_LEN]);

// Verify secp256k1 signature over header (through payload_sha256) || payload.
// pub64 is uncompressed X||Y (64 bytes).
h68p_status_t h68p_check_signature(const h68p_view_t *v,
                                   const uint8_t pub64[H68P_PUBKEY_LEN]);

// Full gate used by the Secure loader.
h68p_status_t h68p_verify(const uint8_t *blob, uint32_t blob_len,
                          const uint8_t serial[H68P_SERIAL_LEN],
                          const uint8_t pub64[H68P_PUBKEY_LEN],
                          h68p_view_t *out);

const char *h68p_status_str(h68p_status_t st);
