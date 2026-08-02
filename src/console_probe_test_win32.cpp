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

/* ---- queue observation ---------------------------------------------------------------
   The record queue is a different observable from the byte stream, and cycle 2 shipped a
   claim about the first while measuring only the second. These two helpers are what makes
   the queue observable: GetNumberOfConsoleInputEvents answers how many records are
   waiting, and PeekConsoleInputW answers what they are. Neither consumes anything and
   neither can block, so a case or a measurement may call them freely.

   Read the counterexamples doc's Entry 35 before writing a sentence about the console into
   a comment: name the API call whose return value the sentence is about, and check that
   the instrument calls it. */
DWORD queued_records() {
    DWORD n {0};
    if (!GetNumberOfConsoleInputEvents(g_in, &n)) { return static_cast<DWORD>(-1); }
    return n;
}

/* Print every record currently queued, one line each. Peeks only. The count goes out
   unindented so a caller can emit a label first; the per-record lines take the indent. */
void dump_queue(const char* indent) {
    DWORD pending {queued_records()};
    if (pending == static_cast<DWORD>(-1)) {
        emitf("GetNumberOfConsoleInputEvents failed, error %lu\n", GetLastError());
        return;
    }
    emitf("%lu record(s) queued\n", pending);
    if (pending == 0) { return; }
    INPUT_RECORD rec[64];
    DWORD got {0};
    if (!PeekConsoleInputW(g_in, rec, 64, &got)) {
        emitf("%sPeekConsoleInputW failed, error %lu\n", indent, GetLastError());
        return;
    }
    for (DWORD i = 0; i < got; ++i) {
        const INPUT_RECORD& r {rec[i]};
        if (r.EventType == KEY_EVENT) {
            const KEY_EVENT_RECORD& k {r.Event.KeyEvent};
            emitf("%s  [%lu] KEY down=%d vk=0x%02X sc=0x%02X uChar=0x%04X\n",
                  indent, i, k.bKeyDown ? 1 : 0, k.wVirtualKeyCode, k.wVirtualScanCode,
                  static_cast<unsigned>(k.uChar.UnicodeChar));
        } else {
            emitf("%s  [%lu] EventType=%u (not a KEY_EVENT)\n", indent, i, r.EventType);
        }
    }
}

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
   mode. This is the criterion that stops the fix recreating the bug in a subtler form by
   counting non-byte records as pending.

   Every row asserts TWO things, and the first of them is why. A row that only asserted the
   probe's answer would pass vacuously wherever conhost discards the record before it ever
   reaches the queue, and three rows here are in exactly that position: a key-up record and
   a bare-modifier key-down are dropped at WRITE time, so the probe returns 0 from its
   `pending == 0` early exit without the classifier being consulted at all. A green line
   there proves nothing about the predicate. So each row also states how many records the
   console kept, which is a real observable with a real way to fail, and the predicate's own
   coverage lives in case_classifier_unit below, where the records are synthesized and the
   console cannot drop them.

   The key-up row carries a NON-ZERO UnicodeChar on purpose, so that when the classifier IS
   consulted (in the unit case, or on a console that queues key-ups) a filter keying on the
   character alone and ignoring bKeyDown fails it. */
void case_classification() {
    struct row {
        const char*  name;
        INPUT_RECORD rec;
        int          want_queued;   // records the console keeps for this injection
        int          want;          // the probe's answer
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
        { "cls_keydown_with_char_is_ready",        char_rec(L'a'),            1, 1 },
        { "cls_keyup_dropped_at_write_time",       key_rec(false, 'A', L'a'), 0, 0 },
        { "cls_modifier_keydown_dropped_at_write_time",
                                                   key_rec(true, VK_SHIFT, 0), 0, 0 },
        { "cls_mouse_event_not_ready",             mouse,                     1, 0 },
        { "cls_window_size_event_not_ready",       resize,                    1, 0 },
        { "cls_focus_event_not_ready",             focus,                     1, 0 },
        { "cls_menu_event_not_ready",              menu,                      1, 0 },
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
        // Before the probe: the probe's raw branch may consume this record (D-5's flush).
        DWORD queued {queued_records()};
        int got {console_probe::stdin_ready(g_in)};
        disarm();
        if (got == r.want && queued == static_cast<DWORD>(r.want_queued)) {
            pass(r.name);
        } else {
            char d[160];
            std::snprintf(d, sizeof d,
                "expected probe %d with %d record(s) queued, got probe %d with %lu queued",
                r.want, r.want_queued, got, queued);
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

/* The 25 members of vt_translated, with the byte sequence --measure recorded for each.
   Shared by the console-level drain cases and by the predicate-level unit cases, so the
   two can never disagree about what the set contains. */
struct vt_member { WORD vk; const char* name; const char* bytes; };

const vt_member kVtMembers[] = {
    { VK_LEFT,   "VK_LEFT",   "\x1B[D" },     { VK_RIGHT,  "VK_RIGHT",  "\x1B[C" },
    { VK_UP,     "VK_UP",     "\x1B[A" },     { VK_DOWN,   "VK_DOWN",   "\x1B[B" },
    { VK_HOME,   "VK_HOME",   "\x1B[H" },     { VK_END,    "VK_END",    "\x1B[F" },
    { VK_PRIOR,  "VK_PRIOR",  "\x1B[5~" },    { VK_NEXT,   "VK_NEXT",   "\x1B[6~" },
    { VK_INSERT, "VK_INSERT", "\x1B[2~" },    { VK_DELETE, "VK_DELETE", "\x1B[3~" },
    { VK_F1,     "VK_F1",     "\x1BOP" },     { VK_F2,     "VK_F2",     "\x1BOQ" },
    { VK_F3,     "VK_F3",     "\x1BOR" },     { VK_F4,     "VK_F4",     "\x1BOS" },
    { VK_F5,     "VK_F5",     "\x1B[15~" },   { VK_F6,     "VK_F6",     "\x1B[17~" },
    { VK_F7,     "VK_F7",     "\x1B[18~" },   { VK_F8,     "VK_F8",     "\x1B[19~" },
    { VK_F9,     "VK_F9",     "\x1B[20~" },   { VK_F10,    "VK_F10",    "\x1B[21~" },
    { VK_F11,    "VK_F11",    "\x1B[23~" },   { VK_F12,    "VK_F12",    "\x1B[24~" },
    /* The three control keys measurement put INTO the set, against the reasoning that
       first left them out. VK_BACK yields 7F (the VT DEL convention) rather than the 08 a
       reader would predict. */
    { VK_BACK,   "VK_BACK",   "\x7F" },       { VK_TAB,    "VK_TAB",    "\x09" },
    { VK_RETURN, "VK_RETURN", "\x0D" },
};

/* AC-3 and OQ-9, at the level the predicate actually lives at. record_yields_byte is a
   pure classifier over a peeked record, and the spec exposes it saying it is "testable
   with synthesized records, which is the whole reason it is exposed". Until this case
   existed nothing in the automated suite called it: its only two call sites in this binary
   were inside --diagnostic, which is a manual mode, and every other case drove it through
   stdin_ready against a real console.

   Driving it through the console cannot cover it, for two independent reasons that only
   showed up once the record queue was measured rather than inferred. Conhost DISCARDS some
   record shapes at write time, so those never reach the classifier at all. And conhost
   EXPANDS virtual keys into per-byte records before they queue, so every record it does
   present carries a non-zero UnicodeChar and the classifier answers at its character
   clause, leaving the vt_translated clause below it unreached. The 25-member set was
   therefore untested in both directions while 25 green lines said otherwise.

   These cases synthesize the records and call the classifier directly. No console is
   involved, so no console behaviour can make one of them vacuous, and the set's membership
   is asserted in both directions and under both console modes. */
void case_classifier_unit() {
    const DWORD vt_on {ENABLE_VIRTUAL_TERMINAL_INPUT};
    const DWORD vt_off {0};

    {
        const char* n {"clf_keydown_with_char_yields_byte"};
        INPUT_RECORD r {char_rec(L'a')};
        check(n, console_probe::record_yields_byte(r, vt_on)
                 && console_probe::record_yields_byte(r, vt_off),
              "a key-down carrying a character must yield a byte in either console mode");
    }
    {
        const char* n {"clf_keyup_yields_nothing"};
        INPUT_RECORD r {key_rec(false, 'A', L'a')};
        check(n, !console_probe::record_yields_byte(r, vt_on),
              "a key-up yields nothing even when it carries a character");
    }
    {
        const char* n {"clf_non_key_records_yield_nothing"};
        INPUT_RECORD recs[4];
        for (INPUT_RECORD& r : recs) { std::memset(&r, 0, sizeof r); }
        recs[0].EventType = MOUSE_EVENT;
        recs[1].EventType = WINDOW_BUFFER_SIZE_EVENT;
        recs[2].EventType = FOCUS_EVENT;
        recs[3].EventType = MENU_EVENT;
        bool ok {true};
        for (const INPUT_RECORD& r : recs) {
            if (console_probe::record_yields_byte(r, vt_on)) { ok = false; }
        }
        check(n, ok, "mouse, resize, focus and menu records yield no bytes");
    }
    {
        /* The OQ-7 flush allowlist's case (c) at the predicate level. These are the keys
           whose records conhost drops at write time, so the console can never present one
           to the probe; synthesizing them is the only way to assert the classification. */
        const char* n {"clf_bare_modifiers_yield_nothing"};
        const WORD vks[] = {
            VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL,
            VK_MENU, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN, VK_APPS,
            VK_CAPITAL, VK_NUMLOCK, VK_SCROLL,
        };
        char detail[128] {0};
        for (WORD vk : vks) {
            INPUT_RECORD r {key_rec(true, vk, 0)};
            if (console_probe::record_yields_byte(r, vt_on)) {
                std::snprintf(detail, sizeof detail,
                              "vk 0x%02X is classified as yielding a byte", vk);
                break;
            }
        }
        check(n, detail[0] == 0, detail[0] ? detail : "");
    }
    {
        const char* n {"clf_vt_members_yield_byte_when_vt_on"};
        char detail[128] {0};
        for (const vt_member& m : kVtMembers) {
            INPUT_RECORD r {key_rec(true, m.vk, 0)};
            if (!console_probe::record_yields_byte(r, vt_on)) {
                std::snprintf(detail, sizeof detail, "%s is not in vt_translated", m.name);
                break;
            }
        }
        check(n, detail[0] == 0, detail[0] ? detail : "");
    }
    {
        /* The mode guard, which is what keeps the set out of cooked mode: the console's own
           line editor owns those records there, and D-5 excludes cooked mode from the flush
           for the same reason. */
        const char* n {"clf_vt_members_yield_nothing_when_vt_off"};
        char detail[128] {0};
        for (const vt_member& m : kVtMembers) {
            INPUT_RECORD r {key_rec(true, m.vk, 0)};
            if (console_probe::record_yields_byte(r, vt_off)) {
                std::snprintf(detail, sizeof detail,
                              "%s yields a byte with VT input off", m.name);
                break;
            }
        }
        check(n, detail[0] == 0, detail[0] ? detail : "");
    }
    {
        /* The other direction of the membership: keys that are NOT in the set must stay
           out of it. VK_ESCAPE leads because it is the one control key measurement kept
           out, and an over-inclusive member is the direction that stalls a read. */
        const char* n {"clf_vt_nonmembers_yield_nothing_when_vt_on"};
        const WORD vks[] = {
            VK_ESCAPE, VK_SPACE, VK_PAUSE, VK_SNAPSHOT, VK_SELECT, VK_HELP,
            VK_F13, VK_F14, VK_CLEAR, VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA,
            'A', '0',
        };
        char detail[128] {0};
        for (WORD vk : vks) {
            INPUT_RECORD r {key_rec(true, vk, 0)};
            if (console_probe::record_yields_byte(r, vt_on)) {
                std::snprintf(detail, sizeof detail,
                              "vk 0x%02X is in vt_translated and should not be", vk);
                break;
            }
        }
        check(n, detail[0] == 0, detail[0] ? detail : "");
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
   quoted at the top of src/console_probe_win32.cpp, where conhost expands an arrow key
   into one record per byte at the moment the record is queued. A stranded byte therefore
   shows up here as a SHORT accumulation rather than as a hang, in either console world:
   a peek-only predicate would answer correctly for these records, and it is cooked mode
   where the retained remainder makes the buffer load-bearing (case_cooked_line).

   The comparison is against what an INJECTED record produces, and the real-keypress
   sequence is the corroborating evidence --diagnostic collects, so a ConPTY-versus-conhost
   difference reads as the finding it is rather than as a red test. */
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

/* Every key in vt_translated gets a drain-loop case of its own: probe, one-byte read,
   re-probe, compare against the sequence --measure recorded. Testing the arrows alone
   would leave Home, End, PgUp, PgDn, Ins, Del and the twelve F-keys unexercised.

   What these cases prove, stated precisely, because OQ-8's resolution once claimed more.
   Conhost expands each of these keys into one record per byte when the record is QUEUED,
   so what runs here is the character clause of record_yields_byte over already-expanded
   records, plus the buffer, plus the drain loop. The membership of vt_translated is NOT
   what these cases exercise, because the clause that consults it is never reached: that
   is what case_classifier_unit covers directly and what case_vt_clause_after_mode_switch
   reaches through the console. What these cases DO prove is the byte sequence for each
   key end to end, through the real probe and the real read path, which is what AC-4 asks
   for and what would catch a console whose translation changed. */
void case_vt_coverage() {
    char name[64];
    for (const vt_member& m : kVtMembers) {
        std::snprintf(name, sizeof name, "vt_%s", m.name);
        INPUT_RECORD rec {key_rec(true, m.vk, 0)};
        run_drain_case(name, &rec, 1, m.bytes, static_cast<int>(std::strlen(m.bytes)));
    }

    /* VK_ESCAPE is the control key measurement kept OUT of the set, and the negative is
       asserted rather than left implicit. The assertion is in two parts on purpose: the
       probe's answer alone would pass vacuously, because conhost drops a bare Escape
       key-down at write time and the probe then returns 0 from its `pending == 0` early
       exit without consulting the classifier. The record count is the part that can fail.
       The classifier's own answer for VK_ESCAPE is asserted in case_classifier_unit. */
    {
        const char* n {"vt_VK_ESCAPE_dropped_at_write_time"};
        arm(n);
        set_raw();
        flush_queue();
        INPUT_RECORD esc {key_rec(true, VK_ESCAPE, 0)};
        inject_one(esc);
        DWORD queued {queued_records()};
        int r {console_probe::stdin_ready(g_in)};
        disarm();
        if (r == 0 && queued == 0) { pass(n); }
        else {
            char d[160];
            std::snprintf(d, sizeof d,
                "expected probe 0 with 0 record(s) queued (the console discards it), "
                "got probe %d with %lu queued", r, queued);
            fail(n, d);
        }
        flush_queue();
    }
}

/* OQ-9, reached through the console rather than through a synthesized record: the one
   queue state in which record_yields_byte's vt_translated clause decides the answer.

   A record queued while ENABLE_VIRTUAL_TERMINAL_INPUT was CLEAR keeps its virtual key and
   its zero UnicodeChar, and turning VT input on afterwards does not expand it. quesOS
   reaches this state when a guest switches from cooked to raw with input already queued.
   The clause is consulted here and answers 1.

   This case deliberately does NOT read. The --measure rows labelled vtoff_* record what a
   read does in this state on this console: it consumes the record and produces no byte, so
   a read here would block until the watchdog killed the process. That is the over-inclusive
   direction, it is why OQ-9 is open and gated on Done, and it is why the set's comment in
   src/console_probe_win32.cpp explains the hold rather than claiming the clause is free.
   The case pins the two facts that decide the question, so that a console which behaves
   differently (a ConPTY-backed terminal is the open case) shows up here as a red line
   rather than as a silent change of meaning. */
void case_vt_clause_after_mode_switch() {
    const char* n {"vt_clause_reached_after_cooked_to_raw_switch"};
    arm(n);
    DWORD m {current_mode()};
    m &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT
           | ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
    SetConsoleMode(g_in, m);
    flush_queue();
    INPUT_RECORD left {key_rec(true, VK_LEFT, 0)};
    inject_one(left);

    INPUT_RECORD peeked[4];
    DWORD got {0};
    bool unexpanded {PeekConsoleInputW(g_in, peeked, 4, &got) && got == 1
                     && peeked[0].EventType == KEY_EVENT
                     && peeked[0].Event.KeyEvent.wVirtualKeyCode == VK_LEFT
                     && peeked[0].Event.KeyEvent.uChar.UnicodeChar == 0};

    set_raw();   // VT input on, with the unexpanded record already queued
    bool classified {console_probe::record_yields_byte(peeked[0], current_mode())};
    int r {console_probe::stdin_ready(g_in)};
    disarm();

    if (unexpanded && classified && r == 1) { pass(n); }
    else {
        char d[192];
        std::snprintf(d, sizeof d,
            "expected 1 unexpanded VK_LEFT record (got %lu record(s), unexpanded=%d), "
            "the classifier to reach vt_translated (got %d), and the probe to answer 1 "
            "(got %d)", got, unexpanded ? 1 : 0, classified ? 1 : 0, r);
        fail(n, d);
    }
    flush_queue();
}

/* D-5's flush, asserted rather than described. The probe consumes the leading run of
   records it proved cannot yield a byte under any console mode, and nothing in the suite
   checked that it does. Only case (a) of the allowlist is reachable through this console,
   because conhost drops key-up and bare-modifier records at write time, so a mouse record
   is the instrument.

   Both directions are here. Alone, the record is consumed. Behind a real character, the
   probe answers 1 at the character and consumes nothing, which is the property that keeps
   the flush from eating input that a later read would have returned. */
void case_flush_leading_unreadable() {
    INPUT_RECORD mouse;
    std::memset(&mouse, 0, sizeof mouse);
    mouse.EventType = MOUSE_EVENT;
    mouse.Event.MouseEvent.dwEventFlags = MOUSE_MOVED;

    {
        const char* n {"flush_consumes_leading_mouse_record"};
        arm(n);
        set_raw();
        flush_queue();
        inject_one(mouse);
        int r {console_probe::stdin_ready(g_in)};
        DWORD after {queued_records()};
        disarm();
        if (r == 0 && after == 0) { pass(n); }
        else {
            char d[160];
            std::snprintf(d, sizeof d,
                "expected probe 0 and the record consumed, got probe %d with %lu queued",
                r, after);
            fail(n, d);
        }
        flush_queue();
    }
    {
        const char* n {"flush_spares_records_behind_a_character"};
        arm(n);
        set_raw();
        flush_queue();
        inject_one(mouse);
        INPUT_RECORD a {char_rec(L'a')};
        inject_one(a);
        int r {console_probe::stdin_ready(g_in)};
        DWORD after {queued_records()};
        disarm();
        if (r == 1 && after == 2) { pass(n); }
        else {
            char d[160];
            std::snprintf(d, sizeof d,
                "expected probe 1 with both records still queued, got probe %d with %lu queued",
                r, after);
            fail(n, d);
        }
        unsigned char sink[8];
        arm(n);
        drain(sink, 8);
        disarm();
        flush_queue();
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

/* --measure (OQ-3, OQ-7, OQ-8, OQ-9). Answers two SEPARATE questions per candidate, and
   keeping them separate is the whole point of this instrument's second version.

   Question 1, the records: how many records does injecting this candidate leave queued,
   and what are they? Answered by GetNumberOfConsoleInputEvents and PeekConsoleInputW,
   which read nothing and cannot block. Cycle 2 of this card shipped an answer to this
   question having never made either call, inferring it from question 2 instead, and the
   inference was wrong for raw mode. See the counterexamples doc, Entry 35.

   Question 2, the bytes: what byte sequence does a read produce for this candidate? That
   one needs real reads, and a naive probe-then-read cannot promise they complete for a
   record the console may translate to nothing. The trick is a sentinel. Inject the
   candidate, then inject a plain 'Z' key-down that certainly yields a byte, then read one
   byte at a time until the 'Z' arrives. Every read is guaranteed to complete, because
   there is always at least the sentinel byte left in the console, and whatever arrives
   BEFORE the 'Z' is exactly the candidate's translation. An empty prefix means the console
   translates that record to nothing, which is the answer that keeps a virtual key OUT of
   vt_translated.

   The queue depth is sampled again after every byte, with the sentinel's own records
   subtracted, so the trace says how many of the CANDIDATE's records are still queued. That
   trace is what distinguishes the two worlds this card's spec named. A count that falls in
   step with the bytes is the per-byte-record world, where the expansion happened when the
   record was queued and every byte is peekable. A count that hits zero while bytes are
   still owed is the retained-remainder world, where the console holds bytes no peek can
   see and only a byte buffer can make them visible to readiness.

   Reading is done with ReadFile rather than through read_console, so the measurement is of
   the CONSOLE and not of this card's buffer. */
void measure_bytes(const char* label, bool cooked, DWORD candidate_records) {
    // The sentinel makes every read below non-blocking.
    if (cooked) {
        INPUT_RECORD tail[2] {char_rec(L'Z'), key_rec(true, VK_RETURN, L'\r')};
        inject(tail, 2);
    } else {
        INPUT_RECORD tail[2] {char_rec(L'Z'), key_rec(false, 'Z', L'Z')};
        inject(tail, 2);
    }
    DWORD after_sentinel {queued_records()};
    DWORD sentinel_records {
        (after_sentinel != static_cast<DWORD>(-1) && candidate_records != static_cast<DWORD>(-1)
         && after_sentinel >= candidate_records)
            ? after_sentinel - candidate_records : 0};

    unsigned char got[64];
    DWORD left[64];
    int n {0};
    arm(label);
    for (; n < 64; ++n) {
        unsigned char b {0};
        DWORD rd {0};
        if (!ReadFile(g_in, &b, 1, &rd, nullptr) || rd != 1) { break; }
        got[n] = b;
        DWORD now {queued_records()};
        left[n] = (now != static_cast<DWORD>(-1) && now > sentinel_records)
                      ? now - sentinel_records : 0;
        if (b == 'Z') { ++n; break; }
    }
    disarm();

    /* Trim at the first sentinel byte. A candidate whose own translation contained 0x5A
       would be reported short here, so the residual check below turns that from a silent
       truncation into a labelled one. */
    int prefix {n};
    for (int i = 0; i < n; ++i) {
        if (got[i] == 'Z') { prefix = i; break; }
    }
    char hexbuf[256];
    hex(got, prefix, hexbuf, sizeof hexbuf);

    char trace[192];
    int toff {0};
    trace[0] = 0;
    for (int i = 0; i < prefix && toff + 8 < static_cast<int>(sizeof trace); ++i) {
        toff += std::snprintf(trace + toff, sizeof trace - static_cast<size_t>(toff),
                              "%lu ", left[i]);
    }
    emitf("       %d byte(s): %-24s candidate records left after each read: %s\n",
          prefix, prefix ? hexbuf : "(none)", prefix ? trace : "(no reads)");
    /* The decisive number for a candidate that yielded NO bytes: if its records are gone
       once the sentinel byte has been read, the console consumed them and produced nothing,
       which is the over-inclusive direction the predicate must not take. Inferring that
       from the empty byte row alone is the mistake Entry 35 is about. */
    if (prefix < n) {
        emitf("       candidate records left once the sentinel byte arrived: %lu\n",
              left[prefix]);
    }

    /* Drain the console back to empty so the next measurement starts clean, counting what
       came after the sentinel. In raw mode the sentinel is one byte and its key-up record
       is dropped at write time, so a clean run leaves nothing: a non-zero residual means
       the byte we stopped on was NOT the sentinel, which is the 0x5A collision this trim
       cannot otherwise see. In cooked mode the residual is the line's own tail and is
       expected, so it is not flagged there. */
    int residual {0};
    arm(label);
    if (cooked) {
        /* A cooked read's remainder lives inside the console rather than in the record
           queue, and FlushConsoleInputBuffer does NOT discard it, which the queue-based
           drain below cannot reach. The first version of this loop left it there and it
           surfaced as two phantom bytes at the head of a later row. The sentinel line ends
           with 0x0A, and every byte of it is already owed, so reading through that byte
           terminates and leaves the console genuinely empty. */
        for (int i = 0; i < 64; ++i) {
            unsigned char b {0};
            DWORD rd {0};
            if (!ReadFile(g_in, &b, 1, &rd, nullptr) || rd != 1) { break; }
            ++residual;
            if (b == 0x0A) { break; }
        }
    }
    DWORD pending {0};
    while (GetNumberOfConsoleInputEvents(g_in, &pending) && pending > 0) {
        unsigned char b {0};
        DWORD rd {0};
        if (!ReadFile(g_in, &b, 1, &rd, nullptr) || rd != 1) { break; }
        ++residual;
    }
    disarm();
    if (!cooked && residual > 0) {
        emitf("       AMBIGUOUS: %d byte(s) followed the sentinel, so the row above may be "
              "truncated at a 0x5A inside the candidate's own translation\n", residual);
    }
    flush_queue();
}

/* Inject under the mode being measured, observe the queue, then observe the bytes. */
void measure_one(const char* label, const INPUT_RECORD* recs, DWORD count, bool cooked) {
    if (cooked) { set_cooked(); } else { set_raw(); }
    flush_queue();
    if (!inject(recs, count)) {
        emitf("  %-24s WriteConsoleInputW failed\n", label);
        return;
    }
    emitf("  %-24s ", label);
    dump_queue("       ");
    measure_bytes(label, cooked, queued_records());
}

/* The one queue state on this console in which record_yields_byte's vt_translated clause
   is reachable, and the reason the clause is kept rather than deleted.

   The expansion of a virtual key into a VT byte sequence happens when the record is
   QUEUED, not when it is read, so the console mode in force at queue time decides what the
   queue holds. A record that arrived while ENABLE_VIRTUAL_TERMINAL_INPUT was OFF keeps its
   virtual key and its zero UnicodeChar, and if VT input is switched on before that record
   is read, the predicate has nothing but the virtual key to classify it by. quesOS reaches
   exactly this state whenever a guest switches the console from cooked to raw with input
   already queued, which is the switch AC-13 is built around. */
void measure_vt_off_then_raw(const char* label, WORD vk) {
    DWORD m {current_mode()};
    m &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT
           | ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
    SetConsoleMode(g_in, m);
    flush_queue();
    INPUT_RECORD rec {key_rec(true, vk, 0)};
    if (!inject_one(rec)) {
        emitf("  %-24s WriteConsoleInputW failed\n", label);
        return;
    }
    emitf("  %-24s ", label);
    dump_queue("       ");
    DWORD candidate_records {queued_records()};
    set_raw();   // VT input on, with the record already queued
    measure_bytes(label, false, candidate_records);
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
    { VK_CAPITAL, "VK_CAPITAL" }, { VK_LWIN, "VK_LWIN" }, { VK_NUMLOCK, "VK_NUMLOCK" },
    { VK_SCROLL, "VK_SCROLL" }, { VK_APPS, "VK_APPS" },
    /* The four keys whose translation is a single control character. They are absent
       from vt_translated on the theory that the console puts the character in
       UnicodeChar, so record_yields_byte's character clause already covers them; these
       rows are what turns that theory into a measurement, by injecting each one with
       UnicodeChar 0 and recording what a read produces. */
    { VK_BACK, "VK_BACK" }, { VK_TAB, "VK_TAB" }, { VK_ESCAPE, "VK_ESCAPE" },
    { VK_RETURN, "VK_RETURN" },
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

    emitf("measure: queued with VT input OFF, then read with VT input ON "
          "(the only state where the vt_translated clause is reachable)\n");
    measure_vt_off_then_raw("vtoff_VK_LEFT", VK_LEFT);
    measure_vt_off_then_raw("vtoff_VK_F5", VK_F5);
    measure_vt_off_then_raw("vtoff_VK_BACK", VK_BACK);
    measure_vt_off_then_raw("vtoff_VK_ESCAPE", VK_ESCAPE);
    return 0;
}

int run_cases() {
    if (!own_a_console()) { return 0; }

    CreateThread(nullptr, 0, watchdog_main, nullptr, 0, nullptr);

    case_empty_queue();
    case_classification();
    case_classifier_unit();
    case_drain_raw();
    case_vt_coverage();
    case_vt_clause_after_mode_switch();
    case_flush_leading_unreadable();
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
