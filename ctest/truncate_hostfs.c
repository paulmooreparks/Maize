/* maize-179 hostfs acceptance: ftruncate (SYS $4D) shrink / extend / EINVAL / EROFS.
 *
 * Runs against a writable sandbox root (cwd /home/user) with a :ro overlay at /ro, the
 * same grant shape as savefs_neg_hostfs. Checks:
 *   1. create ./trunc.dat, write 10 bytes "0123456789"
 *   2. ftruncate(fd, 4)  -> shrinks; fstat size == 4
 *   3. ftruncate(fd, 8)  -> extends; fstat size == 8
 *   4. ftruncate(fd, -1) -> EINVAL (22)
 *   5. reopen O_RDONLY and read 8: the first 4 bytes are "0123" (no stale tail) and the
 *      extend zero-filled bytes 4..7 (POSIX)
 *   6. open /ro/payload.txt O_RDONLY, ftruncate -> EROFS (30), the :ro write-gate on an fd
 * Prints a single "truncate: PASS" / "truncate: FAIL" line. */
#include "syscall.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40

/* Decode st_size (offset 48, little-endian 8 bytes) from the 144-byte struct stat image,
   the same field the savefs fixture reads back. */
static unsigned long stat_size(int fd) {
    unsigned char st[144];
    if (sys_fstat(fd, st) < 0) {
        return (unsigned long)-1;
    }
    unsigned long size = 0;
    for (int i = 55; i >= 48; --i) {
        size = (size << 8) | (unsigned long)st[i];
    }
    return size;
}

int main(void) {
    int ok = 1;
    static const char payload[] = "0123456789";   /* 10 bytes */

    long fd = sys_open("./trunc.dat", O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        ok = 0;
    } else {
        if (sys_write((int)fd, payload, 10) != 10) {
            ok = 0;
        }

        /* shrink to 4: the tail must be dropped. */
        if (sys_ftruncate((int)fd, 4) != 0) {
            ok = 0;
        }
        if (stat_size((int)fd) != 4UL) {
            ok = 0;
        }

        /* extend to 8: zero-filled per POSIX. */
        if (sys_ftruncate((int)fd, 8) != 0) {
            ok = 0;
        }
        if (stat_size((int)fd) != 8UL) {
            ok = 0;
        }

        /* a negative length is EINVAL. */
        long ei = sys_ftruncate((int)fd, -1);
        if (!(ei < 0 && (int)(-ei) == 22)) {
            ok = 0;
        }

        sys_close((int)fd);
    }

    /* Read the file back: 8 bytes, first 4 = "0123" (no stale tail), next 4 = zero-fill. */
    long rf = sys_open("./trunc.dat", O_RDONLY, 0);
    if (rf < 0) {
        ok = 0;
    } else {
        unsigned char buf[8];
        for (int i = 0; i < 8; ++i) {
            buf[i] = 0xFF;
        }
        long rd = sys_read((int)rf, buf, 8);
        if (rd != 8) {
            ok = 0;
        } else {
            for (int i = 0; i < 4; ++i) {
                if (buf[i] != (unsigned char)payload[i]) {
                    ok = 0;
                }
            }
            for (int i = 4; i < 8; ++i) {
                if (buf[i] != 0) {
                    ok = 0;
                }
            }
        }
        sys_close((int)rf);
    }

    /* EROFS: ftruncate on a fd opened from a :ro mount is rejected by the write-gate. */
    long ro = sys_open("/ro/payload.txt", O_RDONLY, 0);
    if (ro < 0) {
        ok = 0;
    } else {
        long er = sys_ftruncate((int)ro, 0);
        if (!(er < 0 && (int)(-er) == 30)) {
            ok = 0;
        }
        sys_close((int)ro);
    }

    sys_write(1, ok ? "truncate: PASS\n" : "truncate: FAIL\n", 15);
    return 0;
}
