/* demos/doom/tools/make_min_iwad.c (maize-154 DOOM Phase C)
 *
 * Generate a minimal, LICENSE-CLEAN synthetic DOOM IWAD that boots the vendored
 * doomgeneric engine (pin dcb7a8d) far enough to render a one-room E1M1 level via
 * `-warp 1 1`. Every byte here is hand-authored or computed in this program; NOT
 * ONE byte is copied from any real DOOM WAD. The committed artifact is THIS
 * SOURCE; the render gate compiles it with the system cc and runs it at test
 * time into a scratch dir (Decision D7: the auditable artifact is the generator,
 * not a binary). Written in portable C (not Python) so the gate runs on both CI
 * hosts, including the Windows MSYS2 lane that ships gcc but no python3.
 *
 * The lump set is traced against the vendored engine's unconditional boot path
 * (see demos/doom/README.md and the maize-154 card). Several requirements were
 * confirmed only by RUNNING DOOM against the WAD (the OQ2 runtime-gap protocol),
 * as a link alone cannot surface them:
 *   - g_game.c:613 sets skyflatnum = R_FlatNumForName("F_SKY1") UNCONDITIONALLY,
 *     so an F_SKY1 flat is REQUIRED (the room's floor/ceiling use OTHER flats, so
 *     neither surface renders as sky).
 *   - p_switch.c P_InitSwitchList calls R_TextureNumForName unconditionally on
 *     every shareware (episode 1) switch-texture pair, so all 19 pairs must be
 *     defined as textures.
 *   - st_stuff.c ST_loadData needs the full status-bar set incl. STTMINUS and
 *     STKEYS0..5 (NUMCARDS=6: 3 cards + 3 skulls).
 *   - s_sound.c S_ChangeMusic does W_GetNumForName("d_e1m1") at level load, so a
 *     (dummy) D_E1M1 music lump must exist.
 *   - R_DrawPlayerSprites draws the ready pistol weapon (SPR_PISG), so a PISGA0
 *     sprite frame must exist (the drawn frame is RANGECHECK'd against numframes).
 *
 * V_DrawPatch / V_CopyRect RANGECHECK against the 320x200 screen and the ST
 * number widgets compute erase geometry from the digit-patch width, so the ONE
 * shared 8x8 patch (WALL/SKY1/switch texture patch AND every font / status-bar
 * lump) keeps every ST blit in bounds. Payload reuse (Decision D2): one directory
 * entry is (filepos,size,name) and many entries may share one filepos, so the
 * whole IWAD is a few KB.
 *
 * Usage: make_min_iwad <out.wad>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Palette indices for the three scene surfaces, given distinct RGB in palette 0
 * so they reach the framebuffer as distinct XRGB through i_video.c's monotonic
 * gammatable (Decision D3/D4; the render gate asserts >= 2 distinct colors in the
 * 3D viewport per OQ3). */
enum { CW = 200, CF = 100, CC = 50 };   /* wall red, floor green, ceiling blue */

/* The one shared patch is 8x8: small enough that every status-bar blit (number
 * widgets erase `x = n->x - numdigits*width` from x=44, and widgets reach y=191)
 * stays inside the 320x200 RANGECHECK, and the texture dims match so
 * R_GenerateLookup finds every column covered. */
enum { PATCH_W = 8, PATCH_H = 8 };

/* One convex square room: half-size 384 (768x768), ceiling 128 high, player at
 * the center. Vertices wound clockwise so each one-sided linedef's front (right)
 * side faces the interior; floor/ceiling use distinct non-sky flats so the
 * viewport shows three color bands (ceiling / wall / floor). */
enum { HALF = 384, CEIL_H = 128 };

/* ---- growable byte buffer, all writes little-endian regardless of host ------ */
typedef struct {
    unsigned char *b;
    size_t len;
    size_t cap;
} Buf;

static void buf_need(Buf *buf, size_t extra)
{
    if (buf->len + extra > buf->cap) {
        size_t nc = buf->cap ? buf->cap * 2 : 256;
        while (nc < buf->len + extra) {
            nc *= 2;
        }
        buf->b = (unsigned char *)realloc(buf->b, nc);
        if (!buf->b) {
            fprintf(stderr, "make_min_iwad: out of memory\n");
            exit(2);
        }
        buf->cap = nc;
    }
}

static void put8(Buf *buf, unsigned v)
{
    buf_need(buf, 1);
    buf->b[buf->len++] = (unsigned char)(v & 0xFF);
}

static void put16(Buf *buf, int v)
{
    unsigned u = (unsigned)(v & 0xFFFF);
    put8(buf, u & 0xFF);
    put8(buf, (u >> 8) & 0xFF);
}

static void put32(Buf *buf, long v)
{
    unsigned long u = (unsigned long)v & 0xFFFFFFFFUL;
    put8(buf, (unsigned)(u & 0xFF));
    put8(buf, (unsigned)((u >> 8) & 0xFF));
    put8(buf, (unsigned)((u >> 16) & 0xFF));
    put8(buf, (unsigned)((u >> 24) & 0xFF));
}

static void putbytes(Buf *buf, const unsigned char *p, size_t n)
{
    buf_need(buf, n);
    memcpy(buf->b + buf->len, p, n);
    buf->len += n;
}

/* Write an 8-byte, NUL-padded, uppercased lump name. */
static void putname8(Buf *buf, const char *s)
{
    int i;
    for (i = 0; i < 8; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0) {
            break;
        }
        if (c >= 'a' && c <= 'z') {
            c = (unsigned char)(c - 'a' + 'A');
        }
        put8(buf, c);
    }
    for (; i < 8; i++) {
        put8(buf, 0);
    }
}

/* ---- payload builders (byte-for-byte mirror of make_min_iwad.py) ------------ */

static void build_playpal(Buf *out)
{
    unsigned char pal[256 * 3];
    int i, p;
    for (i = 0; i < 256; i++) {
        pal[i * 3 + 0] = (unsigned char)((i * 7) & 0xFF);
        pal[i * 3 + 1] = (unsigned char)((i * 5) & 0xFF);
        pal[i * 3 + 2] = (unsigned char)((i * 3) & 0xFF);
    }
    pal[0] = 0;   pal[1] = 0;   pal[2] = 0;      /* index 0 black */
    pal[CW * 3] = 255; pal[CW * 3 + 1] = 0;   pal[CW * 3 + 2] = 0;    /* wall red */
    pal[CF * 3] = 0;   pal[CF * 3 + 1] = 208; pal[CF * 3 + 2] = 0;    /* floor green */
    pal[CC * 3] = 32;  pal[CC * 3 + 1] = 64;  pal[CC * 3 + 2] = 255;  /* ceiling blue */
    for (p = 0; p < 14; p++) {   /* 14 palettes so ST tints never index OOB */
        putbytes(out, pal, sizeof(pal));
    }
}

static void build_colormap(Buf *out)
{
    int m, c;
    for (m = 0; m < 34; m++) {          /* 34 identity light maps */
        for (c = 0; c < 256; c++) {
            put8(out, c);
        }
    }
}

/* A valid column-based patch_t: a solid `color` block, w x h. Matches the
 * v_video.c reader (source = column+3, advance by length+4). */
static void build_patch(Buf *out, int w, int h, int color)
{
    int x, k;
    int colofs_table = 8 + 4 * w;
    int colsize = h + 5;
    put16(out, w);
    put16(out, h);
    put16(out, 0);          /* leftoffset */
    put16(out, 0);          /* topoffset */
    for (x = 0; x < w; x++) {
        put32(out, colofs_table + x * colsize);
    }
    for (x = 0; x < w; x++) {
        put8(out, 0);       /* topdelta */
        put8(out, h);       /* length */
        put8(out, color);   /* leading pad (unused by reader) */
        for (k = 0; k < h; k++) {
            put8(out, color);
        }
        put8(out, color);   /* trailing pad (unused by reader) */
        put8(out, 0xFF);    /* column terminator */
    }
}

static void build_flat(Buf *out, int color)
{
    int i;
    for (i = 0; i < 64 * 64; i++) {
        put8(out, color);
    }
}

static void build_pnames(Buf *out)
{
    put32(out, 1);
    putname8(out, "WALLPAT");
}

/* Shareware (episode 1) switch texture pairs consulted by P_InitSwitchList. */
static const char *SWITCH_NAMES[] = {
    "SW1BRCOM", "SW2BRCOM", "SW1BRN1", "SW2BRN1", "SW1BRN2", "SW2BRN2",
    "SW1BRNGN", "SW2BRNGN", "SW1BROWN", "SW2BROWN", "SW1COMM", "SW2COMM",
    "SW1COMP", "SW2COMP", "SW1DIRT", "SW2DIRT", "SW1EXIT", "SW2EXIT",
    "SW1GRAY", "SW2GRAY", "SW1GRAY1", "SW2GRAY1", "SW1METAL", "SW2METAL",
    "SW1PIPE", "SW2PIPE", "SW1SLAD", "SW2SLAD", "SW1STARG", "SW2STARG",
    "SW1STON1", "SW2STON1", "SW1STON2", "SW2STON2", "SW1STONE", "SW2STONE",
    "SW1STRTN", "SW2STRTN"
};
#define NUM_SWITCH_NAMES ((int)(sizeof(SWITCH_NAMES) / sizeof(SWITCH_NAMES[0])))

/* TEXTURE1: WALL, SKY1, and the shareware switch textures, all patch index 0. */
static void build_texture1(Buf *out)
{
    const char *names[2 + 64];
    int n = 0;
    int i, off;
    int body_len;

    names[n++] = "WALL";
    names[n++] = "SKY1";
    for (i = 0; i < NUM_SWITCH_NAMES; i++) {
        names[n++] = SWITCH_NAMES[i];
    }

    /* Each maptexture_t body is fixed-size: name[8] + int masked + short w +
     * short h + int obsolete + short patchcount + one 10-byte mappatch = 32. */
    body_len = 8 + 4 + 2 + 2 + 4 + 2 + 10;
    put32(out, n);
    off = 4 + 4 * n;
    for (i = 0; i < n; i++) {
        put32(out, off);
        off += body_len;
    }
    for (i = 0; i < n; i++) {
        putname8(out, names[i]);
        put32(out, 0);            /* masked */
        put16(out, PATCH_W);
        put16(out, PATCH_H);
        put32(out, 0);            /* obsolete columndirectory */
        put16(out, 1);            /* patchcount */
        put16(out, 0);            /* mappatch originx */
        put16(out, 0);            /* originy */
        put16(out, 0);            /* patch -> pnames idx 0 */
        put16(out, 1);            /* stepdir */
        put16(out, 0);            /* colormap */
    }
}

static void build_vertexes(Buf *out)
{
    put16(out, -HALF); put16(out, -HALF);
    put16(out,  HALF); put16(out, -HALF);
    put16(out,  HALF); put16(out,  HALF);
    put16(out, -HALF); put16(out,  HALF);
}

static void build_linedefs(Buf *out)
{
    /* v1, v2, flags(ML_BLOCKING=1), special, tag, sidenum0, sidenum1(-1) */
    static const int L[4][7] = {
        { 0, 3, 1, 0, 0, 0, -1 },
        { 3, 2, 1, 0, 0, 1, -1 },
        { 2, 1, 1, 0, 0, 2, -1 },
        { 1, 0, 1, 0, 0, 3, -1 },
    };
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 7; j++) {
            put16(out, L[i][j]);
        }
    }
}

static void build_sidedefs(Buf *out)
{
    int i;
    for (i = 0; i < 4; i++) {
        put16(out, 0);            /* textureoffset */
        put16(out, 0);            /* rowoffset */
        putname8(out, "-");       /* toptexture */
        putname8(out, "-");       /* bottomtexture */
        putname8(out, "WALL");    /* midtexture */
        put16(out, 0);            /* sector */
    }
}

static void build_segs(Buf *out)
{
    /* v1, v2, angle(BAM>>16), linedef, side, offset */
    static const int S[4][6] = {
        { 0, 3,  16384, 0, 0, 0 },   /* north */
        { 3, 2,      0, 1, 0, 0 },   /* east */
        { 2, 1, -16384, 2, 0, 0 },   /* south */
        { 1, 0, -32768, 3, 0, 0 },   /* west */
    };
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 6; j++) {
            put16(out, S[i][j]);
        }
    }
}

static void build_ssectors(Buf *out)
{
    put16(out, 4);   /* numsegs */
    put16(out, 0);   /* firstseg */
}

static void build_sectors(Buf *out)
{
    put16(out, 0);            /* floorheight */
    put16(out, CEIL_H);       /* ceilingheight */
    putname8(out, "FLOOR");
    putname8(out, "CEIL");
    put16(out, 255);          /* lightlevel */
    put16(out, 0);            /* special */
    put16(out, 0);            /* tag */
}

static void build_reject(Buf *out)
{
    put8(out, 0);    /* numsectors^2 = 1 bit -> 1 byte, nothing rejected */
}

static void build_blockmap(Buf *out)
{
    /* origin x,y; columns,rows; one offset (5); leading 0, four linedefs, 0xFFFF */
    static const int W[11] = { -HALF, -HALF, 1, 1, 5, 0, 0, 1, 2, 3, -1 };
    int i;
    for (i = 0; i < 11; i++) {
        put16(out, W[i]);
    }
}

static void build_things(Buf *out)
{
    /* one type-1 player-1 start at center, angle 0 (east), all skills */
    put16(out, 0); put16(out, 0); put16(out, 0); put16(out, 1); put16(out, 7);
}

/* ---- WAD assembly ----------------------------------------------------------- */

/* payload keys (index into the payloads[] table); -1 == 0-length marker lump. */
enum {
    K_NONE = -1,
    K_PLAYPAL = 0, K_COLORMAP, K_PNAMES, K_TEXTURE1, K_MUSIC,
    K_FLATFLOOR, K_FLATCEIL, K_PATCH,
    K_THINGS, K_LINEDEFS, K_SIDEDEFS, K_VERTEXES, K_SEGS, K_SSECTORS,
    K_SECTORS, K_REJECT, K_BLOCKMAP,
    K_COUNT
};

typedef struct {
    char name[16];
    int key;
} Lump;

static void st_status_bar_names(Lump *lumps, int *pn)
{
    int n = *pn;
    int i, j;
    char nm[16];

    for (i = 0; i < 10; i++) {
        snprintf(nm, sizeof(nm), "STTNUM%d", i);
        strcpy(lumps[n].name, nm); lumps[n++].key = K_PATCH;
        snprintf(nm, sizeof(nm), "STYSNUM%d", i);
        strcpy(lumps[n].name, nm); lumps[n++].key = K_PATCH;
    }
    strcpy(lumps[n].name, "STTPRCNT"); lumps[n++].key = K_PATCH;
    strcpy(lumps[n].name, "STTMINUS"); lumps[n++].key = K_PATCH;
    for (i = 0; i < 6; i++) {           /* NUMCARDS (3 cards + 3 skulls) */
        snprintf(nm, sizeof(nm), "STKEYS%d", i);
        strcpy(lumps[n].name, nm); lumps[n++].key = K_PATCH;
    }
    strcpy(lumps[n].name, "STARMS"); lumps[n++].key = K_PATCH;
    for (i = 0; i < 6; i++) {           /* STGNUM2..7 */
        snprintf(nm, sizeof(nm), "STGNUM%d", i + 2);
        strcpy(lumps[n].name, nm); lumps[n++].key = K_PATCH;
    }
    strcpy(lumps[n].name, "STFB0"); lumps[n++].key = K_PATCH;
    strcpy(lumps[n].name, "STBAR"); lumps[n++].key = K_PATCH;
    for (i = 0; i < 5; i++) {           /* ST_NUMPAINFACES */
        for (j = 0; j < 3; j++) {       /* ST_NUMSTRAIGHTFACES */
            snprintf(nm, sizeof(nm), "STFST%d%d", i, j);
            strcpy(lumps[n].name, nm); lumps[n++].key = K_PATCH;
        }
        snprintf(nm, sizeof(nm), "STFTR%d0", i);
        strcpy(lumps[n].name, nm); lumps[n++].key = K_PATCH;
        snprintf(nm, sizeof(nm), "STFTL%d0", i);
        strcpy(lumps[n].name, nm); lumps[n++].key = K_PATCH;
        snprintf(nm, sizeof(nm), "STFOUCH%d", i);
        strcpy(lumps[n].name, nm); lumps[n++].key = K_PATCH;
        snprintf(nm, sizeof(nm), "STFEVL%d", i);
        strcpy(lumps[n].name, nm); lumps[n++].key = K_PATCH;
        snprintf(nm, sizeof(nm), "STFKILL%d", i);
        strcpy(lumps[n].name, nm); lumps[n++].key = K_PATCH;
    }
    strcpy(lumps[n].name, "STFGOD0"); lumps[n++].key = K_PATCH;
    strcpy(lumps[n].name, "STFDEAD0"); lumps[n++].key = K_PATCH;

    *pn = n;
}

static int build_wad(Buf *wad)
{
    Buf payloads[K_COUNT];
    Lump lumps[320];
    long placed_pos[K_COUNT];
    long placed_size[K_COUNT];
    int placed[K_COUNT];
    Buf data;
    Buf dir;
    int n = 0;
    int i, j;
    long header_size = 12;
    long infotableofs;

    memset(payloads, 0, sizeof(payloads));
    build_playpal(&payloads[K_PLAYPAL]);
    build_colormap(&payloads[K_COLORMAP]);
    build_pnames(&payloads[K_PNAMES]);
    build_texture1(&payloads[K_TEXTURE1]);
    { static const unsigned char z[16] = {0};
      putbytes(&payloads[K_MUSIC], z, sizeof(z)); }   /* dummy level music */
    build_flat(&payloads[K_FLATFLOOR], CF);
    build_flat(&payloads[K_FLATCEIL], CC);
    build_patch(&payloads[K_PATCH], PATCH_W, PATCH_H, CW);
    build_things(&payloads[K_THINGS]);
    build_linedefs(&payloads[K_LINEDEFS]);
    build_sidedefs(&payloads[K_SIDEDEFS]);
    build_vertexes(&payloads[K_VERTEXES]);
    build_segs(&payloads[K_SEGS]);
    build_ssectors(&payloads[K_SSECTORS]);
    build_sectors(&payloads[K_SECTORS]);
    build_reject(&payloads[K_REJECT]);
    build_blockmap(&payloads[K_BLOCKMAP]);

    /* Directory order (must match the boot path: E1M1 then the 10 ML_-ordered
     * map sublumps). Marker lumps carry key K_NONE. */
    strcpy(lumps[n].name, "PLAYPAL");  lumps[n++].key = K_PLAYPAL;
    strcpy(lumps[n].name, "COLORMAP"); lumps[n++].key = K_COLORMAP;
    strcpy(lumps[n].name, "PNAMES");   lumps[n++].key = K_PNAMES;
    strcpy(lumps[n].name, "TEXTURE1"); lumps[n++].key = K_TEXTURE1;
    strcpy(lumps[n].name, "D_E1M1");   lumps[n++].key = K_MUSIC;

    strcpy(lumps[n].name, "F_START");  lumps[n++].key = K_NONE;
    strcpy(lumps[n].name, "F_SKY1");   lumps[n++].key = K_FLATFLOOR;
    strcpy(lumps[n].name, "FLOOR");    lumps[n++].key = K_FLATFLOOR;
    strcpy(lumps[n].name, "CEIL");     lumps[n++].key = K_FLATCEIL;
    strcpy(lumps[n].name, "F_END");    lumps[n++].key = K_NONE;

    strcpy(lumps[n].name, "S_START");  lumps[n++].key = K_NONE;
    strcpy(lumps[n].name, "PISGA0");   lumps[n++].key = K_PATCH;
    strcpy(lumps[n].name, "PLAYA0");   lumps[n++].key = K_PATCH;
    strcpy(lumps[n].name, "S_END");    lumps[n++].key = K_NONE;

    strcpy(lumps[n].name, "WALLPAT");  lumps[n++].key = K_PATCH;
    for (j = '!'; j <= '_'; j++) {     /* STCFN033..STCFN095 */
        snprintf(lumps[n].name, sizeof(lumps[n].name), "STCFN%.3d", j);
        lumps[n++].key = K_PATCH;
    }
    st_status_bar_names(lumps, &n);

    strcpy(lumps[n].name, "E1M1");     lumps[n++].key = K_NONE;
    strcpy(lumps[n].name, "THINGS");   lumps[n++].key = K_THINGS;
    strcpy(lumps[n].name, "LINEDEFS"); lumps[n++].key = K_LINEDEFS;
    strcpy(lumps[n].name, "SIDEDEFS"); lumps[n++].key = K_SIDEDEFS;
    strcpy(lumps[n].name, "VERTEXES"); lumps[n++].key = K_VERTEXES;
    strcpy(lumps[n].name, "SEGS");     lumps[n++].key = K_SEGS;
    strcpy(lumps[n].name, "SSECTORS"); lumps[n++].key = K_SSECTORS;
    strcpy(lumps[n].name, "NODES");    lumps[n++].key = K_NONE;   /* 0-length */
    strcpy(lumps[n].name, "SECTORS");  lumps[n++].key = K_SECTORS;
    strcpy(lumps[n].name, "REJECT");   lumps[n++].key = K_REJECT;
    strcpy(lumps[n].name, "BLOCKMAP"); lumps[n++].key = K_BLOCKMAP;

    /* Place each unique payload once (first-reference order); build the data. */
    for (i = 0; i < K_COUNT; i++) {
        placed[i] = 0;
    }
    memset(&data, 0, sizeof(data));
    for (i = 0; i < n; i++) {
        int k = lumps[i].key;
        if (k == K_NONE || placed[k]) {
            continue;
        }
        placed_pos[k] = header_size + (long)data.len;
        placed_size[k] = (long)payloads[k].len;
        putbytes(&data, payloads[k].b, payloads[k].len);
        placed[k] = 1;
    }
    infotableofs = header_size + (long)data.len;

    memset(&dir, 0, sizeof(dir));
    for (i = 0; i < n; i++) {
        int k = lumps[i].key;
        long fp = (k == K_NONE) ? 0 : placed_pos[k];
        long sz = (k == K_NONE) ? 0 : placed_size[k];
        put32(&dir, fp);
        put32(&dir, sz);
        putname8(&dir, lumps[i].name);
    }

    /* header: "IWAD", numlumps, infotableofs */
    putbytes(wad, (const unsigned char *)"IWAD", 4);
    put32(wad, n);
    put32(wad, infotableofs);
    putbytes(wad, data.b, data.len);
    putbytes(wad, dir.b, dir.len);

    for (i = 0; i < K_COUNT; i++) {
        free(payloads[i].b);
    }
    free(data.b);
    free(dir.b);
    return n;
}

int main(int argc, char **argv)
{
    Buf wad;
    int nlumps;
    FILE *f;

    if (argc != 2) {
        fprintf(stderr, "usage: make_min_iwad <out.wad>\n");
        return 2;
    }
    memset(&wad, 0, sizeof(wad));
    nlumps = build_wad(&wad);

    f = fopen(argv[1], "wb");
    if (!f) {
        fprintf(stderr, "make_min_iwad: cannot open %s for writing\n", argv[1]);
        return 2;
    }
    if (fwrite(wad.b, 1, wad.len, f) != wad.len) {
        fprintf(stderr, "make_min_iwad: short write to %s\n", argv[1]);
        fclose(f);
        return 2;
    }
    fclose(f);
    fprintf(stderr, "make_min_iwad: wrote %s (%lu bytes, %d lumps)\n",
            argv[1], (unsigned long)wad.len, nlumps);
    free(wad.b);
    return 0;
}
