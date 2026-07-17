/* bigwrite_native.c -- maize-250 fixture (AC 9108), run UNDER quesOS.
 *
 * Proves native_write delivers a SINGLE write() larger than QUESOS_IOBUF_CAP (4096) in
 * full, rather than silently truncating the tail as it did before this card (the root
 * cause of kilo's garbled full-screen paint: editorRefreshScreen writes the whole frame
 * in one write(), which routinely exceeds 4096 bytes). The fixture issues ONE
 * sys_write(1, buf, 10000) and asserts the returned count is the full 10000 -- since the
 * raw sys_write's return is the host's own accepted byte count, a return of 10000 proves
 * all 10000 bytes reached the host, not just the first 4096.
 *
 * The 10000-byte payload is written to fd 1 first (so it is really observed on the host
 * side; the harness also wc -c's it), then the verdict line. Output on success:
 * "native-bigwrite: PASS n=10000".
 */

int  printf(const char *, ...);
long sys_write(long fd, const void *buf, long count);

#define BIG_N 10000

static char g_buf[BIG_N];

int main(void) {
    long i, w;

    for (i = 0; i < BIG_N; ++i) { g_buf[i] = 'x'; }

    w = sys_write(1, g_buf, BIG_N);   /* ONE write of the whole buffer, no guest-side loop */

    /* Terminate the payload line so the 10000 'x' bytes do not run into the verdict. */
    sys_write(1, "\n", 1);

    if (w != BIG_N) {
        printf("native-bigwrite: FAIL short w=%d\n", (int)w);
        return 1;
    }
    printf("native-bigwrite: PASS n=%d\n", (int)w);
    return 0;
}
