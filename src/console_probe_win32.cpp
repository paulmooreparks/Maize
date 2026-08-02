/* maize-345: the Windows console readiness probe and buffered console read. See
   console_probe_win32.h for why this is its own translation unit. Guarded on _WIN32 so
   it compiles to nothing on POSIX, mirroring the hostfs and presenter-transport backend
   convention.

   STEP 1 of the card (the extraction). This file currently carries the logic that lived
   in src/sys.cpp verbatim, including the FILE_TYPE_CHAR branch's constant answer, so
   that the console test binary can be built against the PRE-FIX predicate and recorded
   failing before the predicate changes (maize-345 AC-2). The truthful predicate and the
   pre-read byte buffer land next. */

#include "console_probe_win32.h"

#ifdef _WIN32

namespace maize {
namespace console_probe {

    bool is_console(HANDLE h) {
        if (h == INVALID_HANDLE_VALUE || h == nullptr) { return false; }
        if (GetFileType(h) != FILE_TYPE_CHAR) { return false; }
        DWORD mode {0};
        return GetConsoleMode(h, &mode) != 0;
    }

    bool record_yields_byte(const INPUT_RECORD& r, DWORD console_mode) {
        (void)console_mode;
        if (r.EventType != KEY_EVENT) { return false; }
        if (!r.Event.KeyEvent.bKeyDown) { return false; }
        return r.Event.KeyEvent.uChar.UnicodeChar != 0;
    }

    /* maize-238 (Branch A, decision 9285): NON-CONSUMING host-stdin readiness probe.
       Windows stdin has no single O_NONBLOCK knob, so dispatch on the handle type (the
       maize-237 console-vs-pipe-vs-file divergence): a pipe uses PeekNamedPipe to test
       for buffered bytes without consuming; a redirected file compares position to size
       without consuming; an interactive console reports "data pending" so the on-demand
       data-port read blocks for a real line (no deadlock). That last branch is the
       maize-345 defect and is replaced in the next commit. Returns 1 (a data byte is
       pending), 0 (nothing pending yet), or -1 (end of input). */
    int stdin_ready(HANDLE h) {
        if (h == INVALID_HANDLE_VALUE || h == nullptr) {
            return -1;
        }
        DWORD type {GetFileType(h)};
        if (type == FILE_TYPE_PIPE) {
            DWORD avail {0};
            if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
                return -1;   // broken pipe == EOF (write end closed)
            }
            return (avail > 0) ? 1 : 0;   // buffered bytes == data pending; else nothing yet
        }
        if (type == FILE_TYPE_CHAR) {
            // A char handle is either an interactive console or a null-ish device (NUL).
            // GetConsoleMode succeeds only on a real console (the isatty analog): treat a
            // real console as data pending (defer to the blocking data-port read), and a
            // non-console char device (NUL, whose reads return 0) as EOF so the data port
            // never synthesizes a NUL byte from a zero-length read.
            DWORD mode {0};
            return GetConsoleMode(h, &mode) ? 1 : -1;
        }
        if (type == FILE_TYPE_DISK) {
            LARGE_INTEGER pos, size, zero;
            zero.QuadPart = 0;
            if (!SetFilePointerEx(h, zero, &pos, FILE_CURRENT)) { return 1; }
            if (!GetFileSizeEx(h, &size)) { return 1; }
            return (pos.QuadPart < size.QuadPart) ? 1 : -1;   // bytes remaining vs EOF
        }
        return 1;   // unknown handle type: a read resolves data vs EOF
    }

    /* The pre-fix console read: one ReadFile straight through, which is what the
       src/sys.cpp chunking loop already reduces to for a console handle (a console read
       returns at the line or at the first available input, so the loop's short-read
       break fires on the first iteration). Replaced by the buffered form next. */
    long read_console(HANDLE h, unsigned char* buf, unsigned long count) {
        if (count == 0) { return 0; }
        DWORD got {0};
        if (!ReadFile(h, buf, count, &got, nullptr)) {
            DWORD err {GetLastError()};
            if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) { return 0; }
            return -1;
        }
        return static_cast<long>(got);
    }

}  // namespace console_probe
}  // namespace maize

#endif  // _WIN32
