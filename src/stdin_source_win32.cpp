/* maize-313: Windows backend for the host stdin source. Guarded on _WIN32 so it compiles
   to nothing on POSIX, mirroring the hostfs and presenter-transport backend convention.
   See src/stdin_source.h for the seam and the arm/ack protocol.

   THE ONE RULE: the source thread never reads fd 0, on any handle type. That is what makes
   the whole of the earlier design's Windows risk surface disappear. There is no outstanding
   read for a cross-thread cancel to reach, none for a SetConsoleMode to race, and none to
   disturb echo or line editing, so this file answers none of the undocumented console
   questions maize-345 had to measure. It waits, it counts, and it peeks a pipe.

   The documented surface it stands on, and nothing else:

   - A console input handle is a waitable synchronization object, signaled while the
     console's input buffer holds unread input and non-signaled when the buffer is empty.
   - GetNumberOfConsoleInputEvents reports the number of unread records without consuming.
   - SetConsoleMode decides which record kinds reach the buffer, and host_tty already
     masks ENABLE_MOUSE_INPUT and ENABLE_WINDOW_INPUT out of it, so a pointer moving
     across the console window queues nothing and cannot wake the VM.
   - PeekNamedPipe reports the bytes available on a pipe without consuming or blocking.
   - A timed wait elapses in units of the system timer resolution, so every interval here
     is a floor rather than a period and elapsed time is accumulated from GetTickCount64
     rather than assumed from the nominal interval.

   The one documented gap is that Windows offers a wait for the input buffer being
   NON-EMPTY and no wait for it GROWING. The record count turns that level into an edge
   cheaply and is the fast path, and because "a change in that count" is this watcher's own
   inference rather than a documented guarantee, the armed-and-non-empty state ALSO raises
   unconditionally once it has persisted wake_forced_ms. So the count comparison decides how
   QUICKLY the guest is woken and never decides WHETHER it is woken. */

#include "stdin_source.h"

#ifdef _WIN32

#include "fault_inject.h"
#include "host_tty.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace maize {
	namespace stdin_source {

		namespace {

			/* How long the watcher waits before re-reading its level indicator while armed
			   and unable to block, and how long an armed and unchanged state may persist
			   before it raises anyway. Nothing about correctness depends on either constant:
			   raising the backstop costs wake latency in one state, lowering it costs host
			   CPU, and the forced interval bounds the pathological case only. Neither is a
			   tuning knob for a race. */
			constexpr unsigned long long wake_backstop_ms {20};
			constexpr unsigned long long wake_forced_ms {1000};

			/* Every cross-thread field lives here behind one mutex, so the mutex's own
			   acquire and release edges carry all the ordering and no explicit memory orders
			   are needed. running_ is the one exception, because the readiness path and the
			   status port read it on hot paths. unchanged_ms and empty_immediate are locals of
			   the watcher's own loop and are not shared at all. */
			std::mutex g_mutex;
			bool g_armed {false};
			bool g_stopping {false};
			unsigned long g_ack_level {0};
			counters g_counters;

			std::atomic<bool> g_raise_outstanding {false};
			std::atomic<bool> g_running {false};
			/* Set by the single exit protocol only, and never by a clean stop(). */
			std::atomic<bool> g_ended {false};
			std::atomic<bool> g_freeze_level {false};

			std::thread g_thread;
			bool g_started {false};
			bool g_has_thread {false};
			void (*g_on_ready)() {nullptr};

			HANDLE g_in {INVALID_HANDLE_VALUE};
			/* The handle's kind, resolved once in start_on_handle and never again. It cannot
			   change for the life of a source, so sample_level has no reason to ask the OS on
			   every readiness probe, and that probe runs once per park plus once per 16384
			   retired instructions. It is also the SAME value that chose the watcher thread,
			   rather than a second derivation that could disagree with it. Published before
			   g_running's release store and read after running()'s acquire load, exactly as
			   g_in is, so the CPU thread sees both or neither. */
			bool g_is_console {false};
			HANDLE g_ack_event {nullptr};    // auto-reset: set by ack_not_ready
			HANDLE g_stop_event {nullptr};   // manual-reset: set by stop()

			HANDLE resolve_stdin() {
				HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
				return h;
			}

			bool stopping() {
				std::lock_guard<std::mutex> lk(g_mutex);
				return g_stopping;
			}

			/* Raise IRQ 33 and disarm. The callback is invoked with no source lock held: its
			   whole body is cpu::raise_irq, which takes int_mutex, and the two mutexes are
			   never held at the same time by either thread. The forced raise obeys the same
			   rule as a count-driven one, so there is no state in which the source raises
			   twice with no intervening acknowledgement. */
			void raise(bool forced) {
				{
					std::lock_guard<std::mutex> lk(g_mutex);
					g_armed = false;
					if (forced) { ++g_counters.forced_raises; }
					else        { ++g_counters.level_raises; }
				}
				g_raise_outstanding.store(true, std::memory_order_release);
				if (g_on_ready != nullptr) { g_on_ready(); }
			}

			/* The single exit protocol (spec section 6, H7 rule 3): latch end of input, raise
			   once, return. Every exit path that is not a clean stop takes it, because a source
			   that dies must be indistinguishable from end of input to a parked guest. */
			void eof_exit() {
				g_ended.store(true, std::memory_order_release);
				{
					std::lock_guard<std::mutex> lk(g_mutex);
					g_armed = false;
				}
				if (g_on_ready != nullptr) { g_on_ready(); }
			}

			/* The console record count, or the acknowledged level when the freeze fault is
			   armed. Returns false when the count could not be taken at all, which means the
			   console handle has gone and the guest already handles that as end of input. */
			bool console_level(unsigned long ack, unsigned long* out) {
				if (g_freeze_level.load(std::memory_order_acquire)
					|| fault::armed(fault::freeze_level)) {
					*out = ack;
					return true;
				}
				DWORD n {0};
				if (!GetNumberOfConsoleInputEvents(g_in, &n)) { return false; }
				*out = static_cast<unsigned long>(n);
				return true;
			}

			bool pipe_level(unsigned long ack, unsigned long* out) {
				if (g_freeze_level.load(std::memory_order_acquire)
					|| fault::armed(fault::freeze_level)) {
					*out = ack;
					return true;
				}
				DWORD avail {0};
				if (!PeekNamedPipe(g_in, nullptr, 0, nullptr, &avail, nullptr)) { return false; }
				*out = static_cast<unsigned long>(avail);
				return true;
			}

			void console_watcher() {
				unsigned long long unchanged_ms = 0;   // armed, level unchanged, accumulated
				int empty_immediate = 0;               // consecutive zero-cost empty-branch returns
				bool die_next = fault::armed(fault::source_die);

				for (;;) {
					bool armed = false;
					unsigned long ack = 0;
					{
						std::lock_guard<std::mutex> lk(g_mutex);
						if (g_stopping) { return; }
						armed = g_armed;
						ack = g_ack_level;
					}

					if (!armed) {
						unchanged_ms = 0;
						empty_immediate = 0;
						HANDLE waits[2] = { g_ack_event, g_stop_event };
						DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
						if (r == WAIT_FAILED) { eof_exit(); return; }
						if (stopping()) { return; }
						continue;
					}

					unsigned long n = 0;
					if (die_next || !console_level(ack, &n)) { eof_exit(); return; }

					/* The comparison is != rather than > deliberately. The record count can
					   fall without the guest consuming a byte, because the raw-mode branch of
					   the shipped probe consumes records it has proved cannot yield one. A >
					   test would leave a stale high watermark that suppresses a later raise;
					   != treats any divergence as a reason to raise, costs at most one spurious
					   IRQ, and self-heals on the next ack. */
					if (n != ack) {
						raise(false);
						unchanged_ms = 0;
						empty_immediate = 0;
						continue;
					}

					if (n == 0 && empty_immediate < 2) {
						/* An empty input buffer holds nothing a guest could consume, and the
						   handle's signal covers the empty-to-non-empty transition by
						   documentation, so this branch never forces. empty_immediate is a
						   structural bound rather than an argument: if the handle ever signals
						   while the count reads zero, the loop takes the timed branch on the
						   third pass and the forced raise applies from there. */
						{
							std::lock_guard<std::mutex> lk(g_mutex);
							++g_counters.infinite_waits;
						}
						ULONGLONG t0 = GetTickCount64();
						HANDLE waits[3] = { g_in, g_ack_event, g_stop_event };
						DWORD r = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
						if (r == WAIT_FAILED) { eof_exit(); return; }
						if (stopping()) { return; }
						empty_immediate = (GetTickCount64() == t0) ? empty_immediate + 1 : 0;
						continue;
					}

					{
						std::lock_guard<std::mutex> lk(g_mutex);
						++g_counters.backstop_waits;
					}
					ULONGLONG t0 = GetTickCount64();
					HANDLE waits[2] = { g_ack_event, g_stop_event };
					DWORD r = WaitForMultipleObjects(2, waits, FALSE,
						static_cast<DWORD>(wake_backstop_ms));
					if (r == WAIT_FAILED) { eof_exit(); return; }
					if (stopping()) { return; }
					if (r == WAIT_TIMEOUT) {
						unchanged_ms += GetTickCount64() - t0;
						if (unchanged_ms >= wake_forced_ms) { raise(true); unchanged_ms = 0; }
					}
					else {
						unchanged_ms = 0;      // an ack landed: the state is fresh
						empty_immediate = 0;
					}
				}
			}

			/* A pipe handle is not a waitable synchronization object, so this loop has no
			   branch that can block on its input and the console loop cannot be reused by
			   substitution. Three branches rather than four. */
			void pipe_watcher() {
				unsigned long long unchanged_ms = 0;
				bool die_next = fault::armed(fault::source_die);

				for (;;) {
					bool armed = false;
					unsigned long ack = 0;
					{
						std::lock_guard<std::mutex> lk(g_mutex);
						if (g_stopping) { return; }
						armed = g_armed;
						ack = g_ack_level;
					}

					if (!armed) {
						unchanged_ms = 0;
						HANDLE waits[2] = { g_ack_event, g_stop_event };
						DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
						if (r == WAIT_FAILED) { eof_exit(); return; }
						if (stopping()) { return; }
						continue;
					}

					unsigned long avail = 0;
					/* A PeekNamedPipe failure is a broken pipe and therefore end of input. */
					if (die_next || !pipe_level(ack, &avail)) { eof_exit(); return; }
					if (avail != ack) { raise(false); unchanged_ms = 0; continue; }

					{
						std::lock_guard<std::mutex> lk(g_mutex);
						++g_counters.backstop_waits;
					}
					ULONGLONG t0 = GetTickCount64();
					HANDLE waits[2] = { g_ack_event, g_stop_event };
					DWORD r = WaitForMultipleObjects(2, waits, FALSE,
						static_cast<DWORD>(wake_backstop_ms));
					if (r == WAIT_FAILED) { eof_exit(); return; }
					if (stopping()) { return; }
					if (r == WAIT_TIMEOUT) {
						unchanged_ms += GetTickCount64() - t0;
						/* The forced raise is carried here for uniformity rather than because a
						   pipe needs it, since a write to a pipe does increase the available
						   count by documentation. One loop shape means one negative control
						   covers both backends. */
						if (unchanged_ms >= wake_forced_ms) { raise(true); unchanged_ms = 0; }
					}
					else {
						unchanged_ms = 0;
					}
				}
			}

			void close_events() {
				if (g_ack_event != nullptr) { CloseHandle(g_ack_event); g_ack_event = nullptr; }
				if (g_stop_event != nullptr) { CloseHandle(g_stop_event); g_stop_event = nullptr; }
			}

			bool start_on_handle(HANDLE h, void (*on_ready)()) {
				if (g_started) { return true; }
				if (fault::armed(fault::no_source)) { return false; }
				if (h == INVALID_HANDLE_VALUE || h == nullptr) { return false; }

				g_in = h;
				g_on_ready = on_ready;

				DWORD type = GetFileType(h);

				if (type == FILE_TYPE_DISK) {
					/* A redirected file needs no source thread at all, and that is a proof
					   rather than a convenience: the disk branch of the shipped probe returns 1
					   or -1 and never 0, so the park hook raises rather than arming and nothing
					   ever needs to wake the guest for file input. running() still answers true,
					   so the guest sees CON_STAT_WAKE and takes the park path for its other wake
					   reasons. */
					g_started = true;
					g_has_thread = false;
					/* Assigned rather than left at the previous run's value. Every other exit
					   from this function either sets it or fails, so leaving it here would make
					   a start-stop-start sequence ending on a redirected file the one path
					   whose handle kind is stale. It cannot bite today, because sample_level()
					   returns 0 on !g_has_thread before it reads this, and nothing starts a
					   second source in a process; the invariant is worth more than the one
					   line it costs. */
					g_is_console = false;
					g_running.store(true, std::memory_order_release);
					return true;
				}

				bool is_console = false;
				if (type == FILE_TYPE_CHAR) {
					DWORD mode = 0;
					if (!GetConsoleMode(h, &mode)) { return false; }   // NUL and friends
					is_console = true;
					/* A console source requires host_tty::active(), so the input-record mask is
					   guaranteed to be in force whenever a console watcher runs. A build that
					   never initializes host_tty, which is maizeg, therefore degrades to
					   master's behaviour through H7 rather than watching an unmasked console. */
					if (!host_tty::active()) { return false; }
				}
				else if (type != FILE_TYPE_PIPE) {
					return false;   // no realistic stdin source produces another type
				}

				g_ack_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
				g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
				if (g_ack_event == nullptr || g_stop_event == nullptr) {
					close_events();
					return false;
				}

				{
					std::lock_guard<std::mutex> lk(g_mutex);
					/* maize-313: start DISARMED, for the reason src/stdin_source_posix.cpp
					   states at the same point. An arm here raises IRQ 33 for an
					   already-readable stdin before the guest has installed a handler, which is
					   fatal to a bare guest, and D-25 removed the need for it by making the VM's
					   park publish the acknowledgement on every path. */
					g_armed = false;
					g_ack_level = 0;
					g_stopping = false;
				}

				g_is_console = is_console;
				try {
					g_thread = is_console ? std::thread(console_watcher) : std::thread(pipe_watcher);
				}
				catch (...) {
					close_events();
					return false;
				}

				g_started = true;
				g_has_thread = true;
				g_running.store(true, std::memory_order_release);
				return true;
			}

		}   // namespace

		bool start(void (*on_ready)()) {
			return start_on_handle(resolve_stdin(), on_ready);
		}

		bool start_for_test(void* handle, void (*on_ready)()) {
			return start_on_handle(static_cast<HANDLE>(handle), on_ready);
		}

		void set_level_freeze_for_test(bool on) {
			g_freeze_level.store(on, std::memory_order_release);
		}

		void stop() {
			if (!g_started) { return; }
			g_running.store(false, std::memory_order_release);
			if (g_has_thread) {
				{
					std::lock_guard<std::mutex> lk(g_mutex);
					g_stopping = true;
				}
				if (g_stop_event != nullptr) { SetEvent(g_stop_event); }
				if (g_thread.joinable()) { g_thread.join(); }
				g_has_thread = false;
			}
			close_events();
			g_started = false;
		}

		bool running() {
			return g_running.load(std::memory_order_acquire);
		}

		bool input_ended() {
			return g_ended.load(std::memory_order_acquire);
		}

		unsigned long sample_level() {
			if (!running() || !g_has_thread) { return 0; }
			unsigned long lvl = 0;
			unsigned long ack = 0;
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				ack = g_ack_level;
			}
			if (!g_is_console) {
				if (!pipe_level(ack, &lvl)) { return ack; }
				return lvl;
			}
			if (!console_level(ack, &lvl)) { return ack; }
			return lvl;
		}

		void ack_not_ready(unsigned long observed_level) {
			if (!running()) { return; }
			bool wake = false;
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				if (!g_armed || g_ack_level != observed_level) {
					g_armed = true;
					g_ack_level = observed_level;
					wake = true;
				}
			}
			/* Signal the watcher only when the ack actually changed something. Without that
			   guard a compute-bound guest, whose instruction-tick pump acks on every tick,
			   would wake the source thread thousands of times a second to tell it nothing.
			   ack_event appears in every wait, which is what stops a lost wakeup: an ack
			   published between the watcher's snapshot and its wait leaves the event already
			   set and the wait returns at once. */
			if (wake && g_ack_event != nullptr) { SetEvent(g_ack_event); }
		}

		bool pushback_pending() { return false; }   // POSIX only
		int  take_pushback()    { return -1; }

		counters snapshot() {
			std::lock_guard<std::mutex> lk(g_mutex);
			return g_counters;
		}

		void note_probe_answer(int r) {
			if (!g_raise_outstanding.load(std::memory_order_acquire)) { return; }
			g_raise_outstanding.store(false, std::memory_order_relaxed);
			if (r != 0) { return; }
			std::lock_guard<std::mutex> lk(g_mutex);
			++g_counters.idle_raises;
		}

	}   // namespace stdin_source
}   // namespace maize

#endif   // _WIN32
