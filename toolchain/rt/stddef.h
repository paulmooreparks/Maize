/* toolchain/rt/stddef.h -- minimal freestanding <stddef.h> for the Maize C
 * runtime (maize-76).
 *
 * cc-maize.sh preprocesses with `cpp -nostdinc -I toolchain/rt`, so no system
 * <stddef.h> is visible. The libc slice needs size_t (allocator / string / stdio
 * lengths) and NULL; ptrdiff_t rounds out the freestanding trio. Fixed-width
 * types are intentionally not provided here: the allocator uses `unsigned long`,
 * so a stdint.h is deferred until a module actually needs one.
 */
#ifndef MAIZE_STDDEF_H
#define MAIZE_STDDEF_H

typedef unsigned long size_t;
typedef long          ptrdiff_t;

#define NULL ((void *)0)

#endif /* MAIZE_STDDEF_H */
