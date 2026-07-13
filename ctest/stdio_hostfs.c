/* maize-120 acceptance: the FILE* stdio + dirent layer over the hostfs stubs.
 *
 * Four checks, all gated behind one "stdio: PASS" line (matching the stat/rofs
 * fixture convention):
 *
 *   1. DOOM/WAD read path (:ro): fopen("rb") a 4096-byte binary file the harness
 *      pre-writes cycling all values 0x00..0xFF (so 0x0A/0x0D are present and any
 *      text-mode mangling is caught), fread it whole, then fseek/ftell to a known
 *      offset and re-read, verifying every byte is exactly what was written. This is
 *      the W_AddFile access pattern, binary-clean.
 *   2. Write round-trip (:rw): fopen("wb"), fwrite a 2000-byte payload, ftell the
 *      buffered position, fseek (which flushes the write buffer), fclose; then reopen
 *      "rb" and fread it back equal, exercising the buffer + flush path.
 *   3. Directory: opendir/readdir/closedir over the :rw mount, confirming the written
 *      rt.dat name enumerates (decoding the frozen linux_dirent64 layout).
 *   4. sprintf: a mixed %d/%s/%x format checked byte-for-byte.
 *
 * Flush-on-exit (AC 8276): in the normal run the fixture opens /rw/unclosed.dat for
 * writing, fwrites a known payload, and RETURNS from main WITHOUT fclose. The atexit-
 * registered __stdio_flush_all must land those bytes; the harness verifies the host
 * file afterward. Run with argv[1] == "noflush" the fixture instead fwrites to
 * /rw/noflush.dat and calls _Exit(0), which bypasses the atexit registry, so that
 * file must stay empty (C-conformant: _Exit does not flush).
 *
 * Every function is kept small and single-loop, and large buffers are malloc'd rather
 * than stack-allocated, to stay inside the pinned qbe-maize backend's budget.
 */
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "dirent.h"
#include "fcntl.h"   /* SEEK_SET */

#define BINLEN 4096

/* Verify len bytes hold the cycling pattern byte[k] == (base + k) & 0xFF. */
static int
verify_pattern(const unsigned char *buf, long base, long len)
{
    long k;
    for (k = 0; k < len; k++) {
        if (buf[k] != (unsigned char)((base + k) & 0xFF))
            return 0;
    }
    return 1;
}

static int
test_ro_read(void)
{
    FILE *f = fopen("/ro/bin.dat", "rb");
    unsigned char *buf;
    long n;
    int ok = 1;

    if (f == NULL)
        return 0;
    buf = malloc(BINLEN);
    if (buf == NULL) {
        fclose(f);
        return 0;
    }

    n = (long)fread(buf, 1, BINLEN, f);
    if (n != BINLEN || !verify_pattern(buf, 0, BINLEN))
        ok = 0;

    /* Seek to a known offset and re-read (the WAD seek/tell pattern). */
    if (fseek(f, 1000, SEEK_SET) != 0 || ftell(f) != 1000)
        ok = 0;
    n = (long)fread(buf, 1, 64, f);
    if (n != 64 || !verify_pattern(buf, 1000, 64))
        ok = 0;
    if (ftell(f) != 1064)
        ok = 0;

    free(buf);
    fclose(f);
    return ok;
}

static int
test_rw_roundtrip(void)
{
    FILE *f = fopen("/rw/rt.dat", "wb");
    unsigned char *wbuf;
    unsigned char *rbuf;
    long i, n;
    long len = 2000;
    int ok = 1;

    if (f == NULL)
        return 0;
    wbuf = malloc(len);
    rbuf = malloc(len);
    if (wbuf == NULL || rbuf == NULL) {
        fclose(f);
        return 0;
    }

    for (i = 0; i < len; i++)
        wbuf[i] = (unsigned char)((i * 7) & 0xFF);

    if ((long)fwrite(wbuf, 1, len, f) != len)
        ok = 0;
    if (ftell(f) != len)                    /* exact buffered-write tell */
        ok = 0;
    if (fseek(f, 500, SEEK_SET) != 0 || ftell(f) != 500)  /* fseek flushes the buffer */
        ok = 0;
    if (fclose(f) != 0)
        ok = 0;

    f = fopen("/rw/rt.dat", "rb");
    if (f == NULL) {
        free(wbuf);
        free(rbuf);
        return 0;
    }
    n = (long)fread(rbuf, 1, len, f);
    if (n != len || memcmp(wbuf, rbuf, len) != 0)
        ok = 0;
    fclose(f);

    free(wbuf);
    free(rbuf);
    return ok;
}

static int
test_dir(void)
{
    DIR *d = opendir("/rw");
    struct dirent *e;
    int found = 0;

    if (d == NULL)
        return 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, "rt.dat") == 0)
            found = 1;
    }
    closedir(d);
    return found;
}

static int
test_sprintf(void)
{
    char buf[64];
    int r = sprintf(buf, "%d/%s/%x", 42, "hi", 255);
    return (r == 8 && strcmp(buf, "42/hi/ff") == 0);
}

int
main(int argc, char **argv)
{
    int ok = 1;
    FILE *uf;

    /* Negative flush case: fwrite then _Exit (bypasses atexit) -> file stays empty. */
    if (argc >= 2 && strcmp(argv[1], "noflush") == 0) {
        FILE *nf = fopen("/rw/noflush.dat", "wb");
        if (nf != NULL)
            fwrite("must-not-land", 1, 13, nf);
        _Exit(0);
    }

    if (!test_ro_read())
        ok = 0;
    if (!test_rw_roundtrip())
        ok = 0;
    if (!test_dir())
        ok = 0;
    if (!test_sprintf())
        ok = 0;

    /* Positive flush case: buffered write stream, no fclose; atexit must land it. */
    uf = fopen("/rw/unclosed.dat", "wb");
    if (uf != NULL)
        fwrite("flush-on-exit-proof", 1, 19, uf);

    puts(ok ? "stdio: PASS" : "stdio: FAIL");
    return 0;
}
