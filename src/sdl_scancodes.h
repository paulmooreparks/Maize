#pragma once

/* maize-264: the single SDL-scancode -> Set-1 (XT) make-code table, shared by the
   in-process SDL window backend (src/devices.cpp) and the out-of-process presenter's SDL
   event loop (src/presenter_main.cpp). Extracted from devices.cpp so the presenter reuses
   the exact same mapping rather than carrying a second copy (the "broadened shared rule"
   duplication class the workbench convention-counterexamples doc warns against). The break
   code is the make code with bit 7 set; unmapped keys return 0 (ignored). SDL-only, so it
   is guarded by MAIZE_DISPLAY exactly like every other SDL reference in the tree. */

#ifdef MAIZE_DISPLAY

#include <SDL.h>
#include "maize_cpu.h"

namespace maize {
namespace devices {
namespace display {

inline maize::u_byte map_scancode(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_ESCAPE: return 0x01;
        case SDL_SCANCODE_1: return 0x02;
        case SDL_SCANCODE_2: return 0x03;
        case SDL_SCANCODE_3: return 0x04;
        case SDL_SCANCODE_4: return 0x05;
        case SDL_SCANCODE_5: return 0x06;
        case SDL_SCANCODE_6: return 0x07;
        case SDL_SCANCODE_7: return 0x08;
        case SDL_SCANCODE_8: return 0x09;
        case SDL_SCANCODE_9: return 0x0A;
        case SDL_SCANCODE_0: return 0x0B;
        case SDL_SCANCODE_Q: return 0x10;
        case SDL_SCANCODE_W: return 0x11;
        case SDL_SCANCODE_E: return 0x12;
        case SDL_SCANCODE_R: return 0x13;
        case SDL_SCANCODE_T: return 0x14;
        case SDL_SCANCODE_Y: return 0x15;
        case SDL_SCANCODE_U: return 0x16;
        case SDL_SCANCODE_I: return 0x17;
        case SDL_SCANCODE_O: return 0x18;
        case SDL_SCANCODE_P: return 0x19;
        case SDL_SCANCODE_A: return 0x1E;
        case SDL_SCANCODE_S: return 0x1F;
        case SDL_SCANCODE_D: return 0x20;
        case SDL_SCANCODE_F: return 0x21;
        case SDL_SCANCODE_G: return 0x22;
        case SDL_SCANCODE_H: return 0x23;
        case SDL_SCANCODE_J: return 0x24;
        case SDL_SCANCODE_K: return 0x25;
        case SDL_SCANCODE_L: return 0x26;
        case SDL_SCANCODE_Z: return 0x2C;
        case SDL_SCANCODE_X: return 0x2D;
        case SDL_SCANCODE_C: return 0x2E;
        case SDL_SCANCODE_V: return 0x2F;
        case SDL_SCANCODE_B: return 0x30;
        case SDL_SCANCODE_N: return 0x31;
        case SDL_SCANCODE_M: return 0x32;
        case SDL_SCANCODE_COMMA: return 0x33;
        case SDL_SCANCODE_PERIOD: return 0x34;
        case SDL_SCANCODE_RETURN: return 0x1C;
        case SDL_SCANCODE_LCTRL: return 0x1D;
        case SDL_SCANCODE_LSHIFT: return 0x2A;
        case SDL_SCANCODE_LALT: return 0x38;
        case SDL_SCANCODE_SPACE: return 0x39;
        case SDL_SCANCODE_UP: return 0x48;
        case SDL_SCANCODE_LEFT: return 0x4B;
        case SDL_SCANCODE_RIGHT: return 0x4D;
        case SDL_SCANCODE_DOWN: return 0x50;
        case SDL_SCANCODE_TAB: return 0x0F;         // automap
        case SDL_SCANCODE_MINUS: return 0x0C;       // reduce view / zoom out
        case SDL_SCANCODE_EQUALS: return 0x0D;      // increase view / zoom in
        case SDL_SCANCODE_BACKSPACE: return 0x0E;
        case SDL_SCANCODE_RCTRL: return 0x1D;       // right ctrl also fires
        case SDL_SCANCODE_RSHIFT: return 0x36;      // right shift also runs
        case SDL_SCANCODE_RALT: return 0x38;        // right alt also strafes
        case SDL_SCANCODE_PAUSE: return 0x45;
        case SDL_SCANCODE_F1: return 0x3B;
        case SDL_SCANCODE_F2: return 0x3C;
        case SDL_SCANCODE_F3: return 0x3D;
        case SDL_SCANCODE_F4: return 0x3E;
        case SDL_SCANCODE_F5: return 0x3F;
        case SDL_SCANCODE_F6: return 0x40;
        case SDL_SCANCODE_F7: return 0x41;
        case SDL_SCANCODE_F8: return 0x42;
        case SDL_SCANCODE_F9: return 0x43;
        case SDL_SCANCODE_F10: return 0x44;
        case SDL_SCANCODE_F11: return 0x57;
        case SDL_SCANCODE_F12: return 0x58;
        /* maize-140: punctuation + navigation keys the text console needs for a
           usable typing experience (the DOOM key path ignores the extras). */
        case SDL_SCANCODE_SEMICOLON: return 0x27;
        case SDL_SCANCODE_APOSTROPHE: return 0x28;
        case SDL_SCANCODE_GRAVE: return 0x29;
        case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
        case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
        case SDL_SCANCODE_BACKSLASH: return 0x2B;
        case SDL_SCANCODE_SLASH: return 0x35;
        case SDL_SCANCODE_HOME: return 0x47;
        case SDL_SCANCODE_END: return 0x4F;
        case SDL_SCANCODE_PAGEUP: return 0x49;
        case SDL_SCANCODE_PAGEDOWN: return 0x51;
        case SDL_SCANCODE_DELETE: return 0x53;
        case SDL_SCANCODE_INSERT: return 0x52;
        default: return 0;
    }
}

}  // namespace display
}  // namespace devices
}  // namespace maize

#endif  // MAIZE_DISPLAY
