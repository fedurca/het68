// h68p_uecc_wrap.c — vendored micro-ecc with renamed symbols to avoid
// colliding with BTstack's older micro-ecc (secp256r1).
// Force pure-C (no ARM asm) so we do not pick up BTstack's asm_arm.inc.
#define uECC_PLATFORM 0 /* uECC_arch_other */
#define uECC_ASM 0      /* uECC_asm_none */
#define uECC_SUPPORTS_secp160r1 0
#define uECC_SUPPORTS_secp192r1 0
#define uECC_SUPPORTS_secp224r1 0
#define uECC_SUPPORTS_secp256r1 0
#define uECC_SUPPORTS_secp256k1 1
#define uECC_OPTIMIZATION_LEVEL 2

#define uECC_version            h68p_uECC_version
#define uECC_curve_private_key_size h68p_uECC_curve_private_key_size
#define uECC_curve_public_key_size  h68p_uECC_curve_public_key_size
#define uECC_make_key           h68p_uECC_make_key
#define uECC_shared_secret      h68p_uECC_shared_secret
#define uECC_sign               h68p_uECC_sign
#define uECC_verify             h68p_uECC_verify
#define uECC_valid_public_key   h68p_uECC_valid_public_key
#define uECC_compute_public_key h68p_uECC_compute_public_key
#define uECC_sign_deterministic h68p_uECC_sign_deterministic
#define uECC_set_rng            h68p_uECC_set_rng
#define uECC_get_rng            h68p_uECC_get_rng
#define uECC_secp256k1          h68p_uECC_secp256k1
#define uECC_secp160r1          h68p_uECC_secp160r1
#define uECC_secp192r1          h68p_uECC_secp192r1
#define uECC_secp224r1          h68p_uECC_secp224r1
#define uECC_secp256r1          h68p_uECC_secp256r1
#define uECC_compress           h68p_uECC_compress
#define uECC_decompress         h68p_uECC_decompress

#include "uECC.c"
