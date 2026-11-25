// h68p.c — parse / integrity / serial / secp256k1 signature checks for H68P.
#include "h68p.h"

#include <stddef.h>
#include <string.h>

#include "h68p_uecc.h"

// Portable SHA-256 (same digest as Python hashlib / host signer).
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    uint32_t datalen;
} h68p_sha256_ctx;

static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32u - n)); }

static void sha256_transform(h68p_sha256_ctx *ctx, const uint8_t data[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t m[64];
    for (uint32_t i = 0, j = 0; i < 16u; ++i, j += 4u)
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | (uint32_t)data[j + 3];
    for (uint32_t i = 16; i < 64u; ++i) {
        uint32_t s0 = rotr(m[i - 15], 7) ^ rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
        uint32_t s1 = rotr(m[i - 2], 17) ^ rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (uint32_t i = 0; i < 64u; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + k[i] + m[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(h68p_sha256_ctx *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
}

static void sha256_update(h68p_sha256_ctx *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64u) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512u;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(h68p_sha256_ctx *ctx, uint8_t hash[32]) {
    uint32_t i = ctx->datalen;
    ctx->data[i++] = 0x80u;
    if (i > 56u) {
        while (i < 64u) ctx->data[i++] = 0;
        sha256_transform(ctx, ctx->data);
        i = 0;
    }
    while (i < 56u) ctx->data[i++] = 0;
    ctx->bitlen += (uint64_t)ctx->datalen * 8u;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);
    for (i = 0; i < 4u; ++i) {
        hash[i]      = (uint8_t)((ctx->state[0] >> (24u - i * 8u)) & 0xffu);
        hash[i + 4]  = (uint8_t)((ctx->state[1] >> (24u - i * 8u)) & 0xffu);
        hash[i + 8]  = (uint8_t)((ctx->state[2] >> (24u - i * 8u)) & 0xffu);
        hash[i + 12] = (uint8_t)((ctx->state[3] >> (24u - i * 8u)) & 0xffu);
        hash[i + 16] = (uint8_t)((ctx->state[4] >> (24u - i * 8u)) & 0xffu);
        hash[i + 20] = (uint8_t)((ctx->state[5] >> (24u - i * 8u)) & 0xffu);
        hash[i + 24] = (uint8_t)((ctx->state[6] >> (24u - i * 8u)) & 0xffu);
        hash[i + 28] = (uint8_t)((ctx->state[7] >> (24u - i * 8u)) & 0xffu);
    }
}

uint32_t h68p_crc32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

void h68p_sha256(const void *data, size_t len, uint8_t out[H68P_HASH_LEN]) {
    h68p_sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)data, len);
    sha256_final(&ctx, out);
}

const char *h68p_status_str(h68p_status_t st) {
    switch (st) {
    case H68P_OK: return "ok";
    case H68P_ERR_MAGIC: return "magic";
    case H68P_ERR_VERSION: return "version";
    case H68P_ERR_ABI: return "abi";
    case H68P_ERR_SIZE: return "size";
    case H68P_ERR_CRC: return "crc";
    case H68P_ERR_HASH: return "hash";
    case H68P_ERR_SERIAL: return "serial";
    case H68P_ERR_SIG: return "sig";
    case H68P_ERR_ENTRY: return "entry";
    default: return "?";
    }
}

h68p_status_t h68p_parse(const uint8_t *blob, uint32_t blob_len, h68p_view_t *out) {
    if (!blob || !out || blob_len < sizeof(h68p_header_t) + H68P_SIG_LEN)
        return H68P_ERR_SIZE;
    const h68p_header_t *hdr = (const h68p_header_t *)blob;
    if (hdr->magic != H68P_MAGIC) return H68P_ERR_MAGIC;
    if (hdr->version != H68P_VERSION) return H68P_ERR_VERSION;
    if (hdr->api_abi != H68P_API_ABI) return H68P_ERR_ABI;
    if (hdr->code_size == 0u || hdr->code_size > H68P_MAX_PAYLOAD) return H68P_ERR_SIZE;
    uint32_t need = (uint32_t)sizeof(h68p_header_t) + hdr->code_size + H68P_SIG_LEN;
    if (blob_len < need) return H68P_ERR_SIZE;
    if (hdr->entry_off >= hdr->code_size || (hdr->entry_off & 1u) != 0u)
        return H68P_ERR_ENTRY;
    out->hdr = hdr;
    out->payload = blob + sizeof(h68p_header_t);
    out->sig = out->payload + hdr->code_size;
    out->total_bytes = need;
    return H68P_OK;
}

h68p_status_t h68p_check_integrity(const h68p_view_t *v) {
    if (!v || !v->hdr || !v->payload) return H68P_ERR_SIZE;
    const h68p_header_t *hdr = v->hdr;
    uint32_t crc = h68p_crc32(hdr, offsetof(h68p_header_t, hdr_crc32));
    if (crc != hdr->hdr_crc32) return H68P_ERR_CRC;
    uint8_t dig[H68P_HASH_LEN];
    h68p_sha256(v->payload, hdr->code_size, dig);
    if (memcmp(dig, hdr->payload_sha256, H68P_HASH_LEN) != 0) return H68P_ERR_HASH;
    return H68P_OK;
}

h68p_status_t h68p_check_serial(const h68p_view_t *v, const uint8_t serial[H68P_SERIAL_LEN]) {
    if (!v || !v->hdr || !serial) return H68P_ERR_SERIAL;
    if (memcmp(v->hdr->device_serial, serial, H68P_SERIAL_LEN) != 0) return H68P_ERR_SERIAL;
    return H68P_OK;
}

h68p_status_t h68p_check_signature(const h68p_view_t *v,
                                   const uint8_t pub64[H68P_PUBKEY_LEN]) {
    if (!v || !v->hdr || !v->payload || !v->sig || !pub64) return H68P_ERR_SIG;

    uint8_t digest[H68P_HASH_LEN];
    h68p_sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)v->hdr, sizeof(h68p_header_t));
    sha256_update(&ctx, v->payload, v->hdr->code_size);
    sha256_final(&ctx, digest);

    // New micro-ecc API: uncompressed key is 0x04 || X || Y.
    uint8_t pub[65];
    pub[0] = 0x04;
    memcpy(pub + 1, pub64, H68P_PUBKEY_LEN);

    if (!h68p_uECC_verify(pub, digest, H68P_HASH_LEN, v->sig, h68p_uECC_secp256k1()))
        return H68P_ERR_SIG;
    return H68P_OK;
}

h68p_status_t h68p_verify(const uint8_t *blob, uint32_t blob_len,
                          const uint8_t serial[H68P_SERIAL_LEN],
                          const uint8_t pub64[H68P_PUBKEY_LEN],
                          h68p_view_t *out) {
    h68p_view_t v;
    h68p_status_t st = h68p_parse(blob, blob_len, &v);
    if (st != H68P_OK) return st;
    st = h68p_check_integrity(&v);
    if (st != H68P_OK) return st;
    st = h68p_check_serial(&v, serial);
    if (st != H68P_OK) return st;
    st = h68p_check_signature(&v, pub64);
    if (st != H68P_OK) return st;
    if (out) *out = v;
    return H68P_OK;
}
