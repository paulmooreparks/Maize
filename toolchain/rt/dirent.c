/* toolchain/rt/dirent.c -- opendir/readdir/closedir over sys_getdents64 (maize-120).
 *
 * The frozen linux_dirent64 layout (docs/design/hostfs.md section 2) is decoded with
 * byte indexing only, exactly as the ls_hostfs.c reference does: no variable multiply
 * ever enters the address path, keeping every function inside the pinned qbe-maize
 * backend's register/spill budget (see the extensive rationale in stdio.c). The u64
 * inode decode is factored into its own single-loop helper so readdir carries just its
 * one name-copy loop (the backend miscompiles two sequential pointer-indexing loops in
 * one function; one loop per function sidesteps that gap).
 */
#include "dirent.h"
#include "fcntl.h"    /* O_DIRECTORY */
#include "syscall.h"  /* open/close, sys_getdents64 */
#include "stddef.h"   /* NULL */
#include "stdlib.h"   /* malloc/free */

/* Decode a little-endian u64 from an 8-byte record field. Its own function so it is
 * the sole loop in the translation unit that readdir shares (one loop per function). */
static unsigned long
dec_u64(const unsigned char *p)
{
    unsigned long v = 0;
    int i;
    for (i = 7; i >= 0; i--)
        v = (v << 8) | (unsigned long)p[i];
    return v;
}

DIR *
opendir(const char *path)
{
    int fd = open(path, O_DIRECTORY, 0);
    DIR *d;

    if (fd < 0)
        return NULL;
    d = malloc(sizeof(DIR));
    if (d == NULL) {
        close(fd);
        return NULL;
    }
    d->fd = fd;
    d->bpos = 0;
    d->blen = 0;
    return d;
}

struct dirent *
readdir(DIR *d)
{
    unsigned char *rec;
    unsigned short reclen;
    long k;

    if (d->bpos >= d->blen) {                   /* buffer drained: refill */
        long n = sys_getdents64(d->fd, d->buf, DIRENT_BUF);
        if (n <= 0)                             /* 0 = EOF, <0 = error */
            return NULL;
        d->blen = n;
        d->bpos = 0;
    }

    rec = d->buf + d->bpos;
    reclen = (unsigned short)(rec[16] | (rec[17] << 8));
    d->de.d_ino = dec_u64(rec);                 /* d_ino @0 */
    d->de.d_type = rec[18];                     /* d_type @18 */

    /* Copy the NUL-terminated name (@19) with a single byte loop; cap at 255 so the
     * trailing NUL always fits d_name[256]. Byte indexing only, no multiply. */
    k = 0;
    while (k < 255 && rec[19 + k] != 0) {
        d->de.d_name[k] = (char)rec[19 + k];
        k++;
    }
    d->de.d_name[k] = '\0';

    d->bpos += reclen;
    return &d->de;
}

int
closedir(DIR *d)
{
    int fd = d->fd;
    free(d);
    return close(fd);
}
