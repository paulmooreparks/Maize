/* src/stdin_source.h -- maize-313: the execution-independent host stdin source.
 *
 * A CPU parked in HALT retires no instructions, so console_device::on_input_tick, which
 * the dispatch preamble calls once per 16384 retired instructions, cannot see the next
 * stdin byte. This seam gives the VM a host-side thread that watches stdin WITHOUT
 * consuming it and raises IRQ 33 from its own thread when the watched level changes, so a
 * HALT-parked CPU wakes and quesos_idle can park instead of burning a core.
 *
 * The source never reads fd 0 on any handle type. The POSIX backend polls fd 0 for
 * readability and the Windows backend waits on the console input handle, which is a
 * documented waitable synchronization object, or peeks a pipe with PeekNamedPipe. That
 * rule is what keeps the design clear of every undocumented console question maize-345
 * had to measure: there is no outstanding read to cancel, none for a mode change to
 * reach, and none to disturb echo or line editing.
 *
 * Both backends implement one wake contract, the arm/ack protocol (spec section 3):
 *
 *   Armed    The source is watching and will raise IRQ 33 when its platform's level
 *            indicator differs from the level the CPU thread last acknowledged, or when
 *            the armed and unchanged state has persisted wake_forced_ms (Windows only).
 *   Disarmed The source has raised and has not been acknowledged since. It watches
 *            nothing and costs nothing.
 *
 * Three rules move it and there are no others. ack_not_ready(level) arms the source and
 * records the level. Raising disarms it. start() leaves it DISARMED and stop() retires it.
 *
 * A source starts disarmed because the VM's park, not start(), is what arms it: the park
 * hook probes host stdin on every path into the wait-for-interrupt park, an answer of 0
 * publishes the acknowledgement that arms the source, and input already pending AT the park
 * raises from the hook rather than from the source. A guest that never parks therefore never
 * arms the source and pays nothing for it. Arming at start instead would raise IRQ 33 for an
 * already-readable stdin before the guest has installed a handler, which quesOS absorbs and a
 * bare guest does not: an asm program that runs SETINT takes an unhandled-interrupt trap.
 */
#ifndef MAIZE_STDIN_SOURCE_H
#define MAIZE_STDIN_SOURCE_H

#include <cstdint>

namespace maize {
	namespace stdin_source {

		/* Start the host stdin source. on_ready is invoked from the source's OWN thread and
		   is the IRQ-33 raise; see the arm/ack protocol above for exactly when it fires.
		   Idempotent: a second start is a no-op. Returns false when no source could be
		   started, which spec section 6's H7 governs: the caller leaves CON_STAT_WAKE clear,
		   the guest spins rather than parking, and the instruction-tick pump keeps raising
		   IRQ 33 exactly as it does on master. */
		bool start(void (*on_ready)());

		/* Stop the source and join its thread. Idempotent, and safe after start() returned
		   false. Every wait the source performs includes the stop event, so the join is
		   bounded without a grace period (D-19). */
		void stop();

		/* True between a successful start() and stop(). console_device reports it to the
		   guest as status bit3, CON_STAT_WAKE. */
		bool running();

		/* True once the source has taken its single exit protocol, which every failure path
		   takes: latch end of input, raise once, return. The readiness probe answers -1 from
		   then on, which is what makes "a source that died" and "host stdin reached end of
		   input" the same thing to a guest.

		   Raising alone is not enough and the difference is a hang. A watcher that dies while
		   the pipe still holds a writer raises once, the guest wakes, its probe answers 0
		   because the pipe is merely empty, its reader stays parked, its next acknowledgement
		   arms a thread that no longer exists, and nothing raises again. The guest already
		   handles end of input without parking, so reporting it is the whole repair. A clean
		   stop() does NOT set this: it runs after cpu::run() has returned, where no guest is
		   left to tell. */
		bool input_ended();

		/* ---- the arm/ack protocol ---------------------------------------------------
		   sample_level() reads the platform's non-consuming level indicator BEFORE the
		   readiness probe runs: the console record count on Windows, the PeekNamedPipe byte
		   count for a Windows pipe, 0 everywhere else. ack_not_ready() publishes "the CPU
		   thread probed host stdin and it was not ready, and this is the level it had",
		   which re-arms the source. Both are no-ops when !running(). Called only from
		   maize::syscall::console_stdin_ready().

		   Sampling the level BEFORE the probe rather than after is the one ordering rule in
		   this seam. Input arriving between the sample and the probe either makes the probe
		   answer 1, in which case no ack is published and the guest does not park, or leaves
		   the ack carrying the older level, in which case the watcher sees a level that
		   differs from the ack and raises. A sample taken after the probe would record input
		   the CPU never judged as input the CPU had judged, and the guest would park on a
		   byte that was already there. */
		unsigned long sample_level();
		void ack_not_ready(unsigned long observed_level);

		/* ---- the POSIX synthetic-byte pushback slot (spec section 6, H6) -------------
		   Always false / -1 on Windows. pushback_pending() is non-consuming and read by the
		   readiness probe; take_pushback() consumes and is read by the fd-0 read path. A
		   cooked-mode host SIGINT sets host_tty's synthetic byte from whichever thread ran
		   the handler and writes the source's self-pipe, so the watcher can pick the byte up
		   even when the CPU thread is parked with no read to interrupt. */
		bool pushback_pending();
		int  take_pushback();

		/* ---- instrumentation (spec section 8.4) --------------------------------------
		   Every claim this design makes about idle cost is a claim about how often the
		   watcher wakes, so the watcher counts its own states rather than leaving them to
		   be argued. --show-perf prints these. */
		struct counters {
			std::uint64_t infinite_waits {0};   // input buffer observed empty while armed
			std::uint64_t backstop_waits {0};   // observed non-empty and unchanged
			std::uint64_t level_raises {0};     // level differed from the acknowledged level
			std::uint64_t forced_raises {0};    // armed and unchanged reached wake_forced_ms
			std::uint64_t idle_raises {0};      // raises the next probe found undeliverable
		};
		counters snapshot();

		/* Called by the readiness probe with the answer it is about to return, so the source
		   can attribute a raise that turned out to deliver nothing (the idle_raises counter
		   above). Cheap and lock-free when no raise is outstanding. */
		void note_probe_answer(int r);

#ifdef _WIN32
		/* ---- the direct-drive test seam (spec section 8.4) ---------------------------
		   Windows has no quesOS fixture, so the Windows-only claims that need no guest are
		   taken by stdin_source_test driving this seam against handles it creates itself
		   rather than by inference from the POSIX legs. The handle is passed as void* so
		   this header stays free of windows.h; the backend casts it back to HANDLE.

		   start_for_test replaces the process stdin handle for the life of one source, and
		   is otherwise the ordinary start(). It is compiled into the VM binaries as well,
		   because a seam that exists only in a test build is a seam the test does not
		   exercise, and it is reachable from nothing the VM itself calls. */
		bool start_for_test(void* handle, void (*on_ready)());

		/* Force the watcher's level read to answer the acknowledged level forever, which is
		   the console-folds-a-record fault the forced raise exists for. The environment
		   token MAIZE_FAULT=freeze_level does the same thing for a whole process; this is
		   the in-process form the direct-drive test needs. */
		void set_level_freeze_for_test(bool on);
#endif

	}   // namespace stdin_source
}   // namespace maize

#endif   // MAIZE_STDIN_SOURCE_H
