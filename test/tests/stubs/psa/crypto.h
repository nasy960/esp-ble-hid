#pragma once
#include <stddef.h>
#include <stdint.h>
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 1
static inline int psa_crypto_init(void) { return 0; }
static inline int psa_hash_compute(int algorithm,const uint8_t *data,size_t n,uint8_t *out,size_t cap,size_t *len)
{ (void)algorithm;(void)data;(void)n;(void)out;(void)cap;(void)len;return -1; }
