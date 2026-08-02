#pragma once

/* maize-345: the Windows console readiness probe and the buffered console read that
   backs it. Guarded on _WIN32 so the TU compiles to nothing on POSIX, mirroring the
   hostfs and presenter-transport backend convention (CMakeLists.txt).

   This file exists so that maize-313 can replace the MECHANISM behind
   maize::syscall::console_stdin_ready() without unpicking src/sys.cpp. The seam and
   its 1/0/-1 contract stay in src/maize_sys.h; everything console-shaped lives here.

   The four entry points below are what src/sys.cpp calls and what the Windows console
   test binary (src/console_probe_test_win32.cpp) drives against a console it owns. The
   handle is a PARAMETER rather than an internal GetStdHandle call precisely so a test
   can pass a CONIN$ handle of its own.

   The POSIX readiness probe (src/sys.cpp, the __linux__ branch) is a real
   poll()+FIONREAD and is not affected by anything here. */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace maize {
namespace console_probe {

    /* True when h is a real interactive console: FILE_TYPE_CHAR and GetConsoleMode
       succeeds. This is the isatty analog, and the gate src/sys.cpp uses to decide
       whether an fd-0 read goes through read_console() below or through the plain
       pipe/file chunking loop. NUL is FILE_TYPE_CHAR but is not a console. */
    bool is_console(HANDLE h);

    /* The whole handle-type dispatch for one stdin handle. Returns 1 (a data byte is
       pending), 0 (nothing pending yet) or -1 (end of input). For a real console the
       answer is derived from the pre-read buffer plus the console input queue, never
       from GetConsoleMode alone. */
    int stdin_ready(HANDLE h);

    /* Pure classifier over one peeked record: does reading it yield a byte, given the
       console's current input mode? Testable with synthesized records, which is the
       whole reason it is exposed. */
    bool record_yields_byte(const INPUT_RECORD& r, DWORD console_mode);

    /* Buffered console read (the pre-read byte buffer). Returns the number of bytes
       delivered, 0 at end of input, or -1 on a host failure the caller folds into the
       ABI I/O-failure code. A short return is normal and is not an error: this never
       issues a second ReadFile to top up a partly satisfied request, because that
       second read could block. */
    long read_console(HANDLE h, unsigned char* buf, unsigned long count);

}  // namespace console_probe
}  // namespace maize

#endif  // _WIN32
