/* maize-313: POSIX backend for the host stdin source. Guarded on __linux__ so it
   compiles to nothing on Windows, mirroring the hostfs and presenter-transport backend
   convention. See src/stdin_source.h for the seam and the arm/ack protocol.

   The watcher polls fd 0 for readability and never reads it. It adds fd 0 to the poll set
   only while the source is armed, which is what stops a level-triggered poll on a readable
   fd 0 from returning immediately and forever: a disarmed source must not watch it.

   Why this backend needs no backstop poll and no forced raise, where the Windows one does.
   The shipped probe cannot answer 0 on an fd that poll reports readable: src/sys.cpp
   returns 0 only when poll returns nothing, or when the revents carry no POLLIN, POLLHUP
   or POLLERR, and every other path returns 1 or -1. So an armed watcher whose poll fires
   always raises, and the raise always disarms. A partial cooked line does not reach the
   watcher at all, because a canonical tty does not report POLLIN until the line is
   complete, which is the one external property this backend assumes and the property
   scripts/pty_oksh_check.py has been standing evidence for since maize-238. */

#include "stdin_source.h"

#ifdef __linux__

#include "fault_inject.h"
#include "host_tty.h"

#include <atomic>
#include <mutex>
#include <thread>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

namespace maize {
	namespace stdin_source {

		namespace {

			/* Every cross-thread field lives here behind one mutex, so the mutex's own
			   acquire and release edges carry all the ordering and no explicit memory orders
			   are needed. running_ is the one exception, because the readiness path and the
			   status port read it on hot paths. */
			std::mutex g_mutex;
			bool g_armed {false};
			bool g_stopping {false};
			unsigned long g_ack_level {0};
			int g_pushback {-1};             // synthetic-byte slot (H6), -1 when empty
			counters g_counters;

			/* A raise that has not yet been judged by a probe. Atomic rather than mutex-held,
			   because note_probe_answer runs on the readiness path and must cost nothing at all
			   in the ordinary case where no raise is outstanding. */
			std::atomic<bool> g_raise_outstanding {false};

			std::atomic<bool> g_running {false};
			/* Set by the single exit protocol only, and never by a clean stop(). */
			std::atomic<bool> g_ended {false};
			std::thread g_thread;
			bool g_started {false};          // start() ran and returned true
			bool g_has_thread {false};       // a watcher thread exists (false for a regular file)
			void (*g_on_ready)() {nullptr};

			/* The self-pipe. Created in start() and NEVER closed, not even by stop(): H6 puts
			   the write end in a signal handler's hands, and a handler that races a close could
			   write into a recycled descriptor. Two descriptors for the life of the process
			   costs nothing and removes that race entirely. */
			int g_wake_rd {-1};
			int g_wake_wr {-1};

			void poke_self_pipe() {
				if (g_wake_wr < 0) { return; }
				const unsigned char b = 1;
				ssize_t ignored = ::write(g_wake_wr, &b, 1);
				(void)ignored;
			}

			void drain_self_pipe() {
				unsigned char buf[64];
				for (;;) {
					ssize_t n = ::read(g_wake_rd, buf, sizeof buf);
					if (n <= 0) { break; }
					if (static_cast<size_t>(n) < sizeof buf) { break; }
				}
			}

			/* Raise IRQ 33 and disarm. The callback is invoked with no source lock held: its
			   whole body is cpu::raise_irq, which takes int_mutex, and the two mutexes are
			   never held at the same time by either thread. */
			void raise_and_disarm(bool count_as_level) {
				{
					std::lock_guard<std::mutex> lk(g_mutex);
					g_armed = false;
					if (count_as_level) { ++g_counters.level_raises; }
				}
				g_raise_outstanding.store(true, std::memory_order_release);
				if (g_on_ready != nullptr) { g_on_ready(); }
			}

			/* The single exit protocol (spec section 6, H7 rule 3): latch end of input,
			   raise once, return. Every exit path that is not a clean stop takes it, because a
			   source that dies must be indistinguishable from end of input to a parked guest.
			   The device latches eof_ itself on the next probe; what this owes is the raise
			   that makes the guest probe at all. */
			void eof_exit() {
				g_ended.store(true, std::memory_order_release);
				{
					std::lock_guard<std::mutex> lk(g_mutex);
					g_armed = false;
				}
				if (g_on_ready != nullptr) { g_on_ready(); }
			}

			/* Any poll return at all, armed or not, including EINTR, drains the self-pipe and
			   takes host_tty's synthetic byte. On a byte, store it in the pushback slot and
			   raise unconditionally, because a synthetic byte is new input that no ack can have
			   accounted for. The take is idempotent: it answers -1 when there is nothing. */
			bool absorb_synthetic() {
				int sb = host_tty::take_synthetic_byte();
				if (sb < 0) { return false; }
				{
					std::lock_guard<std::mutex> lk(g_mutex);
					g_pushback = sb;
				}
				return true;
			}

			void watcher() {
				bool die_next = fault::armed(fault::source_die);
				for (;;) {
					bool armed = false;
					{
						std::lock_guard<std::mutex> lk(g_mutex);
						if (g_stopping) { return; }
						armed = g_armed;
						if (armed) { ++g_counters.infinite_waits; }
					}

					if (die_next) {
						/* AC-16: a source thread that dies mid-run becomes a guest-visible end
						   of input rather than a park with no waker. */
						eof_exit();
						return;
					}

					struct pollfd pfds[2];
					pfds[0].fd = g_wake_rd;
					pfds[0].events = POLLIN;
					pfds[0].revents = 0;
					pfds[1].fd = 0;
					pfds[1].events = POLLIN;
					pfds[1].revents = 0;
					nfds_t nfds = armed ? 2 : 1;

					int pr = ::poll(pfds, nfds, -1);

					drain_self_pipe();
					bool synthetic = absorb_synthetic();

					{
						std::lock_guard<std::mutex> lk(g_mutex);
						if (g_stopping) { return; }
					}

					if (synthetic) {
						raise_and_disarm(true);
						continue;
					}

					if (pr < 0) {
						if (errno == EINTR) { continue; }
						eof_exit();
						return;
					}

					if (!armed) { continue; }

					short rev = pfds[1].revents;
					if ((rev & (POLLNVAL | POLLERR)) != 0) {
						/* Not readability. The shipped probe answers 0 for POLLNVAL, and
						   re-polling a closed fd 0 returns immediately forever, so treating this
						   as anything but end of input would reintroduce the spin this card
						   removes. */
						eof_exit();
						return;
					}
					if ((rev & (POLLIN | POLLHUP)) != 0) {
						raise_and_disarm(true);
					}
				}
			}

		}   // namespace

		bool start(void (*on_ready)()) {
			if (g_started) { return true; }
			if (fault::armed(fault::no_source)) { return false; }

			g_on_ready = on_ready;

			/* A regular file needs no watcher thread at all, and that is a proof rather than
			   a convenience: poll always reports a regular file readable, so the probe answers
			   1 or -1 and never 0, so the park hook raises rather than arming and nothing ever
			   needs to wake the guest for file input. Detected with fstat rather than by
			   inference. running() still answers true, so the guest sees CON_STAT_WAKE and takes
			   the park path for its other wake reasons. */
			struct stat st;
			if (::fstat(0, &st) == 0 && S_ISREG(st.st_mode)) {
				g_started = true;
				g_has_thread = false;
				g_running.store(true, std::memory_order_release);
				return true;
			}

			int fds[2];
			if (::pipe(fds) != 0) { return false; }
			g_wake_rd = fds[0];
			g_wake_wr = fds[1];
			::fcntl(g_wake_rd, F_SETFL, ::fcntl(g_wake_rd, F_GETFL, 0) | O_NONBLOCK);
			::fcntl(g_wake_wr, F_SETFL, ::fcntl(g_wake_wr, F_GETFL, 0) | O_NONBLOCK);

			/* H6: the handler writes here from whichever thread took the signal, so the wake
			   reaches the watcher even when the CPU thread is parked with no read to interrupt. */
			host_tty::set_signal_wake_fd(g_wake_wr);

			{
				std::lock_guard<std::mutex> lk(g_mutex);
				/* maize-313: start DISARMED. The spec's section 2 wrote an arm here with an
				   acknowledged level of 0 and called the raise it produces for an
				   already-readable stdin a spurious raise that section 3 absorbs. That is true
				   of quesOS and false of a bare guest: an asm program that runs SETINT and has
				   no vector-33 handler takes an unhandled-interrupt trap within microseconds of
				   start, where on master the instruction-tick pump would not have reached it for
				   16384 instructions. asm/test_timer.mazm retires 15 instructions and died on
				   exactly that.

				   Nothing needs the start-time arm, because D-25 moved the arming off start()
				   and onto the VM's park: the park hook probes on EVERY path into the park, an
				   answer of 0 publishes the ack that arms the source, and input already pending
				   at the park is the hook's own `raised` outcome rather than the source's. So a
				   guest that never parks never arms the source and pays nothing, and invariant W
				   is untouched. */
				g_armed = false;
				g_ack_level = 0;
				g_stopping = false;
			}

			try {
				g_thread = std::thread(watcher);
			}
			catch (...) {
				host_tty::set_signal_wake_fd(-1);
				return false;
			}

			g_started = true;
			g_has_thread = true;
			g_running.store(true, std::memory_order_release);
			return true;
		}

		void stop() {
			if (!g_started) { return; }
			g_running.store(false, std::memory_order_release);
			if (g_has_thread) {
				{
					std::lock_guard<std::mutex> lk(g_mutex);
					g_stopping = true;
				}
				poke_self_pipe();
				if (g_thread.joinable()) { g_thread.join(); }
				g_has_thread = false;
			}
			g_started = false;
		}

		bool running() {
			return g_running.load(std::memory_order_acquire);
		}

		bool input_ended() {
			return g_ended.load(std::memory_order_acquire);
		}

		unsigned long sample_level() {
			return 0;   // POSIX compares nothing: poll readability is edge-adequate here.
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
			   would wake the source thread thousands of times a second to tell it nothing. */
			if (wake) { poke_self_pipe(); }
		}

		bool pushback_pending() {
			if (!running()) { return false; }
			std::lock_guard<std::mutex> lk(g_mutex);
			return g_pushback >= 0;
		}

		int take_pushback() {
			if (!running()) { return -1; }
			std::lock_guard<std::mutex> lk(g_mutex);
			int b = g_pushback;
			g_pushback = -1;
			return b;
		}

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

#endif   // __linux__
