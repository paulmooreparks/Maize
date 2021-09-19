#pragma once

namespace maize {
	namespace sys {

		void init();
		void call();
		void exit();

		void enter_syscall(qword id);

		class console : public cpu::device {
		public:
			word enable();
			void set(word new_bus_value);
			virtual void on_set();

			virtual void open() = 0;
			virtual void close() = 0;
			virtual void run() = 0;

		protected:
			std::mutex ctrl_mutex;
			std::mutex io_mutex;
			std::counting_semaphore<16> io_set {0};
			std::condition_variable io_run_event;
			std::condition_variable io_close_event;
			bool is_open {false};
		};

	} // namespace sys

} // namespace maize


#ifdef __linux__ 
// Linux-specific code
#elif _WIN32
// Windows-specific code
#define NOMINMAX
#include <Windows.h>

namespace maize {
	namespace sys {
		namespace win {
			class console : public maize::sys::console {
			public:
				virtual void open();
				virtual void close();
				virtual void run();

			protected:
				std::mutex ctrl_mutex;
				std::mutex io_mutex;
				std::counting_semaphore<16> io_set {0};
				std::condition_variable io_run_event;
				std::condition_variable io_close_event;
				bool is_open {false};
			};
		}

	} // namespace sys

} // namespace maize


#else
// Oops....
#endif
