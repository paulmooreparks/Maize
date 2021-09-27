#include "maize.h"
// #include "maize_sys.h"

/* This is all very broken right now, but I'm going to replace it with a sys_call architecture instead. */

namespace maize {
    namespace sys {

        void init() {
            syscall::init();
        }

        word call(qword syscall_id) {
            using namespace cpu;

            switch (syscall_id) {
                /* sys_read */
                case 0x0000U: {
                    word fd {regs::g.w0};
                    word address {regs::h.w0};
                    hword count {regs::j.h0};

                    std::vector<byte> str = mm.read(address, count);
                    byte* buf {&str[0]};
                    return syscall::read(fd, buf, count);
                    break;
                }

                /* sys_write */
                case 0x0001U: {
                    word fd {regs::g.w0};
                    word address {regs::h.w0};
                    hword count {regs::j.h0};

                    std::vector<byte> str = mm.read(address, count);
                    byte const* buf {&str[0]};
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
            syscall::exit();
        }

    } // namespace sys 

    namespace syscall {
#ifdef __linux__ 
        void init() {
        }

        void exit() {
        }

        word read(word fd, void* buf, hword count) {
            return ::read(fd, buf, count);
        }

        word write(word fd, const void* buf, hword count) {
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
        }

        void init() {
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

        void exit() {

        }

        word read(word fd, void* buf, hword count) {
            return -1;
        }

        word write(word fd, const void* buf, hword count) {
            if (fd == 0) {
                return (word)-1;
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

            return static_cast<word>(-1);
        }
#else
// Oops....
#endif
    }
} // namespace maize

    /* Old stuff from here down. I may yet find a use for this. */

#if false
void console::on_set() {
    cpu::reg::on_set();
    set(w0);
}

void console::set(word new_bus_value) {
    {
        std::lock_guard<std::mutex> lk(io_mutex);
        w0 = new_bus_value;
    }

    io_set.release();
}

word console::enable() {
    return 0;
}

namespace win {
    void console::open() {
        // std::cout << "opening" << std::endl;
        std::thread run_thread {&console::run, this};

        {
            std::unique_lock<std::mutex> lk(io_mutex);
            address_reg.h0 = 0xFFFE;
        }

        io_set.release();

        {
            std::unique_lock<std::mutex> lk(io_mutex);
            io_run_event.wait(lk);
            is_open = true;
        }

        // std::cout << "opened" << std::endl;

        if (run_thread.joinable()) {
            run_thread.detach();
        }
    }

    void console::close() {
        // std::cout << "closing" << std::endl;

        {
            std::unique_lock<std::mutex> lk(io_mutex);
            address_reg.h0 = 0xFFFF;
        }

        io_set.release();

        {
            std::unique_lock<std::mutex> lk(io_mutex);
            io_close_event.wait(lk);
            is_open = false;
        }

        // std::cout << "closed" << std::endl;
    }

    void console::run() {
        bool running = true;
        HANDLE hStdin {INVALID_HANDLE_VALUE};
        HANDLE hStdout {INVALID_HANDLE_VALUE};
        HANDLE hStderr {INVALID_HANDLE_VALUE};
        CONSOLE_SCREEN_BUFFER_INFO csbiInfo {0};

        while (running) {
            io_set.acquire();
            word local_bus_value {0};

            {
                std::unique_lock<std::mutex> lk(io_mutex);
                local_bus_value = w0;
            }

            cpu::reg_value cmd {w0};

            switch (address_reg.w0) {
                case 0x7F:
                {
                    auto op = cmd.b1;

                    switch (op) {
                        case 0x0A:
                        {
                            WCHAR c = static_cast<WCHAR>(b0);
                            WCHAR buf[1] {c};
                            WriteConsole(hStdout, buf, 1, nullptr, nullptr);
                            break;
                        }
                    }

                    break;
                }

                case 0xFFFE:
                {
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

                    io_run_event.notify_all();
                    break;
                }

                case 0xFFFF:
                {
                    running = false;
                    break;
                }
            }
        }

        io_close_event.notify_all();
}
#endif

