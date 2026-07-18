/* maize-255 hostfs acceptance: merged root directory listing when a real "/" mount
 * is present alongside other granted mounts (doc section 8; AC 9286, 9287, 9289,
 * 9290, 9291, 9292, 9293, 9303).
 *
 * Same shape as ls_hostfs.c (open "/" with O_DIRECTORY, loop sys_getdents64,
 * print each d_name on its own line), except:
 *   - a deliberately small read buffer (64 bytes, not ls_hostfs.c's 1024) to force
 *     multiple getdents64 calls, exercising the mount-name-phase-to-physical-phase
 *     cursor transition (AC 9292 paging);
 *   - names are printed with an "N1:" prefix on the first full read, then the fd
 *     is rewound with sys_lseek(fd, 0, SEEK_SET) and read again with an "N2:"
 *     prefix (AC 9293: rewind reproduces the same merged set). The harness
 *     compares the sorted N1 set and the sorted N2 set independently, both
 *     against the expected merged set;
 *   - a final "lseekbad:" line asserts that any (whence, offset) on the merged
 *     root fd other than (SEEK_SET, 0) returns -EINVAL (AC 9303), matching the
 *     synthetic root's existing rewind-only posture; HOSTFS_EINVAL is 22.
 * Byte indexing only (element size 1), so the address math never multiplies. */
#include "syscall.h"

static void write_str(const char *s) {
    unsigned long len = 0;
    while (s[len] != 0) {
        ++len;
    }
    sys_write(1, s, len);
}

static void list_once(long fd, char *buf, long bufsz, const char *prefix) {
    for (;;) {
        long n = sys_getdents64((int)fd, buf, (unsigned long)bufsz);
        if (n <= 0) {
            break;
        }
        long pos = 0;
        while (pos < n) {
            unsigned char *rec = (unsigned char *)buf + pos;
            unsigned short reclen = (unsigned short)(rec[16] | (rec[17] << 8));
            char *name = (char *)rec + 19;
            unsigned long len = 0;
            while (name[len] != 0) {
                ++len;
            }
            write_str(prefix);
            sys_write(1, name, len);
            sys_write(1, "\n", 1);
            pos += reclen;
        }
    }
}

int main(void) {
    long fd = sys_open("/", 0x10000 /* O_DIRECTORY */, 0);
    if (fd < 0) {
        sys_write(2, "rootmerge: open failed\n", 24);
        return 1;
    }

    char buf[64];
    list_once(fd, buf, (long)sizeof buf, "N1:");

    long rc = sys_lseek((int)fd, 0, 0 /* SEEK_SET */);
    if (rc < 0) {
        sys_write(2, "rootmerge: rewind failed\n", 26);
        sys_close((int)fd);
        return 1;
    }
    list_once(fd, buf, (long)sizeof buf, "N2:");

    long bad1 = sys_lseek((int)fd, 1, 0 /* SEEK_SET, nonzero offset */);
    long bad2 = sys_lseek((int)fd, 0, 1 /* SEEK_CUR, zero offset */);
    int lseekbad_ok = (bad1 == -22 && bad2 == -22);   /* -HOSTFS_EINVAL */
    write_str(lseekbad_ok ? "lseekbad: PASS\n" : "lseekbad: FAIL\n");

    sys_close((int)fd);
    return 0;
}
