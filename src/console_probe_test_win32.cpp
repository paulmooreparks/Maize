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
    case_nul_device();

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
