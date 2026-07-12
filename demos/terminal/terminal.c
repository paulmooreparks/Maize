/* demos/terminal/terminal.c -- interactive self-hosted framebuffer terminal (maize-121).
 *
 * The visible-window demo entry point: register the framebuffer buffer, clear the screen,
 * then poll the keyboard and echo typed keys as glyph cells (a typewriter / echo loop).
 * The terminal engine lives in term_core.h; this file is just the interactive main.
 *
 * Build + run (visible window, SDL2 backend behind MAIZE_DISPLAY; see README.md):
 *   scripts/cc-maize.sh --dev -o demos/terminal/terminal.mzx demos/terminal/terminal.c
 *   maize --display --input=keyboard demos/terminal/terminal.mzx
 *
 * The headless CI gate is terminal_selfcheck.c, wired into scripts/run-ctest.sh. This
 * interactive loop is a manual demo, not a CI gate: it polls forever, so it is not run
 * headlessly (there is no exit path without a window to close).
 */
#include "term_core.h"

int
main(void)
{
    term_init();

    for (;;) {
        if (kbd_status() & 1) {
            unsigned sc = kbd_read();
            term_key(sc);
            fb_present();
        }
    }
}
