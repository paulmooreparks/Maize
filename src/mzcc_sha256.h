/* mzcc_sha256.h (maize-274): a compact streaming SHA-256 for the mzcc
   content-addressed object cache. Cryptographic (not a fast non-crypto hash) is
   deliberate and load-bearing: the digest IS the cache key, so a collision would
   serve a wrong object, a correctness hazard for a build cache (spec section 1,
   matching the maize-263 cache family's maize_sha256 choice).

   Provenance: this is a from-scratch C11 implementation of FIPS 180-4 SHA-256,
   written for this file. SHA-256 is an unpatented public standard; the constants
   (K[], the initial H values) are the standard's own published values. No third
   party code is vendored, so there is no external license to track. */
#ifndef MZCC_SHA256_H
#define MZCC_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define MZCC_SHA256_DIGEST_LEN 32
#define MZCC_SHA256_HEX_LEN 64 /* excludes the NUL terminator */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   buflen;
} mzcc_sha256_ctx;

void mzcc_sha256_init(mzcc_sha256_ctx *c);
void mzcc_sha256_update(mzcc_sha256_ctx *c, const void *data, size_t len);
void mzcc_sha256_final(mzcc_sha256_ctx *c, uint8_t out[MZCC_SHA256_DIGEST_LEN]);

/* Write the lowercase-hex encoding of `digest` into `out` (>= 65 bytes),
   NUL-terminated. */
void mzcc_sha256_hex(const uint8_t digest[MZCC_SHA256_DIGEST_LEN],
                     char out[MZCC_SHA256_HEX_LEN + 1]);

/* In-tree known-answer test (maize-274 cycle-1 review nit 4): hashes the
   FIPS 180-4 Appendix B.1 "abc" test vector and compares the hex digest
   against the standard's published value. The cache key IS this hash, so a
   transform bug here silently serves wrong objects; guard it in-tree rather
   than resting on one cycle's manual verification. Returns 1 on match, 0 on
   mismatch. Callers (mzcc's startup, in debug builds) assert on this. */
int mzcc_sha256_selfcheck(void);

#endif /* MZCC_SHA256_H */
