/* maize-255 hostfs acceptance: fstat on a granted mount point that has no physical
 * counterpart under the root sandbox (doc sections 2, 5; AC 9288).
 *
 * open("/bin", O_DIRECTORY) hits match_mount directly (its own registered mount,
 * a longer prefix than the root "/"), so fstat on that fd already reports the real
 * backend directory's stat via the existing hostfs_open/hostfs_fstat path (D6):
 * this is pre-existing, unrelated behavior, confirmed rather than newly built.
 * Mirrors stat_hostfs.c's byte-offset decode, but asserts S_IFDIR (a directory)
 * rather than S_IFREG (a file). Prints a single PASS/FAIL line. */
#include "syscall.h"

int main(void) {
    long fd = sys_open("/bin", 0x10000 /* O_DIRECTORY */, 0);
    if (fd < 0) {
        sys_write(2, "statmount: open failed\n", 23);
        return 1;
    }
    unsigned char st[144];
    long r = sys_fstat((int)fd, st);
    if (r < 0) {
        sys_write(2, "statmount: fstat failed\n", 24);
        sys_close((int)fd);
        return 1;
    }

    unsigned long mode = (unsigned long)st[24]
        | ((unsigned long)st[25] << 8)
        | ((unsigned long)st[26] << 16)
        | ((unsigned long)st[27] << 24);

    int ok = 1;
    if ((mode & 0xF000UL) != 0x4000UL) {   /* S_IFDIR */
        ok = 0;
    }

    sys_write(1, ok ? "statmount: PASS\n" : "statmount: FAIL\n", 16);
    sys_close((int)fd);
    return 0;
}
