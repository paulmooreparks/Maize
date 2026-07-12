/* demos/terminal/term_core.h -- self-hosted framebuffer terminal core (maize-121).
 *
 * The reusable terminal engine: an 8x8-font 40x25 character grid blitted into the
 * memory-backed framebuffer's guest-RAM pixel buffer, a byte-stream core (term_putc /
 * term_write) with a minimal ANSI/VT subset, and a shift-aware Set-1 scancode translator
 * (term_key). It is a header of `static` functions + state so the two entry points
 * (terminal.c interactive main, terminal_selfcheck.c CI fixture) each compile it into a
 * single translation unit; the pinned cproc/qbe C pipeline links one TU per image.
 *
 * Pixel model (frozen device contract, Ch.11 / src/devices.cpp): pixels are ordinary
 * stores into a guest-RAM buffer at FB_BASE, whose base is registered once with
 * fb_set_base; the device copies the buffer on fb_present (present-on-change). Every
 * pixel address is computed from a runtime base pointer plus runtime indices, because the
 * pinned qbe -t maize backend cannot emit a store to a folded constant (symbol/absolute)
 * address; a runtime-loaded pointer keeps every store register-indirect (ST @Rn).
 *
 * ANSI subset: CUU/CUD/CUF/CUB, CUP (H and f), ED (J, n=0/1/2), EL (K, n=0/1/2), SGR (m,
 * reset 0 + fg 30-37 + bg 40-47 over the 8 basic colors). Malformed / incomplete escapes
 * are consumed defensively (never hang, never read out of bounds). Bottom-of-screen
 * advance scrolls up one line (no scrollback). No malloc; memset/memcpy are not required.
 */
#ifndef MAIZE_TERM_CORE_H
#define MAIZE_TERM_CORE_H

#include "mzdev.h"
#include "font8x8.h"

#define TERM_COLS 40
#define TERM_ROWS 25
#define TERM_CELL 8
#define TERM_FBW  320                 /* framebuffer width in pixels (default 320x200) */
#define FB_BASE   0x00100000UL        /* 1 MiB: past the loaded image + vector table */

/* 8 basic ANSI colors as XRGB8888 (0x00RRGGBB): index 0..7 =
 * black, red, green, yellow, blue, magenta, cyan, white/grey. */
static const unsigned int term_palette[8] = {
    0x000000u, 0xAA0000u, 0x00AA00u, 0xAA5500u,
    0x0000AAu, 0xAA00AAu, 0x00AAAAu, 0xAAAAAAu
};

/* Grid + cursor + current SGR state. attr byte packs fg in the low nibble, bg in the
 * high nibble: attr = (bg << 4) | fg. */
static unsigned int  *term_fb;                          /* runtime-loaded FB_BASE pointer */
static unsigned char  term_glyph[TERM_ROWS][TERM_COLS];
static unsigned char  term_attr[TERM_ROWS][TERM_COLS];
static int term_row, term_col;
static int term_fg = 7, term_bg = 0;

/* CSI parser state: 0 normal, 1 saw ESC, 2 collecting CSI params. */
static int term_pstate;
static int term_param[8];
static int term_pidx;                  /* index of the current param */
static int term_pseen;                 /* any digit seen in this CSI */

/* Keyboard shift state (Set-1). */
static int term_shift;

static int term_curattr(void) { return (term_bg << 4) | term_fg; }

/* Blit one cell into the pixel buffer: fg where the glyph bit is set, bg elsewhere. */
static void term_draw_cell(int row, int col)
{
    int ch = term_glyph[row][col];
    int a  = term_attr[row][col];
    unsigned int fg = term_palette[a & 7];
    unsigned int bg = term_palette[(a >> 4) & 7];
    int idx, gy, gx, py, base, bits;

    if (ch < FONT_FIRST || ch > FONT_LAST) {
        ch = 0x20;
    }
    idx = ch - FONT_FIRST;
    for (gy = 0; gy < 8; gy++) {
        bits = font8x8[idx][gy];
        py   = row * TERM_CELL + gy;
        base = py * TERM_FBW + col * TERM_CELL;
        for (gx = 0; gx < 8; gx++) {
            if ((bits >> gx) & 1) {
                term_fb[base + gx] = fg;
            } else {
                term_fb[base + gx] = bg;
            }
        }
    }
}

static void term_render_all(void)
{
    int r, c;
    for (r = 0; r < TERM_ROWS; r++) {
        for (c = 0; c < TERM_COLS; c++) {
            term_draw_cell(r, c);
        }
    }
}

static void term_clear_cell(int r, int c)
{
    term_glyph[r][c] = 0x20;
    term_attr[r][c]  = (unsigned char)term_curattr();
}

/* Scroll the grid up one line: rows shift up, the new bottom row clears, re-render. This
 * is ordinary terminal scrolling, not scrollback (no history is retained). */
static void term_scroll(void)
{
    int r, c, last;
    for (r = 0; r < TERM_ROWS - 1; r++) {
        for (c = 0; c < TERM_COLS; c++) {
            term_glyph[r][c] = term_glyph[r + 1][c];
            term_attr[r][c]  = term_attr[r + 1][c];
        }
    }
    last = TERM_ROWS - 1;
    for (c = 0; c < TERM_COLS; c++) {
        term_clear_cell(last, c);
    }
    term_render_all();
}

static void term_newline(void)
{
    term_col = 0;
    term_row++;
    if (term_row >= TERM_ROWS) {
        term_row = TERM_ROWS - 1;
        term_scroll();
    }
}

static void term_put_glyph(int ch)
{
    term_glyph[term_row][term_col] = (unsigned char)ch;
    term_attr[term_row][term_col]  = (unsigned char)term_curattr();
    term_draw_cell(term_row, term_col);
    term_col++;
    if (term_col >= TERM_COLS) {
        term_col = 0;
        term_row++;
        if (term_row >= TERM_ROWS) {
            term_row = TERM_ROWS - 1;
            term_scroll();
        }
    }
}

static void term_csi_dispatch(int final)
{
    int n, r, c, i, np;

    np = term_pidx + 1;
    n  = term_param[0];

    switch (final) {
    case 'A':
        if (!term_pseen || n == 0) n = 1;
        term_row -= n;
        if (term_row < 0) term_row = 0;
        break;
    case 'B':
        if (!term_pseen || n == 0) n = 1;
        term_row += n;
        if (term_row > TERM_ROWS - 1) term_row = TERM_ROWS - 1;
        break;
    case 'C':
        if (!term_pseen || n == 0) n = 1;
        term_col += n;
        if (term_col > TERM_COLS - 1) term_col = TERM_COLS - 1;
        break;
    case 'D':
        if (!term_pseen || n == 0) n = 1;
        term_col -= n;
        if (term_col < 0) term_col = 0;
        break;
    case 'H':
    case 'f': {
        int rr = term_pseen ? term_param[0] : 1;
        int cc = (np >= 2) ? term_param[1] : 1;
        if (rr < 1) rr = 1;
        if (cc < 1) cc = 1;
        term_row = rr - 1;
        term_col = cc - 1;
        if (term_row > TERM_ROWS - 1) term_row = TERM_ROWS - 1;
        if (term_col > TERM_COLS - 1) term_col = TERM_COLS - 1;
        break;
    }
    case 'J': {
        int m = term_pseen ? term_param[0] : 0;
        if (m == 2) {
            for (r = 0; r < TERM_ROWS; r++)
                for (c = 0; c < TERM_COLS; c++)
                    term_clear_cell(r, c);
        } else if (m == 0) {
            for (c = term_col; c < TERM_COLS; c++)
                term_clear_cell(term_row, c);
            for (r = term_row + 1; r < TERM_ROWS; r++)
                for (c = 0; c < TERM_COLS; c++)
                    term_clear_cell(r, c);
        } else if (m == 1) {
            for (r = 0; r < term_row; r++)
                for (c = 0; c < TERM_COLS; c++)
                    term_clear_cell(r, c);
            for (c = 0; c <= term_col; c++)
                term_clear_cell(term_row, c);
        }
        term_render_all();
        break;
    }
    case 'K': {
        int m = term_pseen ? term_param[0] : 0;
        if (m == 0) {
            for (c = term_col; c < TERM_COLS; c++)
                term_clear_cell(term_row, c);
        } else if (m == 1) {
            for (c = 0; c <= term_col; c++)
                term_clear_cell(term_row, c);
        } else if (m == 2) {
            for (c = 0; c < TERM_COLS; c++)
                term_clear_cell(term_row, c);
        }
        term_render_all();
        break;
    }
    case 'm':
        if (!term_pseen) {
            term_fg = 7;
            term_bg = 0;
        } else {
            for (i = 0; i < np; i++) {
                int p = term_param[i];
                if (p == 0) {
                    term_fg = 7;
                    term_bg = 0;
                } else if (p >= 30 && p <= 37) {
                    term_fg = p - 30;
                } else if (p >= 40 && p <= 47) {
                    term_bg = p - 40;
                }
            }
        }
        break;
    default:
        break;   /* unknown final byte: consumed and ignored */
    }
}

/* Feed one output byte to the terminal. Handles printable bytes, CR/LF/BS/TAB, and the
 * ANSI/VT subset above. */
static void term_putc(int chi)
{
    unsigned char ch = (unsigned char)chi;

    if (term_pstate == 0) {
        if (ch == 0x1B) { term_pstate = 1; return; }
        if (ch == 0x0D) { term_col = 0; return; }
        if (ch == 0x0A) { term_newline(); return; }
        if (ch == 0x08) { if (term_col > 0) term_col--; return; }
        if (ch == 0x09) {
            int nc = (term_col & ~7) + 8;
            if (nc > TERM_COLS - 1) nc = TERM_COLS - 1;
            term_col = nc;
            return;
        }
        if (ch >= 0x20 && ch <= 0x7E) { term_put_glyph(ch); return; }
        return;   /* other control bytes ignored */
    }

    if (term_pstate == 1) {
        if (ch == '[') {
            term_pstate = 2;
            term_pidx   = 0;
            term_param[0] = 0;
            term_pseen  = 0;
            return;
        }
        term_pstate = 0;   /* unsupported / incomplete ESC sequence: consume defensively */
        return;
    }

    /* term_pstate == 2: CSI, collecting parameters. */
    if (ch >= '0' && ch <= '9') {
        term_param[term_pidx] = term_param[term_pidx] * 10 + (ch - '0');
        term_pseen = 1;
        return;
    }
    if (ch == ';') {
        if (term_pidx < 7) {
            term_pidx++;
            term_param[term_pidx] = 0;
        }
        return;
    }
    /* Any other byte is the final byte: dispatch and leave CSI mode. */
    term_csi_dispatch(ch);
    term_pstate = 0;
}

static void term_write(const unsigned char *s, int len)
{
    int i;
    for (i = 0; i < len; i++) {
        term_putc(s[i]);
    }
}

static void term_clear(void)
{
    int r, c, a;
    a = term_curattr();
    for (r = 0; r < TERM_ROWS; r++) {
        for (c = 0; c < TERM_COLS; c++) {
            term_glyph[r][c] = 0x20;
            term_attr[r][c]  = (unsigned char)a;
        }
    }
    term_row = 0;
    term_col = 0;
    term_render_all();
}

static void term_init(void)
{
    term_fb = (unsigned int *)FB_BASE;
    fb_set_base((void *)FB_BASE);
    term_fg = 7;
    term_bg = 0;
    term_pstate = 0;
    term_pidx   = 0;
    term_pseen  = 0;
    term_shift  = 0;
    term_clear();
    fb_present();
}

/* Set-1 (XT) make-code -> ASCII, US layout. Index by make code (0..0x3F); 0 = no glyph.
 * The upper table is selected while shift is held (upper/symbol column). Enter is handled
 * specially in term_key (CR + LF), so its table slot is unused there. */
static const unsigned char term_sc_lower[0x40] = {
    /* 0x00 */ 0,    0,   '1', '2', '3', '4', '5', '6',
    /* 0x08 */ '7',  '8', '9', '0', '-', '=', 0x08, 0x09,
    /* 0x10 */ 'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',
    /* 0x18 */ 'o',  'p', '[', ']', 0x0D, 0,  'a', 's',
    /* 0x20 */ 'd',  'f', 'g', 'h', 'j', 'k', 'l', ';',
    /* 0x28 */ '\'', '`', 0,   '\\','z', 'x', 'c', 'v',
    /* 0x30 */ 'b',  'n', 'm', ',', '.', '/', 0,   '*',
    /* 0x38 */ 0,    ' ', 0,   0,   0,   0,   0,   0
};

static const unsigned char term_sc_upper[0x40] = {
    /* 0x00 */ 0,    0,   '!', '@', '#', '$', '%', '^',
    /* 0x08 */ '&',  '*', '(', ')', '_', '+', 0x08, 0x09,
    /* 0x10 */ 'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    /* 0x18 */ 'O',  'P', '{', '}', 0x0D, 0,  'A', 'S',
    /* 0x20 */ 'D',  'F', 'G', 'H', 'J', 'K', 'L', ':',
    /* 0x28 */ '"',  '~', 0,   '|', 'Z', 'X', 'C', 'V',
    /* 0x30 */ 'B',  'N', 'M', '<', '>', '?', 0,   '*',
    /* 0x38 */ 0,    ' ', 0,   0,   0,   0,   0,   0
};

/* Translate one Set-1 scancode and echo any resulting byte(s). LShift/RShift make set the
 * shift state; their releases ($AA/$B6) clear it. Enter emits CR then LF. Other break
 * codes (bit7 set) and codes outside the make-code table are ignored. */
static void term_key(unsigned sc)
{
    if (sc == 0x2A || sc == 0x36) { term_shift = 1; return; }
    if (sc == 0xAA || sc == 0xB6) { term_shift = 0; return; }
    if (sc & 0x80) return;              /* other break codes: ignored */
    if (sc >= 0x40) return;             /* outside the make-code table */
    if (sc == 0x1C) {                   /* Enter: CR + LF */
        term_putc(0x0D);
        term_putc(0x0A);
        return;
    }
    {
        unsigned char a = term_shift ? term_sc_upper[sc] : term_sc_lower[sc];
        if (a) {
            term_putc(a);
        }
    }
}

#endif /* MAIZE_TERM_CORE_H */
