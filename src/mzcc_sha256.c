/* mzcc_sha256.c (maize-274): FIPS 180-4 SHA-256, streaming. See mzcc_sha256.h
   for provenance (from-scratch implementation of the public SHA-256 standard;
   no vendored third-party code). The transform is the textbook one; the K[]
   table and the initial hash values are the standard's published constants. */
#include "mzcc_sha256.h"

#include <string.h>

static uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void sha256_transform(mzcc_sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | ((uint32_t)p[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

void mzcc_sha256_init(mzcc_sha256_ctx *c) {
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->bitlen = 0;
    c->buflen = 0;
}

void mzcc_sha256_update(mzcc_sha256_ctx *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->bitlen += (uint64_t)len * 8;
    while (len > 0) {
        size_t take = 64 - c->buflen;
        if (take > len) {
            take = len;
        }
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        len -= take;
        if (c->buflen == 64) {
            sha256_transform(c, c->buf);
            c->buflen = 0;
        }
    }
}

void mzcc_sha256_final(mzcc_sha256_ctx *c, uint8_t out[MZCC_SHA256_DIGEST_LEN]) {
    uint64_t bitlen = c->bitlen;
    uint8_t pad = 0x80;
    mzcc_sha256_update(c, &pad, 1);
    uint8_t zero = 0x00;
    while (c->buflen != 56) {
        mzcc_sha256_update(c, &zero, 1);
    }
    uint8_t lenbytes[8];
    for (int i = 0; i < 8; ++i) {
        lenbytes[i] = (uint8_t)(bitlen >> (56 - i * 8));
    }
    /* update() would re-add these 8 bytes to bitlen, but bitlen is no longer
       read after this point, so appending the length directly through the
       buffered path is correct: it fills the block to 64 and transforms. */
    mzcc_sha256_update(c, lenbytes, 8);

    for (int i = 0; i < 8; ++i) {
        out[i * 4]     = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->state[i]);
    }
}

void mzcc_sha256_hex(const uint8_t digest[MZCC_SHA256_DIGEST_LEN],
                     char out[MZCC_SHA256_HEX_LEN + 1]) {
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < MZCC_SHA256_DIGEST_LEN; ++i) {
        out[i * 2]     = hexd[(digest[i] >> 4) & 0xf];
        out[i * 2 + 1] = hexd[digest[i] & 0xf];
    }
    out[MZCC_SHA256_HEX_LEN] = '\0';
}

int mzcc_sha256_selfcheck(void) {
    /* FIPS 180-4 Appendix B.1: SHA-256("abc"). */
    static const char kat_input[] = "abc";
    static const char kat_hex[] =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

    mzcc_sha256_ctx c;
    uint8_t digest[MZCC_SHA256_DIGEST_LEN];
    char hex[MZCC_SHA256_HEX_LEN + 1];

    mzcc_sha256_init(&c);
    mzcc_sha256_update(&c, kat_input, sizeof(kat_input) - 1);
    mzcc_sha256_final(&c, digest);
    mzcc_sha256_hex(digest, hex);

    return strcmp(hex, kat_hex) == 0;
}
