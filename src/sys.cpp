#include "maize.h"
// #include "maize_sys.h"

/* This is all very broken right now, but I'm going to replace it with a sys_call architecture instead. */

namespace maize {
    namespace syscall {
#ifdef __linux__ 
        namespace {
            void _init() {
            }

            void _exit() {
            }
        }

        u_word read(u_word fd, void* buf, u_word count) {
            return ::read(fd, buf, count);
        }

        u_word write(u_word fd, const void* buf, u_word count) {
            return ::write(fd, buf, count);
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
                        return (u_word)-1;
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
                return (u_word)-1;
            }

            return -1;
        }

        u_word write(u_word fd, const void* buf, u_word count) {
            if (fd == 0) {
                return (u_word)-1;
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
                        return (u_word)-1;
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

            return static_cast<u_word>(-1);
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
        }

        int exit_code() {
            return exit_status;
        }

        void init() {
            syscall::_init();
        }

        u_word call(u_qword syscall_id) {
            using namespace cpu;

            switch (syscall_id) {
                /* sys_read */
                case 0x0000U: {
                    u_word fd {regs::r0.w0};
                    u_word address {regs::r1.w0};
                    u_word count {regs::r2.w0};

                    std::vector<u_byte> buf;
                    buf.resize(count);
                    u_byte* bufvec = &buf[0];
                    u_word retval = syscall::read(fd, reinterpret_cast<void*>(bufvec), count);

                    for (u_word i = 0; i < count; ++i, ++address) {
                        cpu::mm.write_byte(address, bufvec[i]);
                    }

                    break;
                }

                /* sys_write */
                case 0x0001U: {
                    u_word fd {regs::r0.w0};
                    u_word address {regs::r1.w0};
                    u_word count {regs::r2.w0};

                    std::vector<u_byte> str = mm.read(address, count);
                    u_byte const* buf {&str[0]};
                    return syscall::write(fd, buf, count);
                }

                /* sys_puts: Maize-native string-write ($F0-$FF is the Maize-native
                   convenience band that does not mirror Linux syscall numbers).
                   Write a NUL-terminated string to fd R0 without a caller-supplied
                   length: scan from R1 for the first $00 byte, then write the bytes
                   before it (the NUL is not written; no implicit trailing newline)
                   through the same host writer sys_write uses, so fd handling,
                   short-write behavior, and the (u_word)-1 error return are identical
                   to sys_write by construction. RV.w0 receives write()'s return. */
                case 0x00F0U: {
                    u_word fd {regs::r0.w0};
                    u_word address {regs::r1.w0};

                    /* length = count of bytes before the terminating $00 (C strlen
                       semantics: no cap; an unterminated string is guest UB). */
                    u_word length {0};
                    while (mm.read_byte(address + length) != 0x00U) {
                        ++length;
                    }

                    /* Empty string: write nothing, return 0. The early return also
                       guards against the &vec[0]-on-empty-vector UB the sys_write
                       path would hit at count 0. */
                    if (length == 0) {
                        return 0;
                    }

                    std::vector<u_byte> str = mm.read(address, length);
                    u_byte const* buf {&str[0]};
                    return syscall::write(fd, buf, length);
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

                /* sys_reboot */
                case 0x00A9U: {
                    break;
                }
            }

            return 0;
        }

        void exit() {
            syscall::_exit();
        }

    } // namespace sys 

} // namespace maize

