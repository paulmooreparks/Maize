/* toolchain/rt/mzdev.h -- C binding for the Maize standard-device ports (maize-121).
 *
 * The Maize C toolchain cannot emit port I/O (IN / OUT) from C, so the raw port access
 * lives in a hand-written asm shim (toolchain/rt/mzdev.mazm), exactly as the raw syscall
 * stubs live in toolchain/rt/syscall.mazm. This header declares the C-callable stubs; a
 * guest-C program includes it and links mzdev.mzo via cc-maize.sh's opt-in `--dev` flag.
 *
 * It targets the frozen standard-device pinout (docs/spec/device-surface.md Ch.11;
 * src/devices.{h,cpp}, maize-83). The framebuffer is memory-backed: pixels are ordinary
 * stores into a guest-RAM buffer whose base is registered with fb_set_base; the device
 * copies that buffer on fb_present (synchronous present-on-change), never per pixel.
 *
 *   Keyboard    $10 R  scancode (Set-1/XT make; release = make | $80); read clears
 *                      key-available
 *               $11 R  bit0 key-available
 *   Framebuffer $50 R  width in pixels (host config; default 320)
 *               $51 R  height in pixels (host config; default 200)
 *               $52 R  format id (1 = XRGB8888, pixel = 0x00RRGGBB, 4 bytes/px)
 *               $53 R/W guest base address of the pixel buffer
 *               $54 W  present a frame; R bit0 last-present-valid
 *
 * This header is preprocessed by the system cpp before cproc-qbe (cc-maize.sh), so
 * ordinary include guards are available.
 */
#ifndef MAIZE_MZDEV_H
#define MAIZE_MZDEV_H

/* Keyboard. kbd_status returns bit0 = key-available; poll it, then kbd_read to take the
 * latched Set-1 scancode (the read clears key-available so the next code can latch). */
unsigned kbd_status(void);
unsigned kbd_read(void);

/* Framebuffer control plane. width/height/format are read-only host config. */
unsigned fb_width(void);
unsigned fb_height(void);
unsigned fb_format(void);

/* Register the guest-RAM pixel buffer base (port $53). Call once before presenting. */
void fb_set_base(void *base);

/* Present the current contents of the registered buffer (port $54). The written value is
 * ignored by the device; presence is synchronous present-on-change. */
void fb_present(void);

/* Read bit0 last-present-valid (port $54 read): 1 after a valid present, else 0. */
unsigned fb_present_valid(void);

#endif /* MAIZE_MZDEV_H */
