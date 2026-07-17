/* cwd_resolve.c -- maize-94 fixture (decision 8940), run UNDER quesOS.
 *
 * Proves the per-process working directory and relative-path resolution: getcwd starts at
 * "/", chdir into a subdirectory updates it, a RELATIVE open resolves against the cwd (so
 * it lands at the expected absolute path -- the shell's cd must follow into the process's
 * own opens, not just a display string), chdir("..") pops a component, and chdir to a
 * nonexistent path fails (validation). Runs with a writable /rw mount. Prints
 * "cwd-resolve: PASS" or a FAIL marker naming the failing step.
 */

int  printf(const char *, ...);
long sys_open(const char *path, long flags, long mode);
long sys_write(long fd, const void *buf, long count);
long sys_close(long fd);
long sys_mkdir(const char *path, long mode);
long sys_chdir(const char *path);
long sys_getcwd(char *buf, long size);

#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_CREAT  0x40

/* Its own function so main carries only its own control flow (one loop per function). */
static int streq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) { ++i; }
    return a[i] == b[i];
}

int main(void) {
    char cwd[256];
    long fd;

    if (sys_getcwd(cwd, 256) <= 0 || !streq(cwd, "/")) { printf("cwd-resolve: FAIL getcwd-root\n"); return 1; }

    if (sys_mkdir("/rw/sub", 0755) < 0) { printf("cwd-resolve: FAIL mkdir\n"); return 1; }
    if (sys_chdir("/rw/sub") < 0) { printf("cwd-resolve: FAIL chdir\n"); return 1; }
    if (sys_getcwd(cwd, 256) <= 0 || !streq(cwd, "/rw/sub")) { printf("cwd-resolve: FAIL getcwd-sub\n"); return 1; }

    /* A relative open must resolve against the cwd and create /rw/sub/f. */
    fd = sys_open("f", O_WRONLY | O_CREAT, 0644);
    if (fd < 0) { printf("cwd-resolve: FAIL rel-open\n"); return 1; }
    sys_write(fd, "x", 1);
    sys_close(fd);
    fd = sys_open("/rw/sub/f", O_RDONLY, 0);
    if (fd < 0) { printf("cwd-resolve: FAIL abs-verify\n"); return 1; }
    sys_close(fd);

    /* chdir("..") pops one component back to /rw. */
    if (sys_chdir("..") < 0) { printf("cwd-resolve: FAIL chdir-up\n"); return 1; }
    if (sys_getcwd(cwd, 256) <= 0 || !streq(cwd, "/rw")) { printf("cwd-resolve: FAIL getcwd-up\n"); return 1; }

    /* chdir to a nonexistent directory must fail (existence validation). */
    if (sys_chdir("/rw/nope") >= 0) { printf("cwd-resolve: FAIL chdir-nonexistent\n"); return 1; }

    printf("cwd-resolve: PASS\n");
    return 0;
}
