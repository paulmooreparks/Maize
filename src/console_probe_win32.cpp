/* maize-345: the Windows console readiness probe and buffered console read. See
   console_probe_win32.h for why this is its own translation unit. Guarded on _WIN32 so
   it compiles to nothing on POSIX, mirroring the hostfs and presenter-transport backend
   convention.

   The defect this replaces was `return GetConsoleMode(h, &mode) ? 1 : -1;`, which is 1
   for every real console whatever its input queue holds. A guest reading fd 0 therefore
   never parked, took the data port instead, and blocked the single CPU thread inside a
   host ReadFile until a human typed. No guest instruction executes while that read is
   outstanding, so the scheduler, the timer tick and every other process stop with it.

   ---------------------------------------------------------------------------------
   WHAT THE CONSOLE ACTUALLY DOES (measured, and stated in the vocabulary of the
   instrument that measured it)

   AC-4 and OQ-3 asked which of three worlds a Windows console is in when a raw-mode read
   consumes a record whose translation is longer than one byte. Raw mode and cooked mode
   turn out to be in DIFFERENT worlds. An earlier version of this comment named the third
   world for both, having inferred a fact about the record queue from an instrument that
   only ever watched the byte stream; the instrument now watches both observables and the
   raw-mode half of that answer is reversed. The general lesson is Entry 35 of the
   workbench's Convention counterexamples document, and the local rule is: name the API
   call a sentence is about, then check that the instrument calls it.

   Measured with console_probe_test_win32.exe --measure on Windows 11 (build 26200),
   classic conhost via AllocConsole, input code page 437, raw mode as
   host_tty::apply_host sets it (ENABLE_VIRTUAL_TERMINAL_INPUT set, ENABLE_LINE_INPUT /
   ENABLE_ECHO_INPUT / ENABLE_PROCESSED_INPUT clear). Each row injects one key-down record
   carrying UnicodeChar 0, reports what GetNumberOfConsoleInputEvents and PeekConsoleInputW
   see, and then reads the bytes back one at a time behind a sentinel so no read can block.

   RAW MODE IS THE PER-BYTE-RECORD WORLD, the one the spec named first. The console
   expands a virtual key into ONE KEY_EVENT RECORD PER BYTE, and it does so when the
   record is QUEUED rather than when it is read. Each expanded record carries
   wVirtualKeyCode 0, wVirtualScanCode 0 and one byte of the sequence in UnicodeChar. One
   injected VK_LEFT leaves THREE records queued (0x1B, 0x5B, 0x44) and the count falls
   3, 2, 1, 0 across the three one-byte reads; VK_F5 leaves five and falls 5, 4, 3, 2, 1,
   0. Every byte of every sequence below is therefore visible to a peek, and a peek-only
   predicate would NOT strand any of them:

       VK_LEFT    1B 5B 44        VK_PRIOR   1B 5B 35 7E     VK_F1   1B 4F 50
       VK_RIGHT   1B 5B 43        VK_NEXT    1B 5B 36 7E     VK_F2   1B 4F 51
       VK_UP      1B 5B 41        VK_INSERT  1B 5B 32 7E     VK_F3   1B 4F 52
       VK_DOWN    1B 5B 42        VK_DELETE  1B 5B 33 7E     VK_F4   1B 4F 53
       VK_HOME    1B 5B 48        VK_F5      1B 5B 31 35 7E  VK_F9   1B 5B 32 30 7E
       VK_END     1B 5B 46        VK_F6      1B 5B 31 37 7E  VK_F10  1B 5B 32 31 7E
                                  VK_F7      1B 5B 31 38 7E  VK_F11  1B 5B 32 33 7E
                                  VK_F8      1B 5B 31 39 7E  VK_F12  1B 5B 32 34 7E

       VK_BACK, VK_TAB and VK_RETURN queue ONE record each, carrying 7F, 09 and 0D in
       UnicodeChar. VK_BACK's 7F is the VT DEL convention rather than the 08 a reader
       would predict, which is why it was measured instead of reasoned about (Entry 34).
       VK_ESCAPE and the bare modifier and lock keys (VK_SHIFT, VK_CONTROL, VK_MENU,
       VK_CAPITAL, VK_LWIN, VK_NUMLOCK, VK_SCROLL, VK_APPS) queue ZERO records: the
       console drops them at WRITE time rather than translating them to nothing at read
       time. That is what the OQ-7 flush allowlist rests on, and it is also why those
       records cannot be presented to the probe through this console at all.
       'a': one record, 61.  U+00E9 under code page 437: one record, the single byte 82.

   COOKED MODE IS THE RETAINED-REMAINDER WORLD, and this half is what makes the buffer
   load-bearing. Records for "ab" then Enter stay unexpanded in the queue, each with its
   real virtual key and scan code. The FIRST one-byte read consumes all three records and
   the queue drops to zero while three bytes (62 0D 0A) are still owed. Those bytes live
   inside conhost where no peek can see them, and FlushConsoleInputBuffer does not discard
   them either, so a peek-only predicate answers 0 with a line's tail still readable and
   the guest parks on input it already has.

   Two consequences drive everything below, and they are not the two an earlier version of
   this comment drew. The pre-read byte buffer is required, for COOKED mode, because it is
   the only thing that makes the retained remainder visible to readiness. And vt_translated
   is a DEFENSIVE clause rather than the load-bearing one it was described as, because
   every record this console queues that can yield a byte carries a non-zero UnicodeChar
   and the character clause answers first. The set's own comment carries the one state in
   which the clause is reached, what the console does there, and why the set is kept.

   Maize never calls SetConsoleCP, so the input code page is whatever the session
   inherited. Under 437 a non-ASCII key is one byte; under 65001 (UTF-8) it is two or
   more. The buffer holds either without caring, which is the point of holding bytes
   rather than predicting them.

   OQ-5 and OQ-9 remain open for the operator, and they are the same run: WriteConsoleInputW
   proves how the predicate classifies records and proves nothing about which records a REAL
   keypress queues, and an AllocConsole console is classic conhost while a Windows Terminal
   session is ConPTY-backed. Everything above describes what conhost does with an injected
   record. console_probe_test_win32.exe --diagnostic prints the record count, every record
   field and the bytes one real keypress produces, against the INHERITED console, and is run
   by hand. */

#include "console_probe_win32.h"

#ifdef _WIN32

#include <cstring>

namespace maize {
namespace console_probe {

namespace {

    /* The peek window, in records. A typed character queues a key-down and a key-up, so
       256 records is roughly 128 typed characters. See "the cooked peek ceiling" below
       for why that is a stated limit rather than a tuning knob. */
    constexpr DWORD PEEK_CAP {256};

    /* ---- the pre-read byte buffer ---------------------------------------------------
       Filled by the READ path, never by the probe, and drained by every host read of
       fd 0. This is what makes the COOKED-mode retention measured above visible to
       readiness: the fill asks for PREREAD_CAP bytes rather than one, so a cooked line
       arrives whole instead of leaving its tail inside conhost behind an empty record
       queue, the probe reports the buffer through pre_read_pending(), and successive
       single-byte data-port reads (src/devices.cpp:62) deliver them in order.

       Raw mode does not need it. Conhost queues one record per byte there, so a peek sees
       every byte and the record queue alone would answer correctly. The buffer is harmless
       in that mode (it holds bytes the guest is about to ask for and reports them for
       exactly as long as it holds them), it costs one ReadFile instead of several for a
       multi-byte key, and having one path rather than a mode-dependent pair is worth more
       than the saving. The claim to make for raw mode is that the buffer is unnecessary,
       not that it is load-bearing.

       No state here can outlive a single guest read. g_pre_pos and g_pre_len describe
       bytes that actually exist and the pending count strictly decreases as the guest
       reads them. g_fill_was_full is cleared at the top of the next fill, in every mode,
       and a fill happens on the very next read of fd 0 once the buffer is empty. There
       is no reachable state in which the probe answers ready forever, which is what the
       withdrawn cycle-1 readiness flag allowed.

       The residual, stated rather than hidden: a fill returning exactly PREREAD_CAP
       bytes may or may not have left the console holding more, and that is invisible to
       any peek, so g_fill_was_full makes the probe answer ready once and the next fill
       resolves it. If the console had nothing more, that fill blocks. Reaching it takes
       a cooked line or a translated burst longer than 8192 bytes whose length is an
       exact multiple of 8192; it costs one blocking read rather than a permanent one,
       and it is today's behaviour for every cooked read rather than a new failure mode.
       Answering 0 instead would be worse: it strands real bytes with no queued record
       left to raise IRQ 33, which is a permanent hang.

       This is single-threaded state by construction. The CPU thread is the only caller
       on the VM path, and mazm links this TU without ever calling read_console, so the
       buffer stays empty in that process. (mazm does read stdin, at src/mazm.cpp's
       tokenize(std::cin, tree), but never through here; the claim to make when grepping
       is "the only host reads of fd 0 in the VM path", not "in the tree".)

       ---------------------------------------------------------------------------------
       WHAT A FILL PROMISES, AND THE TWO CASES WHERE IT DOES NOT

       "The fill cannot block" is true of the ordinary path and is NOT an invariant, and
       saying it flatly invites a reader to skip the guard. In raw mode a console ReadFile
       returns as soon as any input is available, and in cooked mode when the line
       completes, which is exactly what today's one-byte read waits for; so a fill issued
       after a probe that answered 1 from a queued readable record returns promptly.

       Two paths break it, both of them stated elsewhere in this file and neither of them
       new:
         - the cooked peek ceiling, where a full peek window with no line terminator in it
           reports ready and the fill then waits for the line to be finished; and
         - g_fill_was_full, which reports ready with zero bytes actually in hand.
       Both are bounded, both are today's behaviour rather than a regression, and both are
       why maize-313 is still wanted.

       ---------------------------------------------------------------------------------
       TWO USER-VISIBLE CONSEQUENCES OF BUFFERING

       Short reads now happen where they did not. read_console never issues a second
       ReadFile to top up a partly satisfied request, because that second read could
       block, so a caller asking for 64 bytes with 3 in the buffer gets 3. Every existing
       caller already survives that: console_device::port_read asks for one byte
       (src/devices.cpp:62), drain_stdin passes rc through (:136), the SYS read(0) path
       copies n bytes and returns n rather than count (src/sys.cpp), host_tty's
       check_kill_escape counts consecutive Ctrl-] across calls in file-scope state so a
       run split over two short reads still triggers, and the guest ABI's read is allowed
       to return fewer bytes than were asked for.

       Type-ahead now belongs to the VM rather than to the parent shell, which nobody had
       written down. Today a one-byte raw read leaves the rest of what you typed as
       records in the console's own input buffer, and the shell that launched maize
       inherits them when maize exits. After this card up to PREREAD_CAP bytes of it are
       inside this process and are discarded at exit. That is a behaviour change, it is
       arguably an improvement (leaked type-ahead executing in the parent shell is its own
       hazard), and it is recorded here rather than discovered later. */
    constexpr unsigned long PREREAD_CAP {8192};

    unsigned char g_pre[PREREAD_CAP];
    unsigned long g_pre_len {0};        // bytes the last fill delivered
    unsigned long g_pre_pos {0};        // next byte to hand out
    bool          g_fill_was_full {false};

    bool pre_read_pending() {
        return g_pre_pos < g_pre_len || g_fill_was_full;
    }

    /* The virtual keys that produce a byte sequence under ENABLE_VIRTUAL_TERMINAL_INPUT.
       Every member was measured non-empty by the --measure run quoted at the top of this
       file, and the byte tables there are what the set was derived from rather than
       hand-written.

       WHEN THIS CLAUSE IS REACHED, WHICH IS NOT WHERE IT WAS THOUGHT TO BE. Conhost
       expands a virtual key into one record per byte when the record is QUEUED, so every
       record it hands the probe in raw mode already carries a non-zero UnicodeChar and
       record_yields_byte answers true one clause earlier. That leaves exactly one state in
       which this set decides anything: a record queued while ENABLE_VIRTUAL_TERMINAL_INPUT
       was CLEAR keeps its virtual key and its zero UnicodeChar, and switching the console
       to raw afterwards does not expand it retroactively. quesOS reaches that state when a
       guest switches from cooked to raw with input already queued, and when a key is typed
       into the inherited cooked console before host_tty::apply_host runs.

       WHAT THE CONSOLE DOES IN THAT STATE, measured rather than reasoned about, by the
       --measure rows labelled vtoff_*. Injecting VK_LEFT with VT input clear leaves ONE
       record queued with wVirtualKeyCode 0x25 and UnicodeChar 0. Turning VT input on and
       reading yields NO byte for it, and the record is gone once the next byte arrives, so
       the read consumed it and produced nothing. VK_F5 and VK_BACK behave the same way.

       So on this console every member of this set is OVER-inclusive in the only state
       where the set is consulted, which is the dangerous direction: the probe answers 1,
       the fill then blocks inside ReadFile until some other key is pressed, and that is
       this card's own failure mechanism reappearing inside its fix. The narrow window and
       the recovery-on-next-keystroke bound the cost, and an UNDER-inclusive member would
       cost only one keystroke of latency (the record stays queued, the next ordinary
       character makes the probe answer 1, and the following read steps over the stale
       record and returns the character), which is why the asymmetry says prefer under.

       The set is nevertheless KEPT, and that is a deliberate hold rather than an
       oversight. Deleting it would be deciding, on classic-conhost evidence about INJECTED
       records, what a ConPTY-backed Windows Terminal does with a REAL keypress: a terminal
       that decodes VT itself may well queue the unexpanded virtual-key record, and there
       this clause is the only thing that reports an arrow key as pending at all. Choosing
       between those on unmeasured behaviour is what pushed this card back twice already.
       OQ-9 carries the decision and is gated on Done, and the operator's --diagnostic run
       against a real keypress in both terminals is what answers it. If that run shows both
       terminals expanding at queue time, this clause has no reachable use and should go.

       The last three members are here because measurement contradicted the reasoning that
       first left them out. Backspace, Tab, Escape and Enter were originally absent on the
       theory that the console puts their control character in UnicodeChar, so the
       character clause above would already cover them. Measured with UnicodeChar 0, that
       theory is wrong for three of the four: VK_BACK queues a record carrying 7F (the VT
       DEL convention, not 08), VK_TAB carries 09 and VK_RETURN carries 0D. VK_ESCAPE is
       the one member of that group that does NOT fit, and it is said out loud rather than
       left implicit: conhost drops it at write time and it yields nothing, so it stays OUT
       of this set. */
    bool vt_translated(WORD vk) {
        switch (vk) {
            case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
            case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
            case VK_INSERT: case VK_DELETE:
            case VK_F1: case VK_F2: case VK_F3: case VK_F4:
            case VK_F5: case VK_F6: case VK_F7: case VK_F8:
            case VK_F9: case VK_F10: case VK_F11: case VK_F12:
            case VK_BACK: case VK_TAB: case VK_RETURN:
                return true;
            default:
                return false;
        }
    }

    /* A bare modifier or lock key: pressing it can never produce a byte under any
       console mode, because it has no character and no console translates it. Measured
       for VK_SHIFT, VK_CONTROL, VK_MENU and VK_CAPITAL; the left/right variants and the
       remaining lock keys are the same class of key and are listed for completeness. */
    bool bare_modifier_or_lock(WORD vk) {
        switch (vk) {
            case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
            case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
            case VK_MENU: case VK_LMENU: case VK_RMENU:
            case VK_LWIN: case VK_RWIN: case VK_APPS:
            case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
                return true;
            default:
                return false;
        }
    }

    /* OQ-7, resolved: this is an ALLOWLIST of records that cannot yield a byte under any
       console mode, not a denylist over everything record_yields_byte rejected.

       D-5's flush exists so that autorepeat on a held modifier key cannot fill the peek
       window and hide a real character behind it. Its safety argument was "it drops
       nothing readable, because a raw-mode ReadFile discards those records anyway", and
       that sentence is true only if the vt_translated membership above is exactly right.
       Making the flush depend on a hand-written list is the shape that produced this
       card's cycle-1 defects: a virtual key missing from the list would be classified as
       yielding nothing, consumed here, and the keystroke destroyed. Without the flush the
       same miss costs one keystroke of latency instead, because the record stays queued
       and the next ordinary character makes the probe answer 1 and the following read
       returns the translation first.

       So the flush consumes only (a) records that are not KEY_EVENT, (b) key-up records,
       and (c) key-down records with no character whose virtual key is a bare modifier or
       lock key. That covers the autorepeat hazard exactly, and no record any console mode
       could translate is ever consumed.

       What can actually ARRIVE here, on the console measured at the top of this file, is
       only (a). Conhost drops key-up records and bare-modifier key-down records at write
       time, so cases (b) and (c) never see a record from this console and only case (a) is
       exercised (flush_consumes_leading_mouse_record in the test binary asserts it). They
       stay because the autorepeat hazard D-5 was written against is a report about a real
       keyboard rather than an injection, because a ConPTY-backed terminal may queue what
       conhost drops, and because a clause that consumes nothing costs nothing. Their being
       unreachable through this console is stated here rather than left for a reader to
       discover from a green suite that never reached them. */
    bool record_is_unreadable_in_any_mode(const INPUT_RECORD& r) {
        if (r.EventType != KEY_EVENT) { return true; }
        if (!r.Event.KeyEvent.bKeyDown) { return true; }
        if (r.Event.KeyEvent.uChar.UnicodeChar != 0) { return false; }
        return bare_modifier_or_lock(r.Event.KeyEvent.wVirtualKeyCode);
    }

    /* Consume the LEADING run of records the scan proved cannot yield a byte. It cannot
       block, because those records are known to be queued. Raw mode only: cooked mode is
       excluded because the console's own line editor consumes non-character records for
       history recall and editing, so eating them there would break console line editing
       (D-5). */
    void flush_leading_unreadable(HANDLE h, const INPUT_RECORD* rec, DWORD got) {
        DWORD run {0};
        while (run < got && record_is_unreadable_in_any_mode(rec[run])) { ++run; }
        while (run > 0) {
            INPUT_RECORD sink[32];
            DWORD want {run < 32 ? run : 32};
            DWORD taken {0};
            if (!ReadConsoleInputW(h, sink, want, &taken) || taken == 0) { return; }
            run -= taken;
        }
    }

    /* The readiness answer for a handle already known to be a real console, given its
       current input mode. */
    int console_ready(HANDLE h, DWORD mode) {
        // Bytes already in hand outrank anything the record queue can say, and they are
        // the only thing that can be true when the queue is empty.
        if (pre_read_pending()) { return 1; }

        /* GetNumberOfConsoleInputEvents first is deliberate (D-3). quesos_idle spins and
           on_input_tick fires every 16384 instructions (src/cpu.cpp:1804), so the idle
           prompt polls this tens of thousands of times per second; the common idle case
           must cost one console call and stop. No rate limiter is added: measure before
           adding one. */
        DWORD pending {0};
        if (!GetNumberOfConsoleInputEvents(h, &pending)) {
            return 1;   // API failure: fail open exactly as the old code did
        }
        if (pending == 0) { return 0; }

        INPUT_RECORD rec[PEEK_CAP];
        DWORD want {pending < PEEK_CAP ? pending : PEEK_CAP};
        DWORD got {0};
        if (!PeekConsoleInputW(h, rec, want, &got) || got == 0) { return 0; }

        if (mode & ENABLE_LINE_INPUT) {
            /* Cooked: the console delivers nothing until the line is complete, so only a
               queued line terminator (Enter, or Ctrl-Z which ends input) makes a read
               non-blocking. Any earlier character is a partial line and is NOT ready. */
            for (DWORD i = 0; i < got; ++i) {
                const INPUT_RECORD& r {rec[i]};
                if (r.EventType != KEY_EVENT || !r.Event.KeyEvent.bKeyDown) { continue; }
                wchar_t ch {r.Event.KeyEvent.uChar.UnicodeChar};
                if (ch == L'\r' || ch == 0x1A) { return 1; }
            }
            /* The cooked peek ceiling, stated as a limit rather than covered by a phrase.
               A full window with no terminator in it reports ready, which sends the guest
               into a read that blocks until the line is finished. That is today's
               behaviour, so cooked mode is improved rather than fixed. The cause is worth
               writing down: PeekConsoleInput only ever peeks from the HEAD of the queue,
               so there is no way to scan past the window without consuming, and a typed
               character queues two records, so PEEK_CAP records is about PEEK_CAP/2 typed
               characters. Raising PEEK_CAP moves the ceiling and does not remove it.
               maize-313 removes it, because a thread that always has a read outstanding
               never has to predict a line terminator at all. */
            return (got == PEEK_CAP) ? 1 : 0;
        }

        for (DWORD i = 0; i < got; ++i) {
            if (record_yields_byte(rec[i], mode)) { return 1; }
        }
        flush_leading_unreadable(h, rec, got);
        return 0;
    }

}  // namespace

bool is_console(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE || h == nullptr) { return false; }
    if (GetFileType(h) != FILE_TYPE_CHAR) { return false; }
    DWORD mode {0};
    return GetConsoleMode(h, &mode) != 0;
}

bool record_yields_byte(const INPUT_RECORD& r, DWORD console_mode) {
    if (r.EventType != KEY_EVENT) { return false; }       // mouse, resize, focus, menu
    if (!r.Event.KeyEvent.bKeyDown) { return false; }     // a key-up yields nothing
    if (r.Event.KeyEvent.uChar.UnicodeChar != 0) { return true; }
    if ((console_mode & ENABLE_VIRTUAL_TERMINAL_INPUT)
        && vt_translated(r.Event.KeyEvent.wVirtualKeyCode)) {
        return true;
    }
    return false;                                          // bare Shift, Ctrl, Alt
}

/* maize-238 (Branch A, decision 9285), rewritten for maize-345. NON-CONSUMING host-stdin
   readiness probe. Windows stdin has no single O_NONBLOCK knob, so dispatch on the handle
   type (the maize-237 console-vs-pipe-vs-file divergence): a pipe uses PeekNamedPipe, a
   redirected file compares position to size, and a real console consults the pre-read
   buffer and its input queue. Returns 1 (a data byte is pending), 0 (nothing pending yet)
   or -1 (end of input).

   The probe consumes nothing except records it has proved cannot yield a byte under any
   console mode. The invariant it protects is not "the probe consumes nothing" but the
   thing that rule was protecting: no byte a data-port read could return is lost or made
   invisible to the probe. */
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
        /* A char handle is either an interactive console or a null-ish device.
           GetConsoleMode succeeds only on a real console (the isatty analog); a
           non-console char device is NUL, whose reads return 0, so it reads as end of
           input and the data port never synthesizes a NUL byte from a zero-length read.
           A serial-port stdin redirect would also land here and would be wrong, but no
           realistic configuration produces one, and it gets this comment rather than
           code (D-7). */
        DWORD mode {0};
        if (!GetConsoleMode(h, &mode)) { return -1; }
        return console_ready(h, mode);
    }
    if (type == FILE_TYPE_DISK) {
        /* These two fail-opens stay (D-7): a disk read cannot block indefinitely, so the
           cost of being wrong here is bounded. */
        LARGE_INTEGER pos, size, zero;
        zero.QuadPart = 0;
        if (!SetFilePointerEx(h, zero, &pos, FILE_CURRENT)) { return 1; }
        if (!GetFileSizeEx(h, &size)) { return 1; }
        return (pos.QuadPart < size.QuadPart) ? 1 : -1;   // bytes remaining vs EOF
    }
    /* Unknown handle type: neither pipe, char nor disk, which no realistic stdin source
       produces. This has the same fail-open shape as the console defect this card fixes
       and it stays, because both alternatives are worse: 0 hangs a guest forever and -1
       fabricates an EOF and loses data (D-7). */
    return 1;
}

long read_console(HANDLE h, unsigned char* buf, unsigned long count) {
    if (count == 0) { return 0; }

    if (g_pre_pos == g_pre_len) {
        /* Empty: the ONE place a console ReadFile is ever issued. Filling from the PROBE
           instead would consume before any guest asked, which is the strand shape D-2
           forbids; looping the fill until a short return would eagerly issue the very
           blocking read this card exists to remove. */
        g_pre_pos = 0;
        g_pre_len = 0;
        g_fill_was_full = false;   // cleared BEFORE the fill, so it cannot outlive one read
        DWORD got {0};
        if (!ReadFile(h, g_pre, PREREAD_CAP, &got, nullptr)) {
            /* A console at end of input surfaces as a zero-byte read rather than a
               failure, but fold the redirect-shaped EOF errors here too so this function
               has the same EOF contract as the pipe path in src/sys.cpp. */
            DWORD err {GetLastError()};
            if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) { return 0; }
            return -1;
        }
        g_pre_len = got;
        g_fill_was_full = (got == PREREAD_CAP);
        if (got == 0) { return 0; }   // end of input (Ctrl-Z in cooked mode)
    }

    unsigned long avail {g_pre_len - g_pre_pos};
    unsigned long n {count < avail ? count : avail};
    std::memcpy(buf, g_pre + g_pre_pos, n);
    g_pre_pos += n;
    return static_cast<long>(n);
}

}  // namespace console_probe
}  // namespace maize

#endif  // _WIN32
