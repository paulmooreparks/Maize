#include "maize.h"
// #include "maize_sys.h"
#include "hostfs/hostfs_core.h"
#include <cerrno>
#include <chrono>
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
                    u_word fd {regs::r0.h0};
                    u_word address {regs::r1.w0};
                    u_word count {regs::r2.w0};

                    std::vector<u_byte> buf;
                    buf.resize(count);
                    u_byte* bufvec = buf.data();

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
                    u_word fd {regs::r0.h0};
                    u_word address {regs::r1.w0};
                    u_word count {regs::r2.w0};

                    std::vector<u_byte> str = mm.read(address, count);
                    u_byte const* buf {str.data()};

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
                    u_word fd {regs::r0.h0};
                    int64_t rc = hostfs_close(hostfs, static_cast<int>(fd));
                    return static_cast<u_word>(rc);
                }

                /* maize-114 sys_fstat: fd R0.H0, statbuf R1. The core composes the
                   144-byte section-2 struct stat image; copy it out to guest memory
                   only on success. -EBADF on an unknown fd. */
                case 0x0005U: {
                    u_word fd {regs::r0.h0};
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
                    u_word fd {regs::r0.h0};
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
                    u_word fd {regs::r0.h0};
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
            }

            return 0;
        }

        void exit() {
            syscall::_exit();
        }

    } // namespace sys 

} // namespace maize

