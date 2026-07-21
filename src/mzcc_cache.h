/* mzcc_cache.h (maize-274): the content-addressed per-TU object cache. One
   .mzo per key; a key is SHA-256(toolchain_fingerprint || preprocessed_bytes ||
   tag) (spec section 2a). The correctness invariant is that no entry is ever
   served whose key preimage differs from the current (post-cpp tool bytes,
   preprocessed bytes, tag), so a stale object is unreachable by construction:
   the whole reason the cache is safe to share across worktrees and branches.

   The store is atomic (temp-write + rename) and happens ONLY after a
   fully-successful .mzo is produced, so a failed or racing pipeline never
   corrupts the cache (spec 2c). Every entry point is best-effort: a store
   failure (read-only cache dir, disk full) never fails the build, it just means
   the next build recompiles. */
#ifndef MZCC_CACHE_H
#define MZCC_CACHE_H

#include <stddef.h>

#include "mzcc_sha256.h" /* MZCC_SHA256_HEX_LEN */

/* Register the post-cpp tool paths whose raw bytes form the toolchain
   fingerprint (cproc-qbe, qbe, mazm; the running mzcc binary is added
   internally via mzcc_self_path). Call after resolve_toolchain. Idempotent: the
   fingerprint is memoized and only recomputed when a path changes (a batch loop
   re-resolving the same preset pays the ~10ms binary hash once). */
void mzcc_cache_configure(const char *cproc_qbe, const char *qbe, const char *mazm);

/* 1 when the object cache is active. Disabled by MAIZE_NO_OBJECT_CACHE=1, which
   the parity reference build relies on (spec section 5). */
int mzcc_cache_enabled(void);

/* Derive the 64-hex cache key for a TU into `out` (>= MZCC_SHA256_HEX_LEN + 1
   bytes), NUL-terminated. Preimage: fingerprint(32) || 0 || preprocessed(plen)
   || 0 || tag || 0. `tag` is the object tag (e.g. "rt_string.body", "crt0"). */
void mzcc_cache_key(const char *preprocessed, size_t plen, const char *tag,
                    char out[MZCC_SHA256_HEX_LEN + 1]);

/* Look up `key`. On a hit, copy the cached .mzo to `dst_path` and return 1. On
   a miss (or any read/copy error, treated as a miss) return 0. */
int mzcc_cache_lookup(const char *key, const char *dst_path);

/* Store the bytes of `src_path` (a freshly produced, fully-successful .mzo)
   under `key` via an atomic temp-write + rename. Best-effort: returns 0 on
   success, -1 on any failure (the caller ignores the result). */
int mzcc_cache_store(const char *key, const char *src_path);

#endif /* MZCC_CACHE_H */
