/* maize-114 hostfs acceptance: list a mount directory (doc section 8).
 *
 * Opens /ro (a :ro mount) with O_DIRECTORY, enumerates it with getdents64, and
 * prints each entry name on its own line. The harness sorts stdout before comparing
 * (getdents order is host-defined), so this fixture prints names in whatever order
 * the backend pages them. Parses the section-2 linux_dirent64 layout directly:
 * d_reclen at offset 16 (u16, little-endian), d_name at offset 19 (NUL-terminated).
 * Byte indexing only (element size 1), so the address math never multiplies. */
#include "syscall.h"

int main(void) {
    long fd = sys_open("/ro", 0x10000 /* O_DIRECTORY */, 0);
    if (fd < 0) {
        sys_write(2, "ls: open failed\n", 16);
        return 1;
    }
    char buf[1024];
    for (;;) {
        long n = sys_getdents64((int)fd, buf, sizeof buf);
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
            sys_write(1, name, len);
            sys_write(1, "\n", 1);
            pos += reclen;
        }
    }
    sys_close((int)fd);
    return 0;
}
