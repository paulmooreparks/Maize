/* filter.c -- maize-93 AC5 pipeline stage 2, run UNDER quesOS via execve.
 * Reads stdin (the producer's pipe), upper-cases it, and writes stdout (the consumer's
 * pipe), until EOF. This is the classic filter shape a shell wires between two pipes. */
long sys_read(long fd, void *buf, long count);
long sys_write(long fd, const void *buf, long count);

int main(void) {
    char buf[64];
    long n;
    while ((n = sys_read(0, buf, (long)sizeof buf)) > 0) {
        long i;
        for (i = 0; i < n; ++i) {
            if (buf[i] >= 'a' && buf[i] <= 'z') { buf[i] = (char)(buf[i] - 32); }
        }
        sys_write(1, buf, n);
    }
    return 0;
}
