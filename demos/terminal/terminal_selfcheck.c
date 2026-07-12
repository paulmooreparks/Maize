/* demos/terminal/terminal_selfcheck.c -- headless CI gate for the terminal (maize-121).
 *
 * Two phases, both verified by reading the rendered pixels back out of the guest-RAM
 * framebuffer buffer (the pattern asm/test_framebuffer.mazm uses) and comparing them
 * against the font + palette the blitter used, so the check is self-consistent:
 *
 *   Phase A  drives term_write with a fixed ASCII + escape script (fully deterministic,
 *            no keyboard) exercising printable render, SGR color, CUP, cursor motion
 *            (CUU/CUD/CUF/CUB), EL, ED, line wrap, and bottom-of-screen scroll.
 *   Phase B  polls the keyboard for a fixed Set-1 scancode sequence injected via
 *            `maize --input=keyboard` (including shifted make codes), echoes it through
 *            the same render path, and checks the resulting glyph cells.
 *
 * Prints exactly "terminal: PASS" on success (else "terminal: FAIL"). Wired into
 * scripts/run-ctest.sh, which pipes the scancode sequence and matches stdout.
 */
#include "term_core.h"
#include "stdio.h"

static int selfcheck_ok = 1;

static void expect(int cond)
{
    if (!cond) {
        selfcheck_ok = 0;
    }
}

/* True iff the 8x8 cell at (row,col) holds glyph `ch` rendered with fg/bg palette indices
 * fgi/bgi: recompute each pixel from font8x8 + term_palette and compare to the guest-RAM
 * buffer. The pixel read is inlined and compared directly against fg/bg (rather than via a
 * helper or a `want` temp): the pinned qbe -t maize backend has 11 allocatable registers
 * and cannot spill, so this keeps the live-value count in the hot loop within budget, the
 * same shape term_core.h's term_draw_cell uses. */
static int cell_matches(int row, int col, int ch, int fgi, int bgi)
{
    unsigned int fg = term_palette[fgi & 7];
    unsigned int bg = term_palette[bgi & 7];
    int idx, gy, gx;

    if (ch < FONT_FIRST || ch > FONT_LAST) {
        ch = 0x20;
    }
    idx = ch - FONT_FIRST;
    for (gy = 0; gy < 8; gy++) {
        int bits = font8x8[idx][gy];
        int base = (row * 8 + gy) * TERM_FBW + col * 8;
        for (gx = 0; gx < 8; gx++) {
            unsigned int got = term_fb[base + gx];
            if ((bits >> gx) & 1) {
                if (got != fg) return 0;
            } else {
                if (got != bg) return 0;
            }
        }
    }
    return 1;
}

/* term_write with a NUL-terminated C string (length computed here). None of the scripts
 * below contain an embedded NUL. */
static void tw(const char *s)
{
    int n = 0;
    while (s[n]) {
        n++;
    }
    term_write((const unsigned char *)s, n);
}

int
main(void)
{
    int consumed;

    /* ---------------- Phase A: deterministic render + ANSI ---------------- */
    term_init();

    /* Printable render at the default colors (fg 7 white, bg 0 black). */
    tw("Hi");
    expect(cell_matches(0, 0, 'H', 7, 0));
    expect(cell_matches(0, 1, 'i', 7, 0));

    /* SGR: red fg (31) on blue bg (44), then a glyph at (0,2). */
    tw("\x1b[31;44mX");
    expect(cell_matches(0, 2, 'X', 1, 4));

    /* SGR reset to defaults. */
    tw("\x1b[0m");

    /* EL: home the cursor, erase the whole line (n=2); (0,0) and (0,2) go blank. */
    tw("\x1b[1;1H\x1b[2K");
    expect(cell_matches(0, 0, ' ', 7, 0));
    expect(cell_matches(0, 2, ' ', 7, 0));

    /* CUP: row 5, col 10 (1-based) -> cell (4,9); write Z there. */
    tw("\x1b[5;10HZ");
    expect(cell_matches(4, 9, 'Z', 7, 0));

    /* Cursor motion: CUP (10,10) -> (9,9); up 3 -> row 6; right 2 -> col 11; write M. */
    tw("\x1b[10;10H\x1b[3A\x1b[2CM");
    expect(cell_matches(6, 11, 'M', 7, 0));
    /* From (6,12): down 2 -> row 8; left 5 -> col 7; write N. */
    tw("\x1b[2B\x1b[5DN");
    expect(cell_matches(8, 7, 'N', 7, 0));

    /* ED: erase the whole screen (n=2); the glyphs above all go blank. */
    tw("\x1b[2J");
    expect(cell_matches(4, 9, ' ', 7, 0));
    expect(cell_matches(6, 11, ' ', 7, 0));
    expect(cell_matches(8, 7, ' ', 7, 0));

    /* Line wrap: CUP (21,39) -> (20,38); "12" fills the last two columns, then the cursor
     * wraps to (21,0) where '3' lands. */
    tw("\x1b[21;39H123");
    expect(cell_matches(20, 38, '1', 7, 0));
    expect(cell_matches(20, 39, '2', 7, 0));
    expect(cell_matches(21, 0, '3', 7, 0));

    /* Scroll: CUP to the last row (25,1) -> (24,0); write S; LF advances past the bottom
     * and scrolls up one line, so S moves to row 23 and the last row clears. */
    tw("\x1b[25;1HS\n");
    expect(cell_matches(23, 0, 'S', 7, 0));
    expect(cell_matches(24, 0, ' ', 7, 0));

    /* Present-on-change: a present must have happened and reported valid. */
    fb_present();
    expect(fb_present_valid() == 1);

    /* ---------------- Phase B: keyboard echo (Set-1 -> ASCII) ---------------- */
    /* Reset the screen so echoes land at known cells starting at (0,0). The run-ctest
     * harness injects, via `maize --input=keyboard`, the 9 scancodes:
     *   1E              -> 'a'
     *   2A 1E AA        -> 'A'  (LShift make, A make, LShift release)
     *   02              -> '1'
     *   2A 02 AA        -> '!'  (LShift make, 1 make, LShift release)
     *   39              -> ' '  (space)
     * yielding echoed glyphs a A 1 ! (space) in cells (0,0)..(0,4). */
    term_clear();

    consumed = 0;
    while (consumed < 9) {
        if (kbd_status() & 1) {
            unsigned sc = kbd_read();
            consumed++;
            term_key(sc);
        }
    }
    fb_present();

    expect(cell_matches(0, 0, 'a', 7, 0));
    expect(cell_matches(0, 1, 'A', 7, 0));
    expect(cell_matches(0, 2, '1', 7, 0));
    expect(cell_matches(0, 3, '!', 7, 0));
    expect(cell_matches(0, 4, ' ', 7, 0));

    puts(selfcheck_ok ? "terminal: PASS" : "terminal: FAIL");
    return 0;
}
