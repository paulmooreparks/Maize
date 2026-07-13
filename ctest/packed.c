/* maize-149: prove cc-maize.sh's cpp-step GNU-attribute strip lets a trailing
 * __attribute__((packed)) struct compile through the real pipeline, and that the
 * resulting layout matches the packed on-disk WAD layout DOOM depends on.
 *
 * doomtype.h defines PACKEDATTR = __attribute__((packed)) under __GNUC__ and uses
 * it in the TRAILING declarator position (} PACKEDATTR name;). The pinned cproc
 * honors packed only in the leading position, so before maize-149 this raised
 * "GNU attribute 'packed' is not supported here". The driver now predefines
 * __attribute__(x) to nothing in the cpp step, neutralizing the attribute before
 * cproc. This fixture mirrors DOOM's mapsidedef_t shape (the representative WAD
 * struct: shorts interleaved with char[8] blocks) using that exact trailing form.
 *
 * The run-time asserts encode the layout-equivalence invariant that makes the
 * strip run-safe: because every member is 2-aligned-or-narrower and every char[8]
 * block is even-length, the natural (unpacked) layout is byte-identical to the
 * packed layout, so sizeof==30 and the trailing short lands at offset 28 with no
 * padding. A wrong layout (interior padding) would flip these and corrupt the
 * verdict line. See ctest/packed.expected for the exact bytes. */
#include "stddef.h"
#include "stdio.h"

/* The freestanding RT stddef.h intentionally omits offsetof (maize-76), and this
 * card adds no RT header, so define the classic form locally. */
#ifndef offsetof
#define offsetof(type, member) ((size_t)&(((type *)0)->member))
#endif

/* Trailing-position packed, exactly as doomtype.h's PACKEDATTR expands. */
typedef struct {
    short textureoffset;
    short rowoffset;
    char  toptexture[8];
    char  bottomtexture[8];
    char  midtexture[8];
    short sector;
} __attribute__((packed)) mapsidedef_t;

/* A second form with the multi-attribute list, to prove the one-arg macro also
 * neutralizes comma-separated attributes (inner commas paren-protected). */
typedef struct {
    short x;
    short y;
} __attribute__((packed, aligned(2))) mapvertex_t;

static int ok = 1;

static void check(int cond) { if (!cond) ok = 0; }

int
main(void)
{
    mapsidedef_t sd;

    /* sizeof proves no padding was inserted (packed == natural for this shape). */
    check(sizeof(mapsidedef_t) == 30);

    /* Per-member offsets: the char[8] blocks at 4/12/20, the trailing short at 28. */
    check(offsetof(mapsidedef_t, textureoffset) == 0);
    check(offsetof(mapsidedef_t, rowoffset)     == 2);
    check(offsetof(mapsidedef_t, toptexture)    == 4);
    check(offsetof(mapsidedef_t, bottomtexture) == 12);
    check(offsetof(mapsidedef_t, midtexture)    == 20);
    check(offsetof(mapsidedef_t, sector)        == 28);

    /* The multi-attribute vertex form compiles and is the trivially-packed 2 shorts. */
    check(sizeof(mapvertex_t) == 4);
    check(offsetof(mapvertex_t, y) == 2);

    /* A field write/read round-trip through the (stripped) struct, proving the
     * member offsets are usable at run time, not just at sizeof time. */
    sd.textureoffset = 111;
    sd.sector        = 222;
    sd.midtexture[0] = 'M';
    check(sd.textureoffset == 111);
    check(sd.sector == 222);
    check(sd.midtexture[0] == 'M');

    printf("packed: %s\n", ok ? "PASS" : "FAIL");
    return 0;
}
