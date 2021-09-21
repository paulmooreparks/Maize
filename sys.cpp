#include "maize.h"
// #include "maize_sys.h"

/* This is all very broken right now, but I'm going to replace it with a sys_call architecture instead. */

namespace maize {
	namespace sys {

		HANDLE hStdin {INVALID_HANDLE_VALUE};
		HANDLE hStdout {INVALID_HANDLE_VALUE};
		HANDLE hStderr {INVALID_HANDLE_VALUE};
		CONSOLE_SCREEN_BUFFER_INFO csbiInfo {0};

		void init() {
			syscall::init();
		}

		word call(qword syscall_id) {
			using namespace cpu;

			switch (syscall_id) {
				/* sys_read */
				case 0x0000: {
					break;
				}

				/* sys_write */
				case 0x0001: {
					word fd {regs::g.w0};
					word address {regs::h.w0};
					hword count {regs::j.h0};

					std::vector<byte> str = mm.read(address, count);
					byte const* buf {&str[0]};
					return syscall::write(fd, buf, count);
				}

				/* sys_exit */
				case 0x003C: {
					break;
				}

				/* sys_reboot */
				case 0x00A9: {
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
		HANDLE hStdin {INVALID_HANDLE_VALUE};
		HANDLE hStdout {INVALID_HANDLE_VALUE};
		HANDLE hStderr {INVALID_HANDLE_VALUE};
		CONSOLE_SCREEN_BUFFER_INFO csbiInfo {0};

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

			return (word)-1;
		}

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
						case 0x7F: {
							auto op = cmd.b1;

							switch (op) {
								case 0x0A: {
									WCHAR c = static_cast<WCHAR>(b0);
									WCHAR buf[1] {c};
									WriteConsole(hStdout, buf, 1, nullptr, nullptr);
									break;
								}
							}

							break;
						}

						case 0xFFFE: {
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

						case 0xFFFF: {
							running = false;
							break;
						}
					}
				}

				io_close_event.notify_all();
			}
#endif

