/* maize-114 hostfs acceptance: byte-ABI struct stat conformance (doc sections 2, 5).
 *
 * fstat a known file on a :ro mount and read the 144-byte section-2 struct stat image
 * back at the exact offsets: st_nlink at 16 (the x86-64 quirk: it precedes st_mode),
 * st_mode at 24 (u32), st_size at 48 (s64), all little-endian. The harness writes a
 * fixed 11-byte payload, so st_size must read back 11, st_mode must carry S_IFREG
 * (0x8000), and st_nlink must be nonzero. Only constant-shift and byte-index math is
 * used (no variable multiply in the address path). Prints a single PASS/FAIL line. */
#include "syscall.h"

int main(void) {
    long fd = sys_open("/ro/payload.txt", 0 /* O_RDONLY */, 0);
    if (fd < 0) {
        sys_write(2, "stat: open failed\n", 18);
        return 1;
    }
    unsigned char st[144];
    long r = sys_fstat((int)fd, st);
    if (r < 0) {
        sys_write(2, "stat: fstat failed\n", 19);
        sys_close((int)fd);
        return 1;
    }

    unsigned long mode = (unsigned long)st[24]
        | ((unsigned long)st[25] << 8)
        | ((unsigned long)st[26] << 16)
        | ((unsigned long)st[27] << 24);

    unsigned long size = 0;
    for (int i = 55; i >= 48; --i) {
        size = (size << 8) | (unsigned long)st[i];
    }
    unsigned long nlink = 0;
    for (int i = 23; i >= 16; --i) {
        nlink = (nlink << 8) | (unsigned long)st[i];
    }

    int ok = 1;
    if ((mode & 0xF000UL) != 0x8000UL) {   /* S_IFREG */
        ok = 0;
    }
    if (size != 11UL) {
        ok = 0;
    }
    if (nlink == 0UL) {
        ok = 0;
    }

    sys_write(1, ok ? "stat: PASS\n" : "stat: FAIL\n", 11);
    sys_close((int)fd);
    return 0;
}
