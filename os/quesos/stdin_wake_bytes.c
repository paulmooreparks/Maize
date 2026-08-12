/* stdin_wake_bytes.c -- maize-313 AC-6 fixture, run UNDER quesOS.
 *
 * The second and subsequent bytes must wake the parked CPU as reliably as the first. The
 * source disarms on its own raise and is re-armed only by an acknowledgement, and that
 * acknowledgement is published by the readiness probe whenever it answers 0, which the park
 * hook runs on every path into the park. Delete the acknowledgement and the first byte still
 * arrives while the second never does, so this fixture reads a long run of bytes with the
 * kernel reaching the idle path between them rather than a single byte.
 *
 * The bytes are a repeating 'a'..'z' cycle so a dropped or duplicated byte fails the check
 * at the position it happened rather than only at the count.
 *
 * Output on success: stdin-wake-bytes: PASS <count>
 */

int  printf(const char *, ...);
long sys_read(long fd, void *buf, long count);

#define WANT 200

int main(void) {
    int i;
    for (i = 0; i < WANT; ++i) {
        char c = 0;
        long r = sys_read(0, &c, 1);
        if (r != 1) {
            printf("stdin-wake-bytes: FAIL short at %d (read returned %ld)\n", i, r);
            return 0;
        }
        if (c != (char)('a' + (i % 26))) {
            printf("stdin-wake-bytes: FAIL wrong byte at %d\n", i);
            return 0;
        }
    }
    printf("stdin-wake-bytes: PASS %d\n", WANT);
    return 0;
}
