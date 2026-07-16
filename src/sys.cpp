#include "maize.h"
// #include "maize_sys.h"
#include "console_io.h"
#include "host_tty.h"
#include "hostfs/hostfs_core.h"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

/* This is all very broken right now, but I'm going to replace it with a sys_call architecture instead. */

namespace maize {
    namespace syscall {
        /* maize-75: Linux-numbered errno values, HARD-CODED so a guest sees the
           same code on every host (operator decision 7397). The bad-fd cases are
           detected here and returned as -EBADF directly rather than deferring to
           the host kernel; only a real host I/O failure on an in-scope stdio fd
           differs (Linux passes its own errno through verbatim, which is
           numerically identical to the ABI; Windows synthesizes -EIO). */
        namespace {
            constexpr int abi_ebadf {9};
            constexpr int abi_eio {5};

            /* Fold an errno code into the frozen [-4095,-1] result band. */
            inline u_word neg_errno(int e) {
                return static_cast<u_word>(-static_cast<long>(e));
            }
        }

#ifdef __linux__
        namespace {
            void _init() {
            }

            void _exit() {
            }
        }

        u_word read(u_word fd, void* buf, u_word count) {
            if (fd == 1 || fd == 2) {
                return neg_errno(abi_ebadf);   // write-only stdio fds
            }
            if (fd >= 3) {
                return neg_errno(abi_ebadf);   // real file I/O is out of scope (M4)
            }
            long r = ::read(static_cast<int>(fd), buf, count);
            if (r < 0) {
                // Host Linux errno is numerically identical to the Maize ABI.
                return neg_errno(errno);
            }
            return static_cast<u_word>(r);
        }

        u_word write(u_word fd, const void* buf, u_word count) {
            if (fd == 0) {
                return neg_errno(abi_ebadf);   // stdin, read-only
            }
            if (fd >= 3) {
                return neg_errno(abi_ebadf);   // real file I/O is out of scope (M4)
            }
            long r = ::write(static_cast<int>(fd), buf, count);
            if (r < 0) {
                return neg_errno(errno);
            }
            return static_cast<u_word>(r);
        }
#elif _WIN32
        namespace {
            std::mutex ctrl_mutex;
            std::mutex io_mutex;
            std::counting_semaphore<16> io_set {0};
            std::condition_variable io_run_event;
            std::condition_variable io_close_event;

            HANDLE hStdin {INVALID_HANDLE_VALUE};
            HANDLE hStdout {INVALID_HANDLE_VALUE};
            HANDLE hStderr {INVALID_HANDLE_VALUE};
            DWORD fdwSaveOldMode;
            CONSOLE_SCREEN_BUFFER_INFO csbiInfo {0};

            VOID ErrorExit(LPCWSTR lpszMessage) {
                fwprintf(stderr, L"%s\n", lpszMessage);
                // Restore input mode on exit.
                SetConsoleMode(hStdin, fdwSaveOldMode);
                ExitProcess(0);
            }

            VOID KeyEventProc(KEY_EVENT_RECORD ker) {
                printf("Key event: ");

                if (ker.bKeyDown) {
                    printf("key pressed\n");
                }
                else {
                    printf("key released\n");
                }
            }

#ifndef MOUSE_HWHEELED
#define MOUSE_HWHEELED 0x0008
#endif

            VOID MouseEventProc(MOUSE_EVENT_RECORD mer) {
                wprintf(L"Mouse event: ");

                switch (mer.dwEventFlags) {
                    case 0: {
                        if (mer.dwButtonState == FROM_LEFT_1ST_BUTTON_PRESSED) {
                            wprintf(L"left button press \n");
                        }
                        else if (mer.dwButtonState == RIGHTMOST_BUTTON_PRESSED) {
                            wprintf(L"right button press \n");
                        }
                        else {
                            wprintf(L"button press\n");
                        }

                        break;
                    }

                    case DOUBLE_CLICK: {
                        wprintf(L"double click\n");
                        break;
                    }

                    case MOUSE_HWHEELED: {
                        wprintf(L"horizontal mouse wheel\n");
                        break;
                    }

                    case MOUSE_MOVED: {
                        wprintf(L"mouse moved\n");
                        break;
                    }

                    case MOUSE_WHEELED: {
                        wprintf(L"vertical mouse wheel\n");
                        break;
                    }

                    default: {
                        wprintf(L"unknown\n");
                        break;
                    }
                }
            }

            VOID ResizeEventProc(WINDOW_BUFFER_SIZE_RECORD wbsr) {
                wprintf(L"Resize event\n");
                wprintf(L"Console screen buffer is %d columns by %d rows.\n", wbsr.dwSize.X, wbsr.dwSize.Y);
            }

            void console_input_thread() {
                DWORD cNumRead {0};
                DWORD fdwMode {0};
                DWORD i {0};
                INPUT_RECORD irInBuf[128] {0};

                // Get the standard input handle.

                hStdin = GetStdHandle(STD_INPUT_HANDLE);

                if (hStdin == INVALID_HANDLE_VALUE) {
                    ErrorExit(L"GetStdHandle");
                }

                // Save the current input mode, to be restored on exit.

                if (!GetConsoleMode(hStdin, &fdwSaveOldMode)) {
                    ErrorExit(L"GetConsoleMode");
                }

                // Enable the window and mouse input events.

                fdwMode = ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT;

                if (!SetConsoleMode(hStdin, fdwMode)) {
                    ErrorExit(L"SetConsoleMode");
                }

                while (true) {
                    // Wait for the events.

                    if (!ReadConsoleInput(hStdin, irInBuf, 128, &cNumRead)) {
                        ErrorExit(L"ReadConsoleInput");
                    }

                    // Dispatch the events to the appropriate handler.

                    for (i = 0; i < cNumRead; i++) {
                        switch (irInBuf[i].EventType) {
                            case KEY_EVENT: // keyboard input
                                KeyEventProc(irInBuf[i].Event.KeyEvent);
                                break;

                            case MOUSE_EVENT: // mouse input
                                MouseEventProc(irInBuf[i].Event.MouseEvent);
                                break;

                            case WINDOW_BUFFER_SIZE_EVENT: // scrn buf. resizing
                                ResizeEventProc(irInBuf[i].Event.WindowBufferSizeEvent);
                                break;

                            case FOCUS_EVENT:  // disregard focus events
                                break;

                            case MENU_EVENT:   // disregard menu events
                                break;

                            default:
                                ErrorExit(L"Unknown event type");
                                break;
                        }
                    }
                }

                // Restore input mode on exit.

                SetConsoleMode(hStdin, fdwSaveOldMode);
            }

            void _init() {
                hStdin = GetStdHandle(STD_INPUT_HANDLE);
                // TODO: error handling
                hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
                // TODO: error handling
                hStderr = GetStdHandle(STD_ERROR_HANDLE);
                // TODO: error handling

                DWORD dwMode {0};

                if (!GetConsoleMode(hStdout, &dwMode)) {
                    // TODO: error handling
                }

                dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;

                if (!SetConsoleMode(hStdout, dwMode)) {
                    // TODO: error handling
                }
            }

            void _exit() {

            }
        }

        // ReadFile/WriteFile take a 32-bit DWORD length, so a 64-bit count above
        // 0xFFFFFFFF cannot be passed in one call. Chunk the transfer in <=DWORD
        // slices (capped at chunk_max) and accumulate the total transferred so the
        // full 64-bit count is honored rather than silently truncated at the WinAPI
        // boundary. fd 0/1/2 stdio never short-transfers at these sizes.
        namespace {
            constexpr u_word chunk_max {0x7FFFFFFF};
        }

        u_word read(u_word fd, void* buf, u_word count) {
            if (fd == 0) {
                u_byte* cursor {reinterpret_cast<u_byte*>(buf)};
                u_word total_read {0};

                while (count > 0) {
                    DWORD this_chunk {static_cast<DWORD>(count < chunk_max ? count : chunk_max)};
                    DWORD bytes_read {0};

                    if (!ReadFile(hStdin, cursor, this_chunk, &bytes_read, nullptr)) {
                        // A redirected stdin (anonymous pipe, or a regular file
                        // read past its end on some Windows versions) surfaces
                        // EOF as a FAILED ReadFile call with ERROR_BROKEN_PIPE or
                        // ERROR_HANDLE_EOF, unlike POSIX read() which returns 0
                        // on EOF without an error. Fold both into the same
                        // zero-bytes-read EOF the ABI promises on every host
                        // (maize-75); any OTHER failure still synthesizes the
                        // ABI I/O-failure code.
                        DWORD err {GetLastError()};
                        if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) {
                            break;
                        }
                        return neg_errno(abi_eio);
                    }

                    total_read += bytes_read;

                    if (bytes_read < this_chunk) {
                        // Short read (end of input); stop rather than spin.
                        break;
                    }

                    cursor += bytes_read;
                    count -= bytes_read;
                }

                return total_read;
            }

            if (fd == 1 || fd == 2) {
                return neg_errno(abi_ebadf);   // write-only stdio fds
            }

            return neg_errno(abi_ebadf);       // real file I/O is out of scope (M4)
        }

        u_word write(u_word fd, const void* buf, u_word count) {
            if (fd == 0) {
                return neg_errno(abi_ebadf);   // stdin, read-only
            }

            if (fd < 3) {
                HANDLE hStdHandle {hStdout};

                if (fd == 2) {
                    hStdHandle = hStderr;
                }

                u_byte const* cursor {reinterpret_cast<u_byte const*>(buf)};
                u_word total_written {0};

                while (count > 0) {
                    DWORD this_chunk {static_cast<DWORD>(count < chunk_max ? count : chunk_max)};
                    DWORD bytes_written {0};

                    if (!WriteFile(hStdHandle, cursor, this_chunk, &bytes_written, nullptr)) {
                        // No host errno; synthesize the ABI I/O-failure code.
                        return neg_errno(abi_eio);
                    }

                    total_written += bytes_written;

                    if (bytes_written < this_chunk) {
                        // Short write; stop rather than spin.
                        break;
                    }

                    cursor += bytes_written;
                    count -= bytes_written;
                }

                return total_written;
            }

            return neg_errno(abi_ebadf);       // real file I/O is out of scope (M4)
        }

#else 
// Future expansion to other systems

#endif

    }

    namespace sys {

        namespace {
            /* Single source of truth for the process exit status. Default 0
               (a program that ends via HALT records no code); written only by
               SYS $3C (sys_exit). maize.cpp's main reads it via exit_code()
               after cpu::run() returns and returns it as the host process's
               own exit status. */
            int exit_status = 0;

            /* maize-75: brk heap bookkeeping. Maize memory is sparse and lazily
               zero-filled, so brk allocates nothing; it moves a break value
               between heap_base (the floor, = align_up(end-of-image,16)) and
               HEAP_CEILING, enforcing those bounds. init_heap seeds both from
               the loaded image's high-water mark before the process-start block
               is built. */
            u_word heap_base = 0;
            u_word current_brk = 0;

            /* Fixed ceiling (operator decision 7396). Reserves the top address
               region below TOP (0xFFFFFFFFFFFFFFF8) for the descending stack; a
               brk request above this returns the current break unchanged, giving
               sbrk/malloc (maize-76) a real, detectable failure mode. */
            constexpr u_word HEAP_CEILING = 0xFFFFFFFF00000000ull;

            /* maize-114: the active hostfs mount table, threaded in from maize.cpp
               after it parses the --mount / --mount-home grants. NULL means hostfs
               is inert (mazm never parses mounts; a bare `maize` with no grant sets
               an empty table so the synthetic root is visible but holds no entries).
               The open/close/fstat/lseek/getdents64 numbers and the fd >= 3 routing
               of read/write dispatch against this table. */
            hostfs_table* hostfs = nullptr;

            /* maize-140: the bound window text console, or NULL (default / mazm / no
               display). When non-NULL, fd 0/1/2 and the termios syscalls route here
               instead of host stdio. */
            console::console_io* console_dev = nullptr;

            /* maize-114: bounded guest-C-string copy-in (maize-79 trust boundary).
               Reads NUL-terminated bytes from guest memory at addr, capped at
               HOSTFS_PATH_MAX. Returns true on a NUL within the bound; false when
               the bound is hit first (the caller then returns -ENAMETOOLONG). */
            bool copy_in_path(u_word addr, std::string& out) {
                out.clear();
                for (u_word i = 0; i < HOSTFS_PATH_MAX; ++i) {
                    u_byte b = cpu::mm.read_byte(addr + i);
                    if (b == 0) {
                        return true;
                    }
                    out.push_back(static_cast<char>(b));
                }
                return false;
            }

            /* maize-141: monotonic-clock baseline, captured once at sys::init(). SYS
               $F0 reports milliseconds since VM start; steady_clock is monotonic
               (never wall-clock-adjusted), so the result is non-decreasing by
               construction. */
            std::chrono::steady_clock::time_point clock_baseline;
        }

        /* maize-114: install the parsed mount table (maize.cpp calls this once at
           startup). Resetting the fd table here keeps a reused process image clean. */
        void set_hostfs_table(hostfs_table* table) {
            hostfs = table;
            hostfs_reset_fds();
        }

        /* maize-140: bind (or clear) the window text console. */
        void set_console(console::console_io* c) {
            console_dev = c;
        }

        int exit_code() {
            return exit_status;
        }

        void init_heap(u_word image_end) {
            /* align_up(image_end, 16): the heap floor sits on a 16-byte boundary
               at or above the last byte of the loaded image. */
            u_word aligned = (image_end + 15u) & ~static_cast<u_word>(15);
            heap_base = aligned;
            current_brk = aligned;
        }

        void init() {
            clock_baseline = std::chrono::steady_clock::now();
            syscall::_init();
        }

        u_word call(u_qword syscall_id) {
            using namespace cpu;

            switch (syscall_id) {
                /* sys_read */
                case 0x0000U: {
                    /* fd is a 32-bit C `int` carried in R0.H0 (the Maize C ABI
                       materializes it there, e.g. `CP $01 R0.H0`), so read the
                       low-32 subregister; address/count stay full-64 (maize-56). */
                    u_word fd {regs::r0.h0()};
                    u_word address {regs::r1.w0};
                    u_word count {regs::r2.w0};

                    std::vector<u_byte> buf;
                    buf.resize(count);
                    u_byte* bufvec = buf.data();

                    /* maize-140: when the window console is bound, fd 0 reads decoded
                       keystrokes through its termios line discipline (blocking read that
                       parks the CPU on empty input); the host-side buffer is then copied
                       to guest memory exactly like the stdio path below. fd 1/2 remain
                       write-only. */
                    if (console_dev != nullptr && fd == 0) {
                        long rc = console_dev->read_in(bufvec, count);
                        u_word n = static_cast<u_word>(rc);
                        for (u_word i = 0; i < n; ++i) {
                            cpu::mm.write_byte(address + i, bufvec[i]);
                        }
                        return n;
                    }

                    /* maize-114: fd >= 3 routes through the hostfs guest fd table
                       (lifting the M4 -EBADF restriction) when a mount table is
                       installed; fds 0/1/2 stay the stdio reservations. */
                    u_word retval;
                    if (fd >= 3 && hostfs != nullptr) {
                        int64_t rc = hostfs_read(hostfs, static_cast<int>(fd),
                            reinterpret_cast<void*>(bufvec), count);
                        retval = static_cast<u_word>(rc);
                    } else {
                        retval = syscall::read(fd, reinterpret_cast<void*>(bufvec), count);
                    }

                    /* retval in [-4095,-1] is an -errno result (same predicate
                       __syscall_ret uses): return it and write nothing to guest
                       memory rather than spilling the buffer. */
                    if (retval > static_cast<u_word>(-4096)) {
                        return retval;
                    }

                    /* maize-228: host-side kill escape (Ctrl-] x3) on the real terminal's raw
                       input. Only the console binary's host-stdio fd 0 path (no bound window
                       console) and only in raw mode; check_kill_escape self-gates. On trigger:
                       restore the terminal and stop the VM, so a wedged raw guest can be killed
                       from the same terminal (raw Ctrl-C is just a byte to the guest). */
                    if (fd == 0 && console_dev == nullptr
                        && host_tty::check_kill_escape(bufvec, retval)) {
                        host_tty::restore();
                        std::cerr << "\r\nmaize: terminated by host escape (Ctrl-] x3)\r\n";
                        cpu::power_off();
                    }

                    /* Copy only the bytes actually read (retval), not the full
                       requested count; a short read must not spill the
                       uninitialized buffer tail. EOF (retval == 0) copies
                       nothing. */
                    for (u_word i = 0; i < retval; ++i) {
                        cpu::mm.write_byte(address + i, bufvec[i]);
                    }

                    return retval;
                }

                /* sys_write */
                case 0x0001U: {
                    /* fd is a 32-bit C `int` in R0.H0 (see sys_read); read the
                       low-32 subregister so a stale upper half from a reused
                       register cannot masquerade as an out-of-range fd. */
                    u_word fd {regs::r0.h0()};
                    u_word address {regs::r1.w0};
                    u_word count {regs::r2.w0};

                    std::vector<u_byte> str = mm.read(address, count);
                    u_byte const* buf {str.data()};

                    /* maize-140: when the window console is bound, fd 1/2 render as
                       glyphs through the VT-output engine instead of host stdout/stderr;
                       the full count is always consumed. */
                    if (console_dev != nullptr && (fd == 1 || fd == 2)) {
                        console_dev->write_out(buf, count);
                        return count;
                    }

                    /* maize-114: fd >= 3 routes through the hostfs guest fd table
                       (a :ro mount returns -EROFS from the core); fds 0/1/2 stay
                       the stdio reservations. */
                    if (fd >= 3 && hostfs != nullptr) {
                        int64_t rc = hostfs_write(hostfs, static_cast<int>(fd), buf, count);
                        return static_cast<u_word>(rc);
                    }
                    return syscall::write(fd, buf, count);
                }

                /* maize-114 sys_open: path R0 (full-64 pointer), flags R1, mode R2.
                   Copy the path in (bounded), then let the hostfs core prefix-resolve
                   the mount, apply the :ro write-intent gate, and confine+open via the
                   backend. Returns the guest fd or -errno. */
                case 0x0002U: {
                    u_word address {regs::r0.w0};
                    int flags {static_cast<int>(regs::r1.w0)};
                    int mode {static_cast<int>(regs::r2.w0)};

                    std::string path;
                    if (!copy_in_path(address, path)) {
                        return static_cast<u_word>(-static_cast<long>(HOSTFS_ENAMETOOLONG));
                    }
                    int64_t rc = hostfs_open(hostfs, path.c_str(), flags, mode);
                    return static_cast<u_word>(rc);
                }

                /* maize-114 sys_close: fd R0.H0. Frees the guest fd and closes the
                   backend handle. -EBADF on an unknown/closed fd. */
                case 0x0003U: {
                    u_word fd {regs::r0.h0()};
                    int64_t rc = hostfs_close(hostfs, static_cast<int>(fd));
                    return static_cast<u_word>(rc);
                }

                /* maize-114 sys_fstat: fd R0.H0, statbuf R1. The core composes the
                   144-byte section-2 struct stat image; copy it out to guest memory
                   only on success. -EBADF on an unknown fd. */
                case 0x0005U: {
                    u_word fd {regs::r0.h0()};
                    u_word statbuf {regs::r1.w0};

                    uint8_t img[HOSTFS_STAT_SIZE];
                    int64_t rc = hostfs_fstat(hostfs, static_cast<int>(fd), img);
                    if (rc < 0) {
                        return static_cast<u_word>(rc);
                    }
                    for (u_word i = 0; i < HOSTFS_STAT_SIZE; ++i) {
                        cpu::mm.write_byte(statbuf + i, img[i]);
                    }
                    return static_cast<u_word>(rc);
                }

                /* maize-114 sys_lseek: fd R0.H0, offset R1 (s64), whence R2. Returns
                   the new offset, -EINVAL on a bad whence / negative result, -EBADF on
                   an unknown fd. */
                case 0x0008U: {
                    u_word fd {regs::r0.h0()};
                    int64_t offset {static_cast<int64_t>(regs::r1.w0)};
                    int whence {static_cast<int>(regs::r2.w0)};
                    int64_t rc = hostfs_lseek(hostfs, static_cast<int>(fd), offset, whence);
                    return static_cast<u_word>(rc);
                }

                /* maize-114 sys_getdents64: fd R0.H0, dirp R1, count R2. The core (or
                   backend) packs linux_dirent64 records into a host buffer; copy out
                   only the bytes written. Returns bytes / 0 at EOF / -EINVAL (buffer
                   too small for one record) / -EBADF (unknown fd). */
                case 0x00D9U: {
                    u_word fd {regs::r0.h0()};
                    u_word dirp {regs::r1.w0};
                    u_word count {regs::r2.w0};

                    std::vector<u_byte> buf;
                    buf.resize(count);
                    int64_t rc = hostfs_getdents(hostfs, static_cast<int>(fd),
                        buf.data(), count);
                    if (rc <= 0) {
                        return static_cast<u_word>(rc);   /* EOF (0) or -errno */
                    }
                    for (u_word i = 0; i < static_cast<u_word>(rc); ++i) {
                        cpu::mm.write_byte(dirp + i, buf[i]);
                    }
                    return static_cast<u_word>(rc);
                }

                /* maize-151 sys_rename: oldpath R0, newpath R1. Copy both paths in
                   (bounded), then let the hostfs core normalize BOTH against the cwd,
                   require them to land in the same rw mount (else -EXDEV / -EROFS), and
                   confine each remainder beneath the mount anchor before the backend
                   rename. Returns 0 or -errno; DOOM renames temp.dsg to the save slot. */
                case 0x0052U: {
                    u_word oldaddr {regs::r0.w0};
                    u_word newaddr {regs::r1.w0};

                    std::string oldp;
                    std::string newp;
                    if (!copy_in_path(oldaddr, oldp) || !copy_in_path(newaddr, newp)) {
                        return static_cast<u_word>(-static_cast<long>(HOSTFS_ENAMETOOLONG));
                    }
                    int64_t rc = hostfs_rename(hostfs, oldp.c_str(), newp.c_str());
                    return static_cast<u_word>(rc);
                }

                /* maize-151 sys_mkdir: path R0, mode R1. Copy the path in (bounded),
                   then let the hostfs core normalize + mount-resolve + write-gate it and
                   confine the remainder beneath the mount anchor before the backend
                   mkdir. Returns 0 or -errno; DOOM creates ./.savegame before a save. */
                case 0x0053U: {
                    u_word address {regs::r0.w0};
                    int mode {static_cast<int>(regs::r1.w0)};

                    std::string path;
                    if (!copy_in_path(address, path)) {
                        return static_cast<u_word>(-static_cast<long>(HOSTFS_ENAMETOOLONG));
                    }
                    int64_t rc = hostfs_mkdir(hostfs, path.c_str(), mode);
                    return static_cast<u_word>(rc);
                }

                /* maize-151 sys_unlink: path R0. Copy the path in (bounded), then let the
                   hostfs core normalize + mount-resolve + write-gate it and confine the
                   remainder beneath the mount anchor before the backend unlink. Returns 0
                   or -errno. */
                case 0x0057U: {
                    u_word address {regs::r0.w0};

                    std::string path;
                    if (!copy_in_path(address, path)) {
                        return static_cast<u_word>(-static_cast<long>(HOSTFS_ENAMETOOLONG));
                    }
                    int64_t rc = hostfs_unlink(hostfs, path.c_str());
                    return static_cast<u_word>(rc);
                }

                /* maize-179 sys_ftruncate: fd R0.H0 (a 32-bit C int, read via the low-32
                   subregister like sys_write), length R1 (signed 64-bit). Only real files
                   opened through the hostfs table (fd >= 3) can be truncated; the core
                   applies the :ro / synthetic-root write-gate (EROFS) and the negative-
                   length check (EINVAL) before the backend resize. The stdio reservations
                   0/1/2 (host stdio or the window console) are not regular files, so
                   truncating them is EINVAL, matching Linux ftruncate on a pipe/tty. */
                case 0x004DU: {
                    u_word fd {regs::r0.h0()};
                    int64_t length {static_cast<int64_t>(regs::r1.w0)};

                    if (fd >= 3 && hostfs != nullptr) {
                        int64_t rc = hostfs_ftruncate(hostfs, static_cast<int>(fd), length);
                        return static_cast<u_word>(rc);
                    }
                    return static_cast<u_word>(-static_cast<long>(HOSTFS_EINVAL));
                }

                /* sys_exit: record main's status and terminate the VM. The exit
                   code is the first integer argument in R0 (same ABI slot fd
                   uses for sys_read/sys_write). We record the low 8 bits only;
                   Maize does not clamp or error on larger values (return 256 is
                   observed as 0), matching the host's 8-bit process-status
                   truncation. power_off() stops the VM so cpu::run() returns
                   rather than blocking on int_event.wait(); this syscall never
                   returns to the program, so its return value is immaterial. */
                case 0x003CU: {
                    exit_status = static_cast<int>(regs::r0.w0 & 0xFFU);
                    cpu::power_off();
                    break;
                }

                /* sys_brk (maize-75): move the heap break. R0 = requested new
                   break (0 queries). Returns the current (possibly unchanged)
                   break and NEVER returns -errno: the returned break is a low
                   address that cannot collide with the [-4095,-1] band, so brk is
                   exempt from the error convention (operator decisions 7392/7396).
                   A request below heap_base or above HEAP_CEILING leaves the break
                   unchanged, the enforced failure mode sbrk/malloc detects
                   (maize-76). Because Maize memory is sparse and lazily
                   zero-filled, no allocation happens here; only the break value
                   moves. */
                case 0x000CU: {
                    u_word requested {regs::r0.w0};

                    if (requested == 0) {
                        return current_brk;             // query idiom
                    }
                    if (requested < heap_base) {
                        return current_brk;             // below the floor: unchanged
                    }
                    if (requested > HEAP_CEILING) {
                        return current_brk;             // over the ceiling: unchanged
                    }

                    current_brk = requested;            // valid: set and return the new break
                    return current_brk;
                }

                /* sys_reboot */
                case 0x00A9U: {
                    break;
                }

                /* sys_clock_ms (maize-141): monotonic ms since VM start. RV = elapsed
                   ms (steady_clock). Reads no argument registers. Exempt from the
                   -errno convention (cf. sys_brk): the value cannot land in
                   [-4095,-1] in any realistic runtime. */
                case 0x00F0U: {
                    auto now = std::chrono::steady_clock::now();
                    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - clock_baseline).count();
                    return static_cast<u_word>(ms);
                }

                /* sys_tcgetattr (maize-140): fd R0.H0, termios* R1. Copy the bound
                   console's termios wire image (console::TERMIOS_SIZE bytes) out to guest
                   memory. Maize-private high-block number ($F1; termios has no clean
                   Linux-number mirror since Linux does it through ioctl TCGETS). Returns 0
                   on success, -EBADF if no console is bound (host stdio has no termios) or
                   the fd is not a tty. */
                case 0x00F1U: {
                    u_word fd {regs::r0.h0()};
                    u_word address {regs::r1.w0};
                    if (fd > 2) {
                        return static_cast<u_word>(-static_cast<long>(9));  /* -EBADF */
                    }
                    unsigned char img[console::TERMIOS_SIZE];
                    if (console_dev != nullptr) {
                        console_dev->termios_get(img);   /* windowed text console */
                    } else if (host_tty::active()) {
                        host_tty::termios_get(img);       /* maize-228: real host terminal */
                    } else {
                        return static_cast<u_word>(-static_cast<long>(9));  /* -EBADF: non-tty host stdio */
                    }
                    for (u_word i = 0; i < console::TERMIOS_SIZE; ++i) {
                        cpu::mm.write_byte(address + i, img[i]);
                    }
                    return 0;
                }

                /* sys_tcsetattr (maize-140): fd R0.H0, optional_actions R1 (accepted and
                   applied immediately; TCSANOW/TCSADRAIN/TCSAFLUSH are equivalent for this
                   line discipline), termios* R2. Adopt the guest's termios wire image.
                   Returns 0 on success, -EBADF if no console is bound / not a tty. */
                case 0x00F2U: {
                    u_word fd {regs::r0.h0()};
                    u_word address {regs::r2.w0};
                    if (fd > 2) {
                        return static_cast<u_word>(-static_cast<long>(9));  /* -EBADF */
                    }
                    if (console_dev == nullptr && !host_tty::active()) {
                        return static_cast<u_word>(-static_cast<long>(9));  /* -EBADF: non-tty host stdio */
                    }
                    unsigned char img[console::TERMIOS_SIZE];
                    for (u_word i = 0; i < console::TERMIOS_SIZE; ++i) {
                        img[i] = cpu::mm.read_byte(address + i);
                    }
                    if (console_dev != nullptr) {
                        console_dev->termios_set(img);   /* windowed text console */
                    } else {
                        host_tty::termios_set(img);       /* maize-228: real host terminal */
                    }
                    return 0;
                }

                /* sys_ttysize (maize-228): fd R0.H0, struct winsize* R1 (ws_row, ws_col,
                   ws_xpixel, ws_ypixel; four u16). Fills the row/col fields from the REAL host
                   terminal so a guest's ioctl(TIOCGWINSZ) succeeds on the console binary and
                   never falls back to the ESC[6n cursor-probe (which stalls a cooked read).
                   Maize-private high-block number ($F6), no Linux mirror. Returns 0 on success,
                   -ENOTTY (25) when there is no host terminal size (windowed console or non-tty
                   host stdio -> the guest keeps its ESC[6n fallback, which the windowed console
                   answers). */
                case 0x00F6U: {
                    u_word fd {regs::r0.h0()};
                    u_word address {regs::r1.w0};
                    if (fd > 2 || console_dev != nullptr || !host_tty::active()) {
                        return static_cast<u_word>(-static_cast<long>(25));  /* -ENOTTY */
                    }
                    unsigned short rows = 0, cols = 0;
                    if (!host_tty::get_winsize(&rows, &cols)) {
                        return static_cast<u_word>(-static_cast<long>(25));  /* -ENOTTY */
                    }
                    /* struct winsize little-endian: ws_row @0, ws_col @2 (u16 each). */
                    cpu::mm.write_byte(address + 0, static_cast<u_byte>(rows & 0xFF));
                    cpu::mm.write_byte(address + 1, static_cast<u_byte>((rows >> 8) & 0xFF));
                    cpu::mm.write_byte(address + 2, static_cast<u_byte>(cols & 0xFF));
                    cpu::mm.write_byte(address + 3, static_cast<u_byte>((cols >> 8) & 0xFF));
                    cpu::mm.write_byte(address + 4, 0);   /* ws_xpixel */
                    cpu::mm.write_byte(address + 5, 0);
                    cpu::mm.write_byte(address + 6, 0);   /* ws_ypixel */
                    cpu::mm.write_byte(address + 7, 0);
                    return 0;
                }

                /* sys_palette_blit (maize-213): a pure indexed 32-bit blit
                   dst[i] = lut[src[i]] over sparse guest memory, at host memcpy
                   speed. Hoists DOOM's per-frame cmap_to_fb 8bpp -> XRGB8888
                   palette convert (~400-500K guest instructions/frame) out of the
                   interpreter. Maize-private high-block number ($F3, no Linux
                   mirror), adjacent to $F0 clock / $F1/$F2 termios; a NEW SYS
                   number, NOT an ISA/encoding change.

                   Args (Maize C ABI, R0..R3, full-64 subregisters):
                     R0 = dst      (guest ptr, XRGB8888 destination)
                     R1 = src      (guest ptr, 8bpp source indices)
                     R2 = lut      (guest ptr, uint32[256] baked XRGB8888 LUT)
                     R3 = npixels  (u64 pixel count)
                   RV = npixels on success, or a [-4095,-1] -errno on a bounds
                   violation (NO guest write on rejection). The gamma + RGB pack
                   is baked guest-side in I_SetPalette, so the result is byte-
                   identical to cmap_to_fb by construction.

                   SECURITY (deny-by-default, load-bearing): the guest address
                   space is sparse and lazily zero-filled, so there is no raw host
                   OOB deref; the real risks a boundary-crossing syscall must deny
                   are (a) a 64-bit base+len wrap letting a crafted ptr+count touch
                   unrelated blocks and (b) an enormous npixels driving unbounded
                   host CPU / block allocation. Both are rejected BEFORE any memory
                   access, mirroring the framebuffer-present idiom (devices.cpp:
                   base == 0 || base + size < base). */
                case 0x00F3U: {
                    constexpr u_word MAX_BLIT_PIXELS {1u << 24};  /* 16,777,216 (up to 4096x4096) */
                    constexpr long abi_einval {22};
                    constexpr long abi_efault {14};

                    u_word dst_addr {regs::r0.w0};
                    u_word src_addr {regs::r1.w0};
                    u_word lut_addr {regs::r2.w0};
                    u_word npixels  {regs::r3.w0};

                    if (npixels == 0) {
                        return 0;                                  /* benign no-op */
                    }
                    if (npixels > MAX_BLIT_PIXELS) {
                        return static_cast<u_word>(-abi_einval);   /* oversized request */
                    }
                    /* base+len wrap checks. npixels <= 2^24 so npixels*4 < 2^26 and
                       the multiply itself cannot overflow u64. */
                    if (src_addr + npixels < src_addr) {
                        return static_cast<u_word>(-abi_efault);   /* src range wraps */
                    }
                    if (lut_addr + 256u * 4u < lut_addr) {
                        return static_cast<u_word>(-abi_efault);   /* lut range wraps */
                    }
                    if (dst_addr + npixels * 4u < dst_addr) {
                        return static_cast<u_word>(-abi_efault);   /* dst range wraps */
                    }

                    /* Reusable scratch buffers grown on demand: the steady state
                       has ZERO per-frame allocation (honors the maize-201 no-per-
                       frame-alloc posture). Do NOT allocate a fresh vector per call. */
                    static thread_local std::vector<u_byte>   src_scratch;
                    static thread_local std::vector<uint32_t> out_scratch;
                    if (src_scratch.size() < npixels) { src_scratch.resize(npixels); }
                    if (out_scratch.size() < npixels) { out_scratch.resize(npixels); }

                    /* Read the 256-entry LUT and the src indices at host speed.
                       On a little-endian host the raw bytes preserve values, so no
                       swap (the fb-present path inherits the same LE invariant). */
                    uint32_t lut[256];
                    cpu::mm.read_into(lut_addr, reinterpret_cast<u_byte*>(lut), 256u * 4u);
                    cpu::mm.read_into(src_addr, src_scratch.data(), npixels);

                    const u_byte* src8 {src_scratch.data()};
                    uint32_t* out32 {out_scratch.data()};
                    for (u_word i = 0; i < npixels; ++i) {
                        out32[i] = lut[src8[i]];
                    }

                    cpu::mm.write_from(dst_addr, reinterpret_cast<const u_byte*>(out32),
                        static_cast<size_t>(npixels) * 4u);
                    return npixels;
                }

                /* sys_bulk_copy (maize-216): copy n bytes src -> dst over sparse
                   guest memory at host memcpy speed, hoisting the large-n case of
                   the guest RT memcpy/memmove out of the interpreter (~14 host
                   cycles/guest-byte through the word loop -> host memcpy speed).
                   The boot/general-path companion to $F3's per-frame blit; a NEW
                   Maize-private SYS number ($F4, no Linux mirror), NOT an ISA change.

                   Args (Maize C ABI, R0..R2, full-64 subregisters):
                     R0 = dst  (guest ptr)
                     R1 = src  (guest ptr)
                     R2 = n    (u64 byte count)
                   RV = n on success, or a [-4095,-1] -errno on a bounds violation
                   (NO guest write on rejection).

                   memmove semantics FOR FREE: the whole src range is read into a
                   host scratch buffer BEFORE any byte is written to dst, so
                   overlapping ranges are correct in both directions. This one
                   syscall therefore serves both memcpy and memmove.

                   SECURITY (deny-by-default, load-bearing): mirrors $F3. The guest
                   space is sparse and lazily zero-filled, so there is no raw host
                   OOB deref; the risks a boundary-crossing syscall must deny are
                   (a) a 64-bit base+len wrap letting a crafted ptr+count touch
                   unrelated blocks and (b) an enormous n driving unbounded host
                   CPU / block allocation. Both are rejected BEFORE any access. */
                case 0x00F4U: {
                    constexpr u_word MAX_BULK_BYTES {1u << 28};  /* 256 MiB */
                    constexpr long abi_einval {22};
                    constexpr long abi_efault {14};

                    u_word dst_addr {regs::r0.w0};
                    u_word src_addr {regs::r1.w0};
                    u_word n        {regs::r2.w0};

                    if (n == 0) {
                        return 0;                                 /* benign no-op */
                    }
                    if (n > MAX_BULK_BYTES) {
                        return static_cast<u_word>(-abi_einval);  /* oversized request */
                    }
                    if (src_addr + n < src_addr) {
                        return static_cast<u_word>(-abi_efault);  /* src range wraps */
                    }
                    if (dst_addr + n < dst_addr) {
                        return static_cast<u_word>(-abi_efault);  /* dst range wraps */
                    }

                    /* Reusable scratch grown on demand: zero per-call allocation in
                       steady state (honors the maize-201 no-per-frame-alloc posture). */
                    static thread_local std::vector<u_byte> bulk_scratch;
                    if (bulk_scratch.size() < n) { bulk_scratch.resize(n); }

                    cpu::mm.read_into(src_addr, bulk_scratch.data(), n);
                    cpu::mm.write_from(dst_addr, bulk_scratch.data(), n);
                    return n;
                }

                /* sys_bulk_set (maize-216): fill n bytes at dst with a single byte
                   value, hoisting the large-n case of the guest RT memset (BSS
                   zeroing, buffer clears) into the host. Maize-private ($F5, no
                   Linux mirror), NOT an ISA change.

                   Args (Maize C ABI):
                     R0 = dst    (guest ptr)
                     R1 = value  (low 8 bits used, like C memset's int c)
                     R2 = n      (u64 byte count)
                   RV = n on success, or a [-4095,-1] -errno on a bounds violation
                   (NO guest write on rejection). Same deny-by-default checks as $F4
                   (no src range to validate). */
                case 0x00F5U: {
                    constexpr u_word MAX_BULK_BYTES {1u << 28};  /* 256 MiB */
                    constexpr long abi_einval {22};
                    constexpr long abi_efault {14};

                    u_word dst_addr {regs::r0.w0};
                    u_byte value    {static_cast<u_byte>(regs::r1.w0 & 0xFFu)};
                    u_word n        {regs::r2.w0};

                    if (n == 0) {
                        return 0;                                 /* benign no-op */
                    }
                    if (n > MAX_BULK_BYTES) {
                        return static_cast<u_word>(-abi_einval);  /* oversized request */
                    }
                    if (dst_addr + n < dst_addr) {
                        return static_cast<u_word>(-abi_efault);  /* dst range wraps */
                    }

                    static thread_local std::vector<u_byte> bulk_set_scratch;
                    if (bulk_set_scratch.size() < n) { bulk_set_scratch.resize(n); }

                    std::memset(bulk_set_scratch.data(), value, static_cast<size_t>(n));
                    cpu::mm.write_from(dst_addr, bulk_set_scratch.data(), n);
                    return n;
                }
            }

            return 0;
        }

        void exit() {
            syscall::_exit();
        }

    } // namespace sys 

} // namespace maize

