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

        u_word read(u_word fd, void* buf, u_hword count) {
            return ::read(fd, buf, count);
        }

        u_word write(u_word fd, const void* buf, u_hword count) {
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

        u_word read(u_word fd, void* buf, u_hword count) {
            if (fd == 0) {
                DWORD chars_read {0};
                ReadConsole(hStdin, buf, count, &chars_read, nullptr);
                return chars_read;
            }

            if (fd == 1 || fd == 2) {
                return (u_word)-1;
            }

            return -1;
        }

        u_word write(u_word fd, const void* buf, u_hword count) {
            if (fd == 0) {
                return (u_word)-1;
            }

            if (fd < 3) {
                HANDLE hStdHandle {hStdout};

                if (fd == 2) {
                    hStdHandle = hStderr;
                }

                DWORD charsWritten {0};
                WriteConsole(hStdHandle, buf, count, &charsWritten, nullptr);
                return charsWritten;
            }

            return static_cast<u_word>(-1);
        }

#else 
// Future expansion to other systems

#endif

    }

    namespace sys {

        void init() {
            syscall::_init();
        }

        u_word call(u_qword syscall_id) {
            using namespace cpu;

            switch (syscall_id) {
                /* sys_read */
                case 0x0000U: {
                    u_word fd {regs::g.w0};
                    u_word address {regs::h.w0};
                    u_hword count {regs::j.h0};
                    size_t s = count;

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
                    u_word fd {regs::g.w0};
                    u_word address {regs::h.w0};
                    u_hword count {regs::j.h0};

                    std::vector<u_byte> str = mm.read(address, count);
                    u_byte const* buf {&str[0]};
                    return syscall::write(fd, buf, count);
                }

                /* sys_exit */
                case 0x003CU: {
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

