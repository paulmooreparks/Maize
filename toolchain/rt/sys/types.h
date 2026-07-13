/* toolchain/rt/sys/types.h -- freestanding <sys/types.h> alias slice for the Maize
 * C runtime (maize-147).
 *
 * cpp -nostdinc -I toolchain/rt resolves #include <sys/types.h> to this file (cpp
 * joins each -I dir with the bracketed subpath), so the sys/ subdir lives under
 * toolchain/rt. DOOM's i_video.c / m_misc.c reference off_t/ssize_t; mode_t backs
 * st_mode and mkdir's perm arg. LP64 widths (long = 64, int = 32), consistent with
 * stddef.h / stdint.h (maize-146). No existing RT header defines these (verified
 * across toolchain/rt). The alias set is kept minimal: struct stat (sys/stat.h) uses
 * concrete widths rather than the dev_t/ino_t soup.
 */
#ifndef MAIZE_SYS_TYPES_H
#define MAIZE_SYS_TYPES_H

typedef long          off_t;    /* 64-bit file offset (LP64) */
typedef long          ssize_t;  /* signed size */
typedef unsigned int  mode_t;   /* st_mode / mkdir perm bits */

#endif /* MAIZE_SYS_TYPES_H */
