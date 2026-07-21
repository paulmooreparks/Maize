/* mzcc_cache.c (maize-274): implementation of the content-addressed per-TU
   object cache (spec section 2). Reuses mzcc.c's file/path helpers (read_file,
   copy_file, mkdir_p, path_exists, joinstr) via mzcc_internal.h and the process
   primitives (mzcc_self_path, mz_mutex_*, mzcc_pid) via mzcc_proc.h; the SHA-256
   is mzcc_sha256.{c,h}. No platform #ifdef lives here beyond the cache-root
   default: the atomicity is plain temp-write + C rename(), which is atomic on
   both NTFS and ext4 for a same-directory rename. */
#include "mzcc_cache.h"

#include "mzcc_internal.h"
#include "mzcc_proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- toolchain fingerprint (memoized) ---------------------------------- */

static char   *g_fp_cproc = NULL;   /* the paths the memoized fingerprint covers */
static char   *g_fp_qbe   = NULL;
static char   *g_fp_mazm  = NULL;
static char   *g_fp_self  = NULL;
static uint8_t g_fingerprint[MZCC_SHA256_DIGEST_LEN];
static int     g_fp_valid = 0;

/* Hash one file's raw bytes into the running SHA-256, length-prefixed so two
   tools cannot alias by a boundary shift. A tool that fails to read hashes as a
   zero-length contribution (with its own length prefix), which still rolls the
   key deterministically. */
static void fp_absorb_file(mzcc_sha256_ctx *c, const char *path) {
    ByteBuf b;
    if (path && read_file(path, &b) == 0) {
        uint8_t lp[8];
        for (int i = 0; i < 8; ++i) { lp[i] = (uint8_t)((uint64_t)b.len >> (56 - i * 8)); }
        mzcc_sha256_update(c, lp, 8);
        mzcc_sha256_update(c, b.data ? b.data : "", b.len);
        byte_buf_free(&b);
    } else {
        /* Unreadable tool: contribute a deterministic zero-length record so the
           fingerprint still rolls consistently. */
        uint8_t lp[8] = {0};
        mzcc_sha256_update(c, lp, 8);
    }
}

static int str_eq(const char *a, const char *b) {
    if (a == b) { return 1; }
    if (!a || !b) { return 0; }
    return strcmp(a, b) == 0;
}

void mzcc_cache_configure(const char *cproc_qbe, const char *qbe, const char *mazm) {
    char self[4096];
    if (mzcc_self_path(NULL, self, sizeof(self)) != 0) {
        self[0] = '\0';
    }
    to_slashes(self);

    /* Memoized: skip the ~10ms binary hash when nothing changed. */
    if (g_fp_valid && str_eq(g_fp_cproc, cproc_qbe) && str_eq(g_fp_qbe, qbe) &&
        str_eq(g_fp_mazm, mazm) && str_eq(g_fp_self, self)) {
        return;
    }

    free(g_fp_cproc); free(g_fp_qbe); free(g_fp_mazm); free(g_fp_self);
    g_fp_cproc = cproc_qbe ? xstrdup(cproc_qbe) : NULL;
    g_fp_qbe   = qbe ? xstrdup(qbe) : NULL;
    g_fp_mazm  = mazm ? xstrdup(mazm) : NULL;
    g_fp_self  = xstrdup(self);

    mzcc_sha256_ctx c;
    mzcc_sha256_init(&c);
    fp_absorb_file(&c, g_fp_cproc);
    fp_absorb_file(&c, g_fp_qbe);
    fp_absorb_file(&c, g_fp_mazm);
    fp_absorb_file(&c, g_fp_self);
    mzcc_sha256_final(&c, g_fingerprint);
    g_fp_valid = 1;
}

/* ---- enable toggle + root resolution ----------------------------------- */

int mzcc_cache_enabled(void) {
    const char *no = getenv("MAIZE_NO_OBJECT_CACHE");
    if (no && no[0] == '1' && no[1] == '\0') {
        return 0;
    }
    return 1;
}

/* Resolve the cache root once (memoized). Precedence: MAIZE_CACHE_DIR (the
   ratified override, decision 9649), then MAIZE_OBJECT_CACHE (the spec-prose
   alias, section 2b), then the shared per-user default: %LOCALAPPDATA%/maize/
   objects on Windows, ${XDG_CACHE_HOME:-$HOME/.cache}/maize/objects on POSIX.
   Returns NULL when no usable location can be resolved (cache silently off). */
static const char *cache_root(void) {
    static char *root = NULL;
    static int resolved = 0;
    if (resolved) {
        return root;
    }
    resolved = 1;

    const char *ov = getenv("MAIZE_CACHE_DIR");
    if (!ov || !ov[0]) { ov = getenv("MAIZE_OBJECT_CACHE"); }
    if (ov && ov[0]) {
        /* An explicit override names the cache dir directly (no extra suffix). */
        root = xstrdup(ov);
        to_slashes(root);
        return root;
    }

#if defined(_WIN32)
    const char *base = getenv("LOCALAPPDATA");
    if (base && base[0]) {
        root = joinstr(base, "/maize/objects", NULL, NULL);
        to_slashes(root);
        return root;
    }
#endif
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0]) {
        root = joinstr(xdg, "/maize/objects", NULL, NULL);
        to_slashes(root);
        return root;
    }
    const char *home = getenv("HOME");
#if defined(_WIN32)
    if (!home || !home[0]) { home = getenv("USERPROFILE"); }
#endif
    if (home && home[0]) {
        root = joinstr(home, "/.cache/maize/objects", NULL, NULL);
        to_slashes(root);
        return root;
    }
    root = NULL;
    return root;
}

/* ---- key derivation ---------------------------------------------------- */

void mzcc_cache_key(const char *preprocessed, size_t plen, const char *tag,
                    char out[MZCC_SHA256_HEX_LEN + 1]) {
    static const uint8_t sep = 0x00;
    mzcc_sha256_ctx c;
    mzcc_sha256_init(&c);
    mzcc_sha256_update(&c, g_fingerprint, sizeof(g_fingerprint));
    mzcc_sha256_update(&c, &sep, 1);
    mzcc_sha256_update(&c, preprocessed, plen);
    mzcc_sha256_update(&c, &sep, 1);
    mzcc_sha256_update(&c, tag, tag ? strlen(tag) : 0);
    mzcc_sha256_update(&c, &sep, 1);
    uint8_t dig[MZCC_SHA256_DIGEST_LEN];
    mzcc_sha256_final(&c, dig);
    mzcc_sha256_hex(dig, out);
}

/* ---- sharded path layout ----------------------------------------------- */

/* <root>/<key[0:2]>/<key>.mzo, or NULL if the cache is off / rootless.
   Fresh allocation. */
static char *entry_path(const char *key) {
    const char *root = cache_root();
    if (!root) {
        return NULL;
    }
    char shard[3];
    shard[0] = key[0];
    shard[1] = key[1];
    shard[2] = '\0';
    char *dir = joinstr(root, "/", shard, NULL);
    char *p = joinstr(dir, "/", key, ".mzo");
    free(dir);
    return p;
}

int mzcc_cache_lookup(const char *key, const char *dst_path) {
    char *entry = entry_path(key);
    if (!entry) {
        return 0;
    }
    if (!path_exists(entry)) {
        free(entry);
        return 0;
    }
    int rc = copy_file(entry, dst_path);
    free(entry);
    return rc == 0 ? 1 : 0;
}

/* ---- atomic store ------------------------------------------------------ */

static MzMutex *g_store_mtx = NULL;
static unsigned long g_store_ctr = 0;

int mzcc_cache_store(const char *key, const char *src_path) {
    const char *root = cache_root();
    if (!root) {
        return -1;
    }
    char shard[3];
    shard[0] = key[0];
    shard[1] = key[1];
    shard[2] = '\0';
    char *dir = joinstr(root, "/", shard, NULL);
    /* mkdir -p the shard dir; a concurrent creator is harmless. */
    if (!path_exists(dir)) {
        mkdir_p(dir);
    }
    char *final = joinstr(dir, "/", key, ".mzo");

    /* Already present (another writer of the same key won; identical bytes):
       nothing to do. */
    if (path_exists(final)) {
        free(dir); free(final);
        return 0;
    }

    /* Unique temp name: <pid>.<counter>.tmp under the same shard dir, so the
       rename is same-directory (atomic on NTFS and ext4). */
    if (!g_store_mtx) {
        g_store_mtx = mz_mutex_new();
    }
    unsigned long ctr;
    mz_mutex_lock(g_store_mtx);
    ctr = ++g_store_ctr;
    mz_mutex_unlock(g_store_mtx);

    char suffix[64];
    snprintf(suffix, sizeof(suffix), ".%lu.%lu.tmp", mzcc_pid(), ctr);
    char *tmp = joinstr(dir, "/", key, suffix);
    free(dir);

    /* Copy the produced .mzo bytes into the temp file, then rename onto the
       final name. The presence of the fully-renamed file is the completeness
       marker; a torn tmp is never renamed. */
    ByteBuf b;
    if (read_file(src_path, &b) != 0) {
        byte_buf_free(&b);
        free(tmp); free(final);
        return -1;
    }
    int wrc = write_file(tmp, b.data, b.len);
    byte_buf_free(&b);
    if (wrc != 0) {
        remove(tmp);
        free(tmp); free(final);
        return -1;
    }

    if (rename(tmp, final) != 0) {
        /* A racing writer may have created `final` between our check and here
           (Windows rename fails onto an existing file). If it now exists, that
           writer's bytes are identical to ours: discard our tmp and succeed. */
        if (path_exists(final)) {
            remove(tmp);
            free(tmp); free(final);
            return 0;
        }
        remove(tmp);
        free(tmp); free(final);
        return -1;
    }
    free(tmp); free(final);
    return 0;
}
