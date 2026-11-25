// h68p_uecc.h — renamed micro-ecc secp256k1 entry points (see h68p_uecc_wrap.c).
#pragma once
#include <stdint.h>

typedef const struct uECC_Curve_t * h68p_uECC_Curve;
h68p_uECC_Curve h68p_uECC_secp256k1(void);
int h68p_uECC_verify(const uint8_t *public_key,
                     const uint8_t *message_hash,
                     unsigned hash_size,
                     const uint8_t *signature,
                     h68p_uECC_Curve curve);
