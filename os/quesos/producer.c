/* producer.c -- maize-93 AC5 pipeline stage 1, run UNDER quesOS via execve.
 * Writes a fixed payload to stdout, which the pipeline launcher has wired to the
 * first pipe's write end. */
long sys_write(long fd, const void *buf, long count);

int main(void) {
    sys_write(1, "hello\n", 6);
    return 0;
}
