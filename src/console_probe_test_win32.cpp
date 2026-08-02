/* maize-345: the Windows console test binary (spec section 6, D-10).

   The failing configuration this card fixes is an interactive console, and the Windows
   CI suite feeds a pipe, so nothing in the tree reached the FILE_TYPE_CHAR branch until
   this binary existed. It reaches it by owning a console of its own: FreeConsole, then
   AllocConsole, then CONIN$ / CONOUT$ opened with CreateFileW. That console is a genuine
   console object (GetFileType returns FILE_TYPE_CHAR, GetConsoleMode succeeds, and
   ReadFile applies the same cooked and raw semantics), so the predicate, the retention
   behaviour and the real blocking read are all exercised for real.

   What injection does NOT prove is which records a real keypress queues, which is the
   question OQ-5 carries; --diagnostic answers that against the INHERITED console and is
   run by hand. --measure is the instrument that produced the expected byte sequences
   asserted below, and it is kept in the binary so they can be re-derived rather than
   trusted.

   This is a test, not a deliverable: it is not installed, not in the Ctrl+Shift+B task,
   and not in the MAIZE_SANITIZE target list. */

#ifdef _WIN32

#include "console_io.h"
#include "console_probe_win32.h"
#include "host_tty.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

using namespace maize;

/* ---- report plumbing ---------------------------------------------------------------
   FreeConsole detaches the process from the console it inherited, so the PASS/FAIL
   report must not be written to a handle that detachment can invalidate. The original
   STD_OUTPUT_HANDLE is captured before FreeConsole; under the test runner that is a
   pipe, which survives, and the report reaches the harness. If it does not survive (a
   by-hand run whose stdout WAS the inherited console), fall back to the allocated
   console's own output handle so the operator still sees the lines. */
HANDLE g_report {INVALID_HANDLE_VALUE};
HANDLE g_in {INVALID_HANDLE_VALUE};
HANDLE g_out {INVALID_HANDLE_VALUE};
int    g_failures {0};
int    g_cases {0};

void emit(const char* s) {
    DWORD n {0};
    DWORD len {static_cast<DWORD>(std::strlen(s))};
    if (g_report != INVALID_HANDLE_VALUE && WriteFile(g_report, s, len, &n, nullptr)) {
        return;
    }
    if (g_out != INVALID_HANDLE_VALUE) {
        WriteFile(g_out, s, len, &n, nullptr);
    }
}

void emitf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    emit(buf);
}

void pass(const char* name) {
    ++g_cases;
    emitf("[PASS] %s\n", name);
}

void fail(const char* name, const char* detail) {
    ++g_cases;
    ++g_failures;
    emitf("[FAIL] %s: %s\n", name, detail);
}

void check(const char* name, bool ok, const char* detail) {
    if (ok) { pass(name); } else { fail(name, detail); }
}

/* ---- watchdog -----------------------------------------------------------------------
   Armed per case with the case name, and it TERMINATES the process rather than joining:
   the thread it guards can be inside a console ReadFile that nothing can interrupt, so a
   join would hang the harness with it. Every operation under test either cannot block or
   has been proved ready by a probe that answered 1, so the expected latency is
   microseconds against this multi-second budget and CI load does not close that margin.
   A watchdog kill IS the failure this card is about, wearing a different hat. */
constexpr DWORD kWatchdogMs {6000};

char             g_case[128] {0};
volatile LONG    g_armed {0};
volatile LONG64  g_deadline {0};

DWORD WINAPI watchdog_main(LPVOID) {
    for (;;) {
        Sleep(25);
        if (InterlockedCompareExchange(&g_armed, 1, 1) != 1) { continue; }
        LONG64 due {InterlockedCompareExchange64(&g_deadline, 0, 0)};
        if (static_cast<LONG64>(GetTickCount64()) < due) { continue; }
        emitf("[FAIL] %s: watchdog fired after %lu ms (a read that was reported ready "
              "blocked, which is this card's own defect)\n", g_case, kWatchdogMs);
        TerminateProcess(GetCurrentProcess(), 3);
    }
}

void arm(const char* name) {
    std::snprintf(g_case, sizeof g_case, "%s", name);
    InterlockedExchange64(&g_deadline,
        static_cast<LONG64>(GetTickCount64()) + static_cast<LONG64>(kWatchdogMs));
    InterlockedExchange(&g_armed, 1);
}

void disarm() { InterlockedExchange(&g_armed, 0); }

/* ---- console helpers ----------------------------------------------------------------- */

/* The raw input mode host_tty::apply_host installs (src/host_tty.cpp), including the
   maize-345 mouse/window mask, so the cases run against the mode the oksh line editor
   actually runs in. */
DWORD raw_mode_from(DWORD orig) {
    DWORD m {orig};
    m &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT
           | ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT);
    m |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    return m;
}

DWORD cooked_mode_from(DWORD orig) {
    DWORD m {orig};
    m &= ~(ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
    m |= ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT;
    return m;
}

DWORD current_mode() {
    DWORD m {0};
    GetConsoleMode(g_in, &m);
    return m;
}

void set_raw()    { SetConsoleMode(g_in, raw_mode_from(current_mode())); }
void set_cooked() { SetConsoleMode(g_in, cooked_mode_from(current_mode())); }

void flush_queue() { FlushConsoleInputBuffer(g_in); }

INPUT_RECORD key_rec(bool down, WORD vk, wchar_t ch) {
    INPUT_RECORD r;
    std::memset(&r, 0, sizeof r);
    r.EventType = KEY_EVENT;
    r.Event.KeyEvent.bKeyDown = down ? TRUE : FALSE;
    r.Event.KeyEvent.wRepeatCount = 1;
    r.Event.KeyEvent.wVirtualKeyCode = vk;
    r.Event.KeyEvent.wVirtualScanCode =
        static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    r.Event.KeyEvent.uChar.UnicodeChar = ch;
    r.Event.KeyEvent.dwControlKeyState = 0;
    return r;
}

INPUT_RECORD char_rec(wchar_t ch) {
    SHORT vks {VkKeyScanW(ch)};
    WORD vk {static_cast<WORD>(vks == -1 ? 0 : (vks & 0xFF))};
    return key_rec(true, vk, ch);
}

bool inject(const INPUT_RECORD* recs, DWORD count) {
    DWORD written {0};
    return WriteConsoleInputW(g_in, recs, count, &written) && written == count;
}

bool inject_one(const INPUT_RECORD& r) { return inject(&r, 1); }

/* The drain loop quesOS actually runs: probe, and while it answers 1 read EXACTLY one
   byte and append it, stopping when the probe answers 0 (src/devices.cpp:62 reads one
   byte per data-port read). Reading with a multi-byte buffer passes in all three
   possible console worlds and proves nothing, which is why cycle 1's version of AC-4
   could not fail. A stranded remainder shows up here as a SHORT accumulation, never as
   a hang, because a read is only ever issued after a probe answered 1. */
int drain(unsigned char* out, int cap) {
    int n {0};
    for (;;) {
        int r {console_probe::stdin_ready(g_in)};
        if (r != 1) { return n; }
        if (n >= cap) { return n; }
        unsigned char b {0};
        long rc {console_probe::read_console(g_in, &b, 1)};
        if (rc <= 0) {
            emitf("       (drain: read_console returned %ld, error %lu)\n", rc, GetLastError());
            return n;
        }
        out[n++] = b;
    }
}

void hex(const unsigned char* p, int n, char* dst, int dstcap) {
    int off {0};
    dst[0] = 0;
    for (int i = 0; i < n && off + 4 < dstcap; ++i) {
        off += std::snprintf(dst + off, static_cast<size_t>(dstcap - off), "%02X ", p[i]);
    }
}

bool bytes_equal(const unsigned char* a, int an, const char* want) {
    int wn {static_cast<int>(std::strlen(want))};
    if (an != wn) { return false; }
    return std::memcmp(a, want, static_cast<size_t>(an)) == 0;
}

/* Every case ends here. AC-13's general invariant is that no probe-side state outlives a
   single guest read, so once a case has drained what it made available the probe MUST
   answer 0. A case that leaves the probe latched at 1 also poisons the next case, so
   this both asserts the invariant and keeps the cases independent. */
void expect_quiescent(const char* name) {
    int r {console_probe::stdin_ready(g_in)};
    if (r == 0) { return; }
    char detail[128];
    std::snprintf(detail, sizeof detail,
        "probe answered %d with everything drained (state outlived the read)", r);
    fail(name, detail);
    // Bounded recovery so the following cases still mean something.
    unsigned char sink[64];
    for (int i = 0; i < 64 && console_probe::stdin_ready(g_in) == 1; ++i) {
        console_probe::read_console(g_in, sink, 1);
    }
    flush_queue();
}

}  // namespace

/* ======================================================================================
   Cases
   ====================================================================================== */

namespace {

/* AC-2 / AC-6. This is the red-first instrument and it runs FIRST, so its line is on the
   record before any later case can trip the watchdog and take the process down. Against
   the pre-fix predicate (`return GetConsoleMode(h, &mode) ? 1 : -1;`) it FAILS, because
   AllocConsole gave this process a real console and the answer is 1 for an empty queue.
   Against the fixed predicate it passes. It also carries half of AC-6: an empty console
   queue must never read as end of input, which is the direction the fix must not
   overshoot into. */
void case_empty_queue() {
    const char* n1 {"empty_raw_reports_not_ready"};
    arm(n1);
    set_raw();
    flush_queue();
    int r {console_probe::stdin_ready(g_in)};
    disarm();
    if (r == 0) { pass(n1); }
    else {
        char d[96];
        std::snprintf(d, sizeof d, "expected 0 (nothing pending), got %d", r);
        fail(n1, d);
    }

    const char* n2 {"empty_cooked_reports_not_ready"};
    arm(n2);
    set_cooked();
    flush_queue();
    r = console_probe::stdin_ready(g_in);
    disarm();
    if (r == 0) { pass(n2); }
    else {
        char d[96];
        std::snprintf(d, sizeof d, "expected 0 (nothing pending), got %d", r);
        fail(n2, d);
    }
}

/* AC-3: record classification, one record per case into an otherwise empty queue, in raw
   mode. The key-up case carries a NON-ZERO UnicodeChar on purpose, so a filter that keys
   on the character alone and ignores bKeyDown fails it. This is the criterion that stops
   the fix recreating the bug in a subtler form by counting non-byte records as pending. */
void case_classification() {
    struct row {
        const char*  name;
        INPUT_RECORD rec;
        int          want;
    };

    INPUT_RECORD mouse;      std::memset(&mouse, 0, sizeof mouse);
    mouse.EventType = MOUSE_EVENT;
    mouse.Event.MouseEvent.dwEventFlags = MOUSE_MOVED;

    INPUT_RECORD resize;     std::memset(&resize, 0, sizeof resize);
    resize.EventType = WINDOW_BUFFER_SIZE_EVENT;
    resize.Event.WindowBufferSizeEvent.dwSize.X = 80;
    resize.Event.WindowBufferSizeEvent.dwSize.Y = 25;

    INPUT_RECORD focus;      std::memset(&focus, 0, sizeof focus);
    focus.EventType = FOCUS_EVENT;
    focus.Event.FocusEvent.bSetFocus = TRUE;

    INPUT_RECORD menu;       std::memset(&menu, 0, sizeof menu);
    menu.EventType = MENU_EVENT;
    menu.Event.MenuEvent.dwCommandId = 0;

    const row rows[] = {
        { "cls_keydown_with_char_is_ready",   char_rec(L'a'),                     1 },
        { "cls_keyup_with_char_not_ready",    key_rec(false, 'A', L'a'),          0 },
        { "cls_modifier_keydown_not_ready",   key_rec(true, VK_SHIFT, 0),         0 },
        { "cls_mouse_event_not_ready",        mouse,                              0 },
        { "cls_window_size_event_not_ready",  resize,                             0 },
        { "cls_focus_event_not_ready",        focus,                              0 },
        { "cls_menu_event_not_ready",         menu,                               0 },
    };

    for (const row& r : rows) {
        arm(r.name);
        set_raw();
        flush_queue();
        if (!inject_one(r.rec)) {
            disarm();
            char d[96];
            std::snprintf(d, sizeof d, "WriteConsoleInputW failed, error %lu", GetLastError());
            fail(r.name, d);
            continue;
        }
        int got {console_probe::stdin_ready(g_in)};
        disarm();
        if (got == r.want) {
            pass(r.name);
        } else {
            char d[96];
            std::snprintf(d, sizeof d, "expected %d, got %d", r.want, got);
            fail(r.name, d);
        }
        // Drain whatever the case made readable so the next case starts clean. Armed,
        // because a drain that blocks is the same defect the cases exist to catch.
        arm(r.name);
        unsigned char sink[32];
        drain(sink, 32);
        disarm();
        flush_queue();
    }
}

/* AC-6, the other half: the NUL device is FILE_TYPE_CHAR but is not a console (its reads
   return 0), so it must still read as end of input rather than being fail-opened into a
   pending byte. The pipe half of AC-6 is covered by the suite's existing piped-stdin
   legs (Invoke-SysreadTest, Invoke-KeyboardTest). */
void case_nul_device() {
    const char* n {"nul_device_reports_eof"};
    arm(n);
    HANDLE nul {CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr)};
    if (nul == INVALID_HANDLE_VALUE) {
        disarm();
        fail(n, "could not open the NUL device");
        return;
    }
    int r {console_probe::stdin_ready(nul)};
    bool console_says_no {console_probe::is_console(nul)};
    CloseHandle(nul);
    disarm();
    if (r == -1 && !console_says_no) { pass(n); }
    else {
        char d[96];
        std::snprintf(d, sizeof d, "expected -1 and is_console false, got %d / %d",
                      r, console_says_no ? 1 : 0);
        fail(n, d);
    }
}

/* AC-4: raw mode delivers every byte of a multi-byte translation, proved with one-byte
   reads and a re-probe between them. The expected sequences come from the --measure run
   quoted at the top of src/console_probe_win32.cpp; the console holds ONE virtual-key
   record and synthesizes the bytes at read time, retaining what it does not deliver where
   no PeekConsoleInput can see it, so a peek-only predicate would report 0 with two bytes
   of an arrow key still readable. The comparison is against what an INJECTED record
   produces, and the real-keypress sequence is the corroborating evidence --diagnostic
   collects, so a ConPTY-versus-conhost difference reads as the finding it is rather than
   as a red test. */
void run_drain_case(const char* name, const INPUT_RECORD* recs, DWORD count,
                    const char* want, int wantlen) {
    arm(name);
    set_raw();
    flush_queue();
    if (!inject(recs, count)) {
        disarm();
        fail(name, "WriteConsoleInputW failed");
        return;
    }
    unsigned char got[64];
    int n {drain(got, static_cast<int>(sizeof got))};
    disarm();

    bool ok {n == wantlen && std::memcmp(got, want, static_cast<size_t>(n)) == 0};
    if (ok) {
        pass(name);
    } else {
        char gothex[192], wanthex[192], d[512];
        hex(got, n, gothex, sizeof gothex);
        hex(reinterpret_cast<const unsigned char*>(want), wantlen, wanthex, sizeof wanthex);
        std::snprintf(d, sizeof d, "drained %d byte(s) [%s], expected %d [%s]",
                      n, gothex, wantlen, wanthex);
        fail(name, d);
    }
    expect_quiescent(name);
    flush_queue();
}

void case_drain_raw() {
    {
        INPUT_RECORD abc[3] {char_rec(L'a'), char_rec(L'b'), char_rec(L'c')};
        run_drain_case("drain_raw_abc", abc, 3, "abc", 3);
    }
    {
        INPUT_RECORD left {key_rec(true, VK_LEFT, 0)};
        run_drain_case("drain_raw_arrow_left", &left, 1, "\x1B[D", 3);
    }
    {
        /* The non-ASCII expectation is DERIVED from the console's input code page rather
           than hard-coded, because maize never calls SetConsoleCP and the session's page
           decides the answer: U+00E9 is one byte (0x82) under 437 and two under 65001. */
        const wchar_t wide[2] {L'\x00E9', 0};
        char want[8] {0};
        int wantlen {WideCharToMultiByte(GetConsoleCP(), 0, wide, 1, want,
                                         static_cast<int>(sizeof want), nullptr, nullptr)};
        if (wantlen <= 0) {
            fail("drain_raw_nonascii", "the input code page cannot encode U+00E9");
        } else {
            INPUT_RECORD e {char_rec(L'\x00E9')};
            run_drain_case("drain_raw_nonascii", &e, 1, want, wantlen);
        }
    }
}

/* OQ-8: every member of the final vt_translated set gets a drain-loop case of its own.
   An over-inclusive member (the predicate claims the console translates a key it does
   not) makes the probe answer 1 and the fill then blocks inside ReadFile, which trips the
   per-case watchdog; an under-inclusive one accumulates nothing here. Testing the arrows
   alone, as cycle 2 left it, would have left Home, End, PgUp, PgDn, Ins, Del and the
   twelve F-keys unexercised, and section 2 names every one of them as a required member
   in the world the measurement found. */
void case_vt_coverage() {
    struct row { WORD vk; const char* name; const char* want; };
    const row rows[] = {
        { VK_LEFT,   "vt_VK_LEFT",   "\x1B[D" },
        { VK_RIGHT,  "vt_VK_RIGHT",  "\x1B[C" },
        { VK_UP,     "vt_VK_UP",     "\x1B[A" },
        { VK_DOWN,   "vt_VK_DOWN",   "\x1B[B" },
        { VK_HOME,   "vt_VK_HOME",   "\x1B[H" },
        { VK_END,    "vt_VK_END",    "\x1B[F" },
        { VK_PRIOR,  "vt_VK_PRIOR",  "\x1B[5~" },
        { VK_NEXT,   "vt_VK_NEXT",   "\x1B[6~" },
        { VK_INSERT, "vt_VK_INSERT", "\x1B[2~" },
        { VK_DELETE, "vt_VK_DELETE", "\x1B[3~" },
        { VK_F1,     "vt_VK_F1",     "\x1BOP" },
        { VK_F2,     "vt_VK_F2",     "\x1BOQ" },
        { VK_F3,     "vt_VK_F3",     "\x1BOR" },
        { VK_F4,     "vt_VK_F4",     "\x1BOS" },
        { VK_F5,     "vt_VK_F5",     "\x1B[15~" },
        { VK_F6,     "vt_VK_F6",     "\x1B[17~" },
        { VK_F7,     "vt_VK_F7",     "\x1B[18~" },
        { VK_F8,     "vt_VK_F8",     "\x1B[19~" },
        { VK_F9,     "vt_VK_F9",     "\x1B[20~" },
        { VK_F10,    "vt_VK_F10",    "\x1B[21~" },
        { VK_F11,    "vt_VK_F11",    "\x1B[23~" },
        { VK_F12,    "vt_VK_F12",    "\x1B[24~" },
    };
    for (const row& r : rows) {
        INPUT_RECORD rec {key_rec(true, r.vk, 0)};
        run_drain_case(r.name, &rec, 1, r.want, static_cast<int>(std::strlen(r.want)));
    }
}

/* Read one screen-buffer row starting at the current cursor line, for the AC-5 echo
   observation. */
void read_screen_row(wchar_t* dst, int cap) {
    dst[0] = 0;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(g_out, &csbi)) { return; }
    COORD at {0, csbi.dwCursorPosition.Y};
    DWORD read {0};
    if (!ReadConsoleOutputCharacterW(g_out, dst, static_cast<DWORD>(cap - 1), at, &read)) {
        return;
    }
    dst[read] = 0;
    // Trim the trailing blanks the console pads a row with.
    for (int i = static_cast<int>(read) - 1; i >= 0 && dst[i] == L' '; --i) { dst[i] = 0; }
}

/* AC-5: cooked mode. A partial line must report NOT ready, because a cooked console
   delivers nothing until the line is complete, so reporting ready there is exactly this
   card's defect wearing cooked clothing; the same line plus Enter must report ready and
   then drain completely, including the terminator the console appends. The echo half is a
   recorded observation rather than a threshold (D-12), so it is printed and not asserted:
   OQ-6 carries the consequence and it is not fixable on this card either way. */
void case_cooked_line() {
    const char* n1 {"cooked_partial_line_not_ready"};
    arm(n1);
    set_cooked();
    flush_queue();
    INPUT_RECORD partial[2] {char_rec(L'a'), char_rec(L'b')};
    inject(partial, 2);
    wchar_t before[128];
    read_screen_row(before, 128);
    int r {console_probe::stdin_ready(g_in)};
    disarm();
    if (r == 0) { pass(n1); }
    else {
        char d[96];
        std::snprintf(d, sizeof d, "expected 0 for a partial line, got %d", r);
        fail(n1, d);
    }

    const char* n2 {"cooked_full_line_ready_and_drains"};
    arm(n2);
    INPUT_RECORD cr {key_rec(true, VK_RETURN, L'\r')};
    inject_one(cr);
    r = console_probe::stdin_ready(g_in);
    if (r != 1) {
        disarm();
        char d[96];
        std::snprintf(d, sizeof d, "expected 1 once Enter was queued, got %d", r);
        fail(n2, d);
    } else {
        unsigned char got[64];
        int n {drain(got, static_cast<int>(sizeof got))};
        wchar_t after[128];
        read_screen_row(after, 128);
        disarm();
        if (n == 4 && std::memcmp(got, "ab\r\n", 4) == 0) {
            pass(n2);
        } else {
            char gothex[192], d[320];
            hex(got, n, gothex, sizeof gothex);
            std::snprintf(d, sizeof d,
                "drained %d byte(s) [%s], expected 4 [61 62 0D 0A]", n, gothex);
            fail(n2, d);
        }
        emitf("       (echo observation, OQ-6: screen row before the read was \"%ls\", "
              "after the read \"%ls\")\n", before, after);
    }
    expect_quiescent(n2);
    flush_queue();
}

/* AC-13: no probe-side state outlives a single guest read, and the cooked-line-then-raw
   sequence in particular reports not ready rather than latching ready for the rest of the
   session. This is the criterion that catches the silent no-op, the defect that survived
   cycle 1 because nothing could detect it. Against the withdrawn cooked-remainder flag it
   fails by either of two routes: a wrong answer at step 6 if the console keeps the cooked
   remainder across the mode switch, or a watchdog kill at step 5 if it drops it. Step 6 is
   the assertion, and no blocking read is needed to detect the failure, because the wrong
   answer IS the failure. */
void case_cooked_then_raw() {
    const char* n {"cooked_then_raw_no_latched_ready"};
    arm(n);
    set_cooked();
    flush_queue();
    INPUT_RECORD line[3] {char_rec(L'a'), char_rec(L'b'), key_rec(true, VK_RETURN, L'\r')};
    if (!inject(line, 3)) {
        disarm();
        fail(n, "WriteConsoleInputW failed");
        return;
    }

    if (console_probe::stdin_ready(g_in) != 1) {
        disarm();
        fail(n, "step 2: the probe did not report ready for a complete cooked line");
        return;
    }

    // Step 3: read exactly two bytes with two single-byte reads, so the console's line
    // remainder is left in the pre-read buffer rather than in the console.
    unsigned char two[2] {0, 0};
    for (int i = 0; i < 2; ++i) {
        if (console_probe::read_console(g_in, &two[i], 1) != 1) {
            disarm();
            fail(n, "step 3: a single-byte read of a line reported ready returned nothing");
            return;
        }
    }
    if (two[0] != 'a' || two[1] != 'b') {
        disarm();
        char d[96];
        std::snprintf(d, sizeof d, "step 3: expected 'a','b', got 0x%02X,0x%02X",
                      two[0], two[1]);
        fail(n, d);
        return;
    }

    // Step 4: switch to raw, clearing ENABLE_LINE_INPUT, with bytes still owed.
    set_raw();

    // Step 5: the probe must still report ready, and the drain loop must deliver the
    // rest of the line one byte at a time. Input queued before a mode switch is
    // delivered after it, which matches the POSIX reference this card brings Windows
    // into line with: a termios change does not discard already-queued input unless the
    // caller asks for a flush.
    unsigned char rest[16];
    int n_rest {drain(rest, static_cast<int>(sizeof rest))};
    disarm();
    if (n_rest != 2 || rest[0] != '\r' || rest[1] != '\n') {
        char resthex[64], d[192];
        hex(rest, n_rest, resthex, sizeof resthex);
        std::snprintf(d, sizeof d,
            "step 5: expected the remaining 2 bytes [0D 0A] after the mode switch, got "
            "%d [%s]", n_rest, resthex);
        fail(n, d);
        flush_queue();
        return;
    }

    // Step 6: the assertion. Record queue empty, buffer drained, so the probe MUST say 0.
    int r {console_probe::stdin_ready(g_in)};
    if (r == 0) {
        pass(n);
    } else {
        char d[128];
        std::snprintf(d, sizeof d,
            "step 6: probe answered %d with the queue empty and everything drained "
            "(readiness latched across the mode switch)", r);
        fail(n, d);
    }
    flush_queue();
}

/* AC-14: the console input-mode mask, verified in BOTH branches. Cycle 1 specified the
   mask and tested nothing, and the cooked branch is where it matters most, because it
   wrote the inherited mode back verbatim.

   The test drives this through host_tty's own entry points rather than a copy of them:
   set an inherited mode that HAS the bits the mask must remove plus several that must
   survive, then host_tty::init() (which captures its original from GetStdHandle, which
   AllocConsole reassigned to this console), then termios_set with a raw image and with a
   cooked one, reading the mode back after each. The final restore() must put the original
   back, unmasked, which also covers the restore path.

   The AC's "every other bit of the original inherited mode is unchanged" is asserted
   literally for the cooked branch. For the raw branch it is asserted as the documented
   raw transform, which additionally clears the three line-discipline bits and sets VT
   input: raw mode changes those by definition, so a literal reading of the clause cannot
   hold there and asserting it would be asserting something false. */
void case_host_tty_mask() {
    const char* n {"host_tty_input_mode_mask"};
    arm(n);

    const DWORD inherited {ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT
                           | ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT
                           | ENABLE_INSERT_MODE | ENABLE_QUICK_EDIT_MODE
                           | ENABLE_EXTENDED_FLAGS};
    if (!SetConsoleMode(g_in, inherited)) {
        disarm();
        fail(n, "could not install the crafted inherited console mode");
        return;
    }

    /* host_tty::init() takes its handles from GetStdHandle. AllocConsole reassigns the
       standard handles only where they were not already redirected, and under the test
       runner this process's stdout IS redirected to a pipe, so point them at the console
       explicitly. The report keeps going to the handle captured before FreeConsole, so
       the harness still sees these lines. */
    HANDLE saved_in {GetStdHandle(STD_INPUT_HANDLE)};
    HANDLE saved_out {GetStdHandle(STD_OUTPUT_HANDLE)};
    SetStdHandle(STD_INPUT_HANDLE, g_in);
    SetStdHandle(STD_OUTPUT_HANDLE, g_out);

    host_tty::init();
    if (!host_tty::active()) {
        disarm();
        SetStdHandle(STD_INPUT_HANDLE, saved_in);
        SetStdHandle(STD_OUTPUT_HANDLE, saved_out);
        fail(n, "host_tty::init() did not see an interactive console");
        return;
    }

    unsigned char image[maize::console::TERMIOS_SIZE];
    host_tty::termios_get(image);

    const DWORD mask {ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT};
    const DWORD want_cooked {inherited & ~mask};
    const DWORD want_raw {(want_cooked
                           & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT))
                          | ENABLE_VIRTUAL_TERMINAL_INPUT};

    // Raw: the guest clears ICANON.
    unsigned lflag {static_cast<unsigned>(image[maize::console::TERMIOS_OFF_LFLAG])
                    | (static_cast<unsigned>(image[maize::console::TERMIOS_OFF_LFLAG + 1]) << 8)};
    image[maize::console::TERMIOS_OFF_LFLAG] =
        static_cast<unsigned char>((lflag & ~maize::console::TERMIOS_ICANON) & 0xFF);
    host_tty::termios_set(image);
    DWORD got_raw {current_mode()};

    // Cooked: the guest sets ICANON again.
    image[maize::console::TERMIOS_OFF_LFLAG] =
        static_cast<unsigned char>((lflag | maize::console::TERMIOS_ICANON) & 0xFF);
    host_tty::termios_set(image);
    DWORD got_cooked {current_mode()};

    host_tty::restore();
    DWORD got_restored {current_mode()};
    SetStdHandle(STD_INPUT_HANDLE, saved_in);
    SetStdHandle(STD_OUTPUT_HANDLE, saved_out);
    disarm();

    bool ok {got_raw == want_raw && got_cooked == want_cooked
             && got_restored == inherited};
    if (ok) {
        pass(n);
    } else {
        char d[384];
        std::snprintf(d, sizeof d,
            "raw 0x%08lX (want 0x%08lX), cooked 0x%08lX (want 0x%08lX), "
            "restored 0x%08lX (want 0x%08lX)",
            got_raw, want_raw, got_cooked, want_cooked, got_restored, inherited);
        fail(n, d);
    }
}

}  // namespace

/* ======================================================================================
   Modes
   ====================================================================================== */

namespace {

bool own_a_console() {
    FreeConsole();
    if (!AllocConsole()) {
        emitf("[SKIP] console_probe (AllocConsole failed, error %lu; this runner has no "
              "console host, so the console legs cannot run here)\n", GetLastError());
        return false;
    }
    g_in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    g_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_in == INVALID_HANDLE_VALUE || g_out == INVALID_HANDLE_VALUE) {
        emitf("[SKIP] console_probe (CONIN$/CONOUT$ could not be opened, error %lu)\n",
              GetLastError());
        return false;
    }
    DWORD probe_mode {0};
    DWORD probe_pending {0};
    BOOL  mode_ok {GetConsoleMode(g_in, &probe_mode)};
    BOOL  count_ok {GetNumberOfConsoleInputEvents(g_in, &probe_pending)};
    emitf("console_probe: allocated console window=%p type=%lu mode_ok=%d mode=0x%08lX "
          "count_ok=%d pending=%lu\n",
          (void*)GetConsoleWindow(), GetFileType(g_in), mode_ok ? 1 : 0, probe_mode,
          count_ok ? 1 : 0, probe_pending);
    return true;
}

/* --measure (OQ-3, OQ-7, OQ-8). Answers "what bytes does a read produce for THIS record"
   without ever risking a blocking read, which a naive probe-then-read measurement cannot
   promise for a record the console may translate to nothing.

   The trick is a sentinel. Inject the candidate record, then inject a plain 'Z' key-down
   that certainly yields a byte, then read one byte at a time until the 'Z' arrives. Every
   read is guaranteed to complete, because there is always at least the sentinel byte left
   in the console, and whatever arrives BEFORE the 'Z' is exactly the candidate's
   translation. An empty prefix means the console translates that record to nothing, which
   is the answer that keeps a virtual key OUT of vt_translated.

   This is the instrument that produced the expected sequences the cases assert. Reading
   is done with ReadFile rather than through read_console, so the measurement is of the
   CONSOLE and not of this card's buffer. */
void measure_one(const char* label, const INPUT_RECORD* recs, DWORD count, bool cooked) {
    if (cooked) { set_cooked(); } else { set_raw(); }
    flush_queue();
    if (!inject(recs, count)) {
        emitf("  %-24s WriteConsoleInputW failed\n", label);
        return;
    }
    if (cooked) {
        INPUT_RECORD tail[2] {char_rec(L'Z'), key_rec(true, VK_RETURN, L'\r')};
        inject(tail, 2);
    } else {
        INPUT_RECORD tail[2] {char_rec(L'Z'), key_rec(false, 'Z', L'Z')};
        inject(tail, 2);
    }

    unsigned char got[64];
    int n {0};
    arm(label);
    for (; n < 64; ++n) {
        unsigned char b {0};
        DWORD rd {0};
        if (!ReadFile(g_in, &b, 1, &rd, nullptr) || rd != 1) { break; }
        got[n] = b;
        if (b == 'Z') { ++n; break; }
    }
    disarm();

    // Trim the sentinel and anything the console appended after it.
    int prefix {n};
    for (int i = 0; i < n; ++i) {
        if (got[i] == 'Z') { prefix = i; break; }
    }
    char hexbuf[256];
    hex(got, prefix, hexbuf, sizeof hexbuf);
    emitf("  %-24s %d byte(s): %s\n", label, prefix, prefix ? hexbuf : "(none)");

    // Drain the console back to empty so the next measurement starts clean.
    DWORD pending {0};
    while (GetNumberOfConsoleInputEvents(g_in, &pending) && pending > 0) {
        unsigned char b {0};
        DWORD rd {0};
        if (!ReadFile(g_in, &b, 1, &rd, nullptr) || rd != 1) { break; }
    }
    flush_queue();
}

struct vk_row { WORD vk; const char* name; };

const vk_row kVkCandidates[] = {
    { VK_LEFT, "VK_LEFT" },   { VK_RIGHT, "VK_RIGHT" }, { VK_UP, "VK_UP" },
    { VK_DOWN, "VK_DOWN" },   { VK_HOME, "VK_HOME" },   { VK_END, "VK_END" },
    { VK_PRIOR, "VK_PRIOR" }, { VK_NEXT, "VK_NEXT" },   { VK_INSERT, "VK_INSERT" },
    { VK_DELETE, "VK_DELETE" },
    { VK_F1, "VK_F1" }, { VK_F2, "VK_F2" }, { VK_F3, "VK_F3" }, { VK_F4, "VK_F4" },
    { VK_F5, "VK_F5" }, { VK_F6, "VK_F6" }, { VK_F7, "VK_F7" }, { VK_F8, "VK_F8" },
    { VK_F9, "VK_F9" }, { VK_F10, "VK_F10" }, { VK_F11, "VK_F11" }, { VK_F12, "VK_F12" },
    { VK_SHIFT, "VK_SHIFT" }, { VK_CONTROL, "VK_CONTROL" }, { VK_MENU, "VK_MENU" },
    { VK_CAPITAL, "VK_CAPITAL" },
};

int run_measure() {
    if (!own_a_console()) { return 0; }
    CreateThread(nullptr, 0, watchdog_main, nullptr, 0, nullptr);

    emitf("measure: input code page %u, output code page %u\n",
          GetConsoleCP(), GetConsoleOutputCP());
    emitf("measure: raw mode (ENABLE_VIRTUAL_TERMINAL_INPUT set), one key-down record per row\n");
    for (const vk_row& r : kVkCandidates) {
        INPUT_RECORD rec {key_rec(true, r.vk, 0)};
        measure_one(r.name, &rec, 1, false);
    }

    emitf("measure: raw mode, character records\n");
    {
        INPUT_RECORD a {char_rec(L'a')};
        measure_one("char_a", &a, 1, false);
        INPUT_RECORD eacute {char_rec(L'\x00E9')};
        measure_one("char_U+00E9", &eacute, 1, false);
        INPUT_RECORD cr {key_rec(true, VK_RETURN, L'\r')};
        measure_one("char_CR_raw", &cr, 1, false);
    }

    emitf("measure: cooked mode (ENABLE_LINE_INPUT set)\n");
    {
        INPUT_RECORD line[3] {char_rec(L'a'), char_rec(L'b'),
                              key_rec(true, VK_RETURN, L'\r')};
        measure_one("cooked_ab_CR", line, 3, true);
    }
    return 0;
}

int run_cases() {
    if (!own_a_console()) { return 0; }

    CreateThread(nullptr, 0, watchdog_main, nullptr, 0, nullptr);

    case_empty_queue();
    case_classification();
    case_drain_raw();
    case_vt_coverage();
    case_cooked_line();
    case_cooked_then_raw();
    case_nul_device();
    // Last: it hands the console to host_tty, which installs its own atexit restore.
    case_host_tty_mask();

    emitf("console_probe: %d passed, %d failed (%d total)\n",
          g_cases - g_failures, g_failures, g_cases);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace

/* --diagnostic (OQ-5, D-10). WriteConsoleInputW proves the predicate classifies records
   correctly and proves NOTHING about which records a real keypress queues, which is
   exactly AC-4's question. This mode therefore skips FreeConsole and AllocConsole
   entirely, attaches to the console it INHERITED, puts it in the raw mode
   host_tty::apply_host would set, and dumps every field of every INPUT_RECORD one typed
   keystroke produces, followed by the bytes a single ReadFile returns for it.

   Run it by hand, once in Windows Terminal (ConPTY-backed, records synthesized by
   decoding VT from the terminal) and once in conhost (classic, records from the keyboard
   driver), because there is no reason to assume the two agree. It runs nothing in CI. */
int run_diagnostic() {
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_report = g_out;
    if (!console_probe::is_console(g_in)) {
        emit("diagnostic: stdin is not a console; run this from a real terminal without "
             "redirection.\n");
        return 2;
    }
    DWORD orig {current_mode()};
    emitf("diagnostic: inherited input mode 0x%08lX, input code page %u\n",
          orig, GetConsoleCP());
    set_raw();
    emitf("diagnostic: raw input mode 0x%08lX\n", current_mode());
    emit("diagnostic: press ONE key now.\n");

    flush_queue();
    for (;;) {
        DWORD pending {0};
        if (!GetNumberOfConsoleInputEvents(g_in, &pending)) { break; }
        if (pending > 0) { break; }
        Sleep(20);
    }

    INPUT_RECORD recs[64];
    DWORD got {0};
    if (!PeekConsoleInputW(g_in, recs, 64, &got)) {
        emit("diagnostic: PeekConsoleInputW failed\n");
        SetConsoleMode(g_in, orig);
        return 2;
    }
    emitf("diagnostic: %lu record(s) queued for that keypress\n", got);
    for (DWORD i = 0; i < got; ++i) {
        const INPUT_RECORD& r {recs[i]};
        if (r.EventType == KEY_EVENT) {
            const KEY_EVENT_RECORD& k {r.Event.KeyEvent};
            emitf("  [%lu] KEY_EVENT bKeyDown=%d wRepeatCount=%u wVirtualKeyCode=0x%02X "
                  "wVirtualScanCode=0x%02X UnicodeChar=U+%04X AsciiChar=0x%02X "
                  "dwControlKeyState=0x%08lX yields_byte=%d\n",
                  i, k.bKeyDown ? 1 : 0, k.wRepeatCount, k.wVirtualKeyCode,
                  k.wVirtualScanCode, static_cast<unsigned>(k.uChar.UnicodeChar),
                  static_cast<unsigned char>(k.uChar.AsciiChar), k.dwControlKeyState,
                  console_probe::record_yields_byte(r, current_mode()) ? 1 : 0);
        } else {
            emitf("  [%lu] EventType=%u (not a KEY_EVENT) yields_byte=%d\n",
                  i, r.EventType,
                  console_probe::record_yields_byte(r, current_mode()) ? 1 : 0);
        }
    }
    emitf("diagnostic: probe answers %d with those records queued\n",
          console_probe::stdin_ready(g_in));

    unsigned char buf[64];
    DWORD rd {0};
    if (ReadFile(g_in, buf, sizeof buf, &rd, nullptr)) {
        char hexbuf[256];
        hex(buf, static_cast<int>(rd), hexbuf, sizeof hexbuf);
        emitf("diagnostic: one ReadFile returned %lu byte(s): %s\n", rd, hexbuf);
    } else {
        emitf("diagnostic: ReadFile failed, error %lu\n", GetLastError());
    }
    SetConsoleMode(g_in, orig);
    return 0;
}

int main(int argc, char** argv) {
    g_report = GetStdHandle(STD_OUTPUT_HANDLE);
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--diagnostic") == 0) { return run_diagnostic(); }
        if (std::strcmp(argv[i], "--measure") == 0) { return run_measure(); }
    }
    return run_cases();
}

#else

int main() { return 0; }

#endif  // _WIN32
