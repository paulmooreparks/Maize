#pragma once

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

namespace maize {
	namespace sys {
		void init();
		word call(qword syscall_id);
		void exit();
	} // namespace sys

	namespace syscall {
		void init();
		void exit();
		word read(word fd, void* buf, hword count);
		word write(word fd, const void *buf, hword count);
	}

} // namespace maize


