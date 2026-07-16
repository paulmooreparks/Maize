/* consumer.c -- maize-93 AC5 pipeline stage 3, run UNDER quesOS via execve.
 * Reads stdin (the filter's pipe) to EOF and verifies it received the upper-cased
 * payload, then prints the pipeline verdict to the real stdout. */
int printf(const char *, ...);
long sys_read(long fd, void *buf, long count);

int main(void) {
    char buf[64];
    long total = 0;
    long n;
    while ((n = sys_read(0, buf + total, (long)sizeof buf - total)) > 0) { total += n; }
    int ok = (total == 6)
             && buf[0] == 'H' && buf[1] == 'E' && buf[2] == 'L'
             && buf[3] == 'L' && buf[4] == 'O' && buf[5] == '\n';
    printf(ok ? "pipeline: PASS\n" : "pipeline: FAIL\n");
    return 0;
}
