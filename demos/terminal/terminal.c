/* demos/terminal/terminal.c -- interactive self-hosted framebuffer terminal (maize-121).
 *
 * The visible-window demo entry point: register the framebuffer buffer, clear the screen,
 * then wait for typed keys and echo them as glyph cells (a typewriter / echo loop). The
 * terminal engine lives in term_core.h; this file is just the interactive main.
 *
 * Input is interrupt-driven, not busy-polled: the loop parks the CPU in HALT between
 * keystrokes (wait_for_irq) so an idle terminal drops to ~0 MIPS instead of pinning the
 * core spinning on the status port. A keypress wakes it immediately (keyboard IRQ); the
 * vsync IRQ is the periodic backstop that keeps the wait race-free. See toolchain/rt/
 * mzdev.h for the model.
 *
 * Build + run (visible window, SDL2 backend behind MAIZE_DISPLAY; see README.md):
 *   scripts/cc-maize.sh --dev -o demos/terminal/terminal.mzx demos/terminal/terminal.c
 *   maizeg --display --input=keyboard demos/terminal/terminal.mzx
 *
 * The headless CI gate is terminal_selfcheck.c, wired into scripts/run-ctest.sh. This
 * interactive loop is a manual demo, not a CI gate: it waits for input forever, so it is
 * not run headlessly (there is no exit path without a window to close).
 */
#include "term_core.h"

int
main(void)
{
    term_init();

    /* Install the wake handlers and unmask interrupts before the first wait. */
    kbd_irq_install();
    vsync_irq_enable();
    irq_enable();

    for (;;) {
        while (!(kbd_status() & 1)) {
            wait_for_irq();   /* HALT: park until a keypress or the next vblank */
        }
        unsigned sc = kbd_read();
        term_key(sc);
        fb_present();
    }
}
