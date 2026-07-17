/* fs_forward.c -- maize-94 fixture (decision 8941), run UNDER quesOS.
 *
 * Proves quesOS forwards the native hostfs file/dir subset (fstat, lseek, getdents64,
 * unlink, mkdir, rename, ftruncate) to a user process, each bouncing its fixed-size
 * struct / directory-record buffer through the kernel staging area. Before this card a
 * quesOS process got -ENOSYS on every one of these. Runs with a writable /rw mount: it
 * makes a directory, creates + writes a file, fstats its size, ftruncates it, lseek-reads
 * it, renames it, lists the directory with getdents64, and unlinks it, self-checking each
 * step. Prints "fs-forward: PASS", or a FAIL marker naming the failing step (never exits
 * silently -- the maize-236 lesson).
 */

int  printf(const char *, ...);
long sys_open(const char *path, long flags, long mode);
long sys_read(long fd, void *buf, long count);
long sys_write(long fd, const void *buf, long count);
long sys_close(long fd);
long sys_fstat(long fd, void *statbuf);
long sys_lseek(long fd, long offset, long whence);
long sys_getdents64(long fd, void *dirp, long count);
long sys_unlink(const char *path);
long sys_mkdir(const char *path, long mode);
long sys_rename(const char *oldp, const char *newp);
long sys_ftruncate(long fd, long length);

#define O_RDONLY    0x0
#define O_WRONLY    0x1
#define O_RDWR      0x2
#define O_CREAT     0x40
#define O_TRUNC     0x200
#define O_DIRECTORY 0x10000

/* Decode the little-endian 8-byte st_size field (its own loop; one loop per function to
 * stay inside the qbe-maize backend's pointer-indexing budget, cf. dirent.c). */
static long rd_u64le(const unsigned char *p) {
    long v = 0;
    int i;
    for (i = 7; i >= 0; --i) { v = (v << 8) | (long)p[i]; }
    return v;
}

int main(void) {
    unsigned char stbuf[144];
    unsigned char db[1024];
    char two[2];
    long fd, n, off;
    int found_b;

    if (sys_mkdir("/rw/d", 0755) < 0) { printf("fs-forward: FAIL mkdir\n"); return 1; }

    fd = sys_open("/rw/d/a", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { printf("fs-forward: FAIL open-create\n"); return 1; }
    if (sys_write(fd, "hello", 5) != 5) { printf("fs-forward: FAIL write\n"); return 1; }
    if (sys_fstat(fd, stbuf) < 0 || rd_u64le(stbuf + 48) != 5) { printf("fs-forward: FAIL fstat\n"); return 1; }
    if (sys_ftruncate(fd, 3) < 0) { printf("fs-forward: FAIL ftruncate\n"); return 1; }
    if (sys_fstat(fd, stbuf) < 0 || rd_u64le(stbuf + 48) != 3) { printf("fs-forward: FAIL fstat2\n"); return 1; }
    sys_close(fd);

    fd = sys_open("/rw/d/a", O_RDWR, 0);
    if (fd < 0) { printf("fs-forward: FAIL reopen\n"); return 1; }
    if (sys_lseek(fd, 1, O_RDONLY /* SEEK_SET=0 */) != 1) { printf("fs-forward: FAIL lseek\n"); return 1; }
    if (sys_read(fd, two, 2) != 2 || two[0] != 'e' || two[1] != 'l') { printf("fs-forward: FAIL lseek-read\n"); return 1; }
    sys_close(fd);

    if (sys_rename("/rw/d/a", "/rw/d/b") < 0) { printf("fs-forward: FAIL rename\n"); return 1; }

    fd = sys_open("/rw/d", O_DIRECTORY, 0);
    if (fd < 0) { printf("fs-forward: FAIL opendir\n"); return 1; }
    n = sys_getdents64(fd, db, 1024);
    sys_close(fd);
    if (n <= 0) { printf("fs-forward: FAIL getdents-empty\n"); return 1; }
    found_b = 0;
    off = 0;
    while (off < n) {
        unsigned short reclen = (unsigned short)(db[off + 16] | (db[off + 17] << 8));
        if (db[off + 19] == 'b' && db[off + 20] == 0) { found_b = 1; }
        if (reclen == 0) { break; }
        off += reclen;
    }
    if (!found_b) { printf("fs-forward: FAIL getdents-missing-b\n"); return 1; }

    if (sys_unlink("/rw/d/b") < 0) { printf("fs-forward: FAIL unlink\n"); return 1; }

    printf("fs-forward: PASS\n");
    return 0;
}
