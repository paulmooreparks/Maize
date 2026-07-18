#pragma once
// #include "maize_cpu.h"

#ifdef __linux__ 
// Linux-specific code
#include <unistd.h>

#elif _WIN32
// Windows-specific code
#define NOMINMAX
#include <Windows.h>

#else
// Oops....

#endif

/* maize-114: forward-declared at global scope so the setter below binds to the
   freestanding core's ::hostfs_table (src/hostfs/hostfs.h), not a namespace-local
   incomplete type. */
struct hostfs_table;

namespace maize {
	/* maize-140: forward-declared so sys.cpp can hold a console_io* without pulling in
	   the device models (mazm links sys.cpp but never devices.cpp). maize-238 adds the
	   stdin_injector seam (the single-stdin-owner routing) for the same reason. */
	namespace console { class console_io; class stdin_injector; }

	namespace sys {
		void init();
		u_word call(u_qword syscall_id);
		void exit();
		/* Process exit status recorded by SYS $3C (sys_exit); default 0 when no
		   sys_exit ran (e.g. a program that ended via HALT). Read by maize.cpp's
		   main after cpu::run() returns. */
		int exit_code();
		/* maize-75: seed the brk heap from the loaded image's end address.
		   Called by maize.cpp's main after the image is loaded and before the
		   process-start block is built; sets heap_base = current_brk =
		   align_up(image_end, 16). */
		void init_heap(u_word image_end);
		/* maize-114: install the parsed hostfs mount table (built by maize.cpp
		   from the --mount / --mount-home grants). NULL leaves hostfs inert
		   (mazm never calls this). Forward-declared; the full type lives in the
		   freestanding core header src/hostfs/hostfs.h. */
		void set_hostfs_table(hostfs_table* table);

		/* maize-140: bind the window text console to fd 0/1/2. NULL (the default, and
		   always in mazm) leaves stdio routed to the host exactly as before; maize.cpp
		   installs the text_console when the window console is active. When bound, fd 1/2
		   writes render as glyphs, fd 0 reads pull decoded keystrokes through the console's
		   termios line discipline, and SYS $F1/$F2 (tcgetattr/tcsetattr) reach its termios
		   state. */
		void set_console(console::console_io* c);

		/* maize-238 (Branch A): bind the active stdin injector (the console PORT device on
		   the default path). When set, SYS $00 read(0) routes through it so the device's
		   eager pre-read latch and the guest's synchronous read do not both consume host
		   stdin. NULL (default, and always in mazm) leaves fd 0 on the plain host-stdin
		   path exactly as before. */
		void set_stdin_injector(console::stdin_injector* inj);
	} // namespace sys

	namespace syscall {
		u_word read(u_word fd, void* buf, u_word count);
		u_word write(u_word fd, const void *buf, u_word count);

		/* maize-238 (Branch A): non-blocking host-stdin poll for console_device::on_input_tick.
		   Returns 1 and stores one byte in *b if a byte is immediately available, 0 if nothing
		   is pending (would block), or -1 on end of input / error. Platform-specific (POSIX
		   poll(fd0,0); Windows PeekNamedPipe / handle-type dispatch). */
		int console_poll_read(unsigned char* b);
	}

} // namespace maize


/* Old stuff from here down. I may yet find a use for this. */

#if false
void console::on_set() {
    cpu::reg::on_set();
    set(w0);
}

void console::set(u_word new_bus_value) {
    {
        std::lock_guard<std::mutex> lk(io_mutex);
        w0 = new_bus_value;
    }

    io_set.release();
}

u_word console::enable() {
    return 0;
}

namespace win {
    void console::open() {
        // std::cout << "opening" << std::endl;
        std::thread run_thread {&console::run, this};

        {
            std::unique_lock<std::mutex> lk(io_mutex);
            address_reg.set_h0(0xFFFE);
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
            address_reg.set_h0(0xFFFF);
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
            u_word local_bus_value {0};

            {
                std::unique_lock<std::mutex> lk(io_mutex);
                local_bus_value = w0;
            }

            cpu::reg_value cmd {w0};

            switch (address_reg.w0) {
                case 0x7F:
                {
                    auto op = cmd.b1();

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

