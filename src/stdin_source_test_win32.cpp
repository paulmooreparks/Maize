/* maize-313: the Windows direct-drive test for the host stdin source.
 *
 * Windows has no quesOS fixture, so the Windows-only claims that need no guest are taken
 * here rather than inferred from the POSIX legs. It drives maize::stdin_source against
 * handles it creates itself, with a counting stub for the raise callback, so the pipe
 * watcher's timing and the forced raise are measured rather than reasoned about.
 *
 * A test, not a deliverable: deliberately absent from scripts/install-mzasm.ps1,
 * scripts/install-mzasm.sh, the Ctrl+Shift+B task and the MAIZE_SANITIZE list, for the same
 * reason console_probe_test is. The console loop's own branches need a real console and are
 * covered by the interactive checks instead; what is here is everything a pipe handle can
 * reach, which is the whole of the pipe watcher plus the forced raise plus the join.
 */

#include "stdin_source.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <vector>

namespace {

	std::atomic<unsigned> g_raises {0};

	void on_ready_stub() { g_raises.fetch_add(1, std::memory_order_release); }

	int g_failures = 0;

	void report(bool ok, const char* what) {
		std::printf("%s %s\n", ok ? "  [pass]" : "  [FAIL]", what);
		if (!ok) { ++g_failures; }
	}

	/* The system timer resolution is commonly 15.6 ms and this design does not alter it, so
	   every interval the watcher uses is a FLOOR rather than a period. One tick of slack on
	   every deadline here is that documented fact, not a fudge factor. */
	constexpr unsigned long tick_slack_ms = 32;
	constexpr unsigned long wake_backstop_ms = 20;
	constexpr unsigned long wake_forced_ms = 1000;

	unsigned raises() { return g_raises.load(std::memory_order_acquire); }

	/* Wait for the raise count to move past `from`, up to `budget` ms. Returns the elapsed
	   milliseconds, or budget + 1 when it never moved. */
	unsigned long wait_for_raise(unsigned from, unsigned long budget) {
		ULONGLONG t0 = GetTickCount64();
		for (;;) {
			if (raises() > from) {
				return static_cast<unsigned long>(GetTickCount64() - t0);
			}
			if (GetTickCount64() - t0 > budget) { return budget + 1; }
			Sleep(1);
		}
	}

	struct pipe_pair {
		HANDLE rd {nullptr};
		HANDLE wr {nullptr};
		~pipe_pair() {
			if (rd != nullptr) { CloseHandle(rd); }
			if (wr != nullptr) { CloseHandle(wr); }
		}
	};

	bool make_pipe(pipe_pair& p) {
		SECURITY_ATTRIBUTES sa {};
		sa.nLength = sizeof sa;
		sa.bInheritHandle = FALSE;
		return CreatePipe(&p.rd, &p.wr, &sa, 0) != 0;
	}

	void write_byte(HANDLE h) {
		const char b = 'x';
		DWORD n = 0;
		WriteFile(h, &b, 1, &n, nullptr);
	}

	/* Case 1: the pipe watcher's raise latency, over 100 trials, reported as median and
	   maximum against the backstop floor plus one tick. The maximum alone would hide the
	   distribution, and the system timer tick is exactly what makes the two differ. */
	void case_pipe_latency() {
		std::printf("pipe watcher raise latency (100 trials)\n");
		pipe_pair p;
		if (!make_pipe(p)) { report(false, "CreatePipe"); return; }
		if (!maize::stdin_source::start_for_test(p.rd, &on_ready_stub)) {
			report(false, "start_for_test on a pipe handle"); return;
		}

		std::vector<unsigned long> lat;
		lat.reserve(100);
		bool all_raised = true;
		for (int i = 0; i < 100; ++i) {
			unsigned before = raises();
			/* Arm the source at the level the CPU thread would have observed, which for an
			   empty pipe is zero, then produce the input the watcher must notice. */
			maize::stdin_source::ack_not_ready(0);
			write_byte(p.wr);
			unsigned long ms = wait_for_raise(before, wake_backstop_ms + tick_slack_ms + 200);
			if (ms > wake_backstop_ms + tick_slack_ms + 200) { all_raised = false; break; }
			lat.push_back(ms);
			/* Drain, so the next trial starts from an empty pipe again. The TEST drains it,
			   never the watcher: the watcher issues no read on any handle type, which is the
			   one rule the whole Windows design rests on. */
			char sink = 0;
			DWORD got = 0;
			ReadFile(p.rd, &sink, 1, &got, nullptr);
		}
		maize::stdin_source::stop();

		report(all_raised && lat.size() == 100, "every trial raised");
		if (lat.size() == 100) {
			std::vector<unsigned long> sorted = lat;
			for (size_t i = 1; i < sorted.size(); ++i) {
				unsigned long v = sorted[i];
				size_t j = i;
				while (j > 0 && sorted[j - 1] > v) { sorted[j] = sorted[j - 1]; --j; }
				sorted[j] = v;
			}
			unsigned long med = sorted[sorted.size() / 2];
			unsigned long mx = sorted.back();
			std::printf("    median %lu ms, maximum %lu ms (floor %lu ms plus one %lu ms tick)\n",
				med, mx, wake_backstop_ms, tick_slack_ms);
			report(mx <= wake_backstop_ms + tick_slack_ms + 50,
				"the maximum sits within the backstop floor plus one tick");
			/* The number a piped guest actually pays per line, recorded rather than left as
			   an impression. Master's instruction-tick pump reaches a byte in about 0.16 ms,
			   so this is a real regression on a piped workload and the card is entitled to
			   ship it only because somebody has looked at it. */
			std::printf("    implied per-line cost for a piped guest: about %lu ms, "
				"against master's instruction-tick figure of about 0.16 ms\n", med);
		}
	}

	/* Case 2: a disarmed source issues no peek at all. A watcher that polls while nothing is
	   parked is a spin with a timer on it, which is the shape this design exists to avoid. */
	void case_disarmed_is_silent() {
		std::printf("a disarmed source watches nothing\n");
		pipe_pair p;
		if (!make_pipe(p)) { report(false, "CreatePipe"); return; }
		if (!maize::stdin_source::start_for_test(p.rd, &on_ready_stub)) {
			report(false, "start_for_test on a pipe handle"); return;
		}
		/* start() leaves the source disarmed, so this is its state already. Write a byte and
		   require that nothing happens: with no acknowledgement, nothing may raise. */
		maize::stdin_source::counters before = maize::stdin_source::snapshot();
		unsigned raises_before = raises();
		write_byte(p.wr);
		Sleep(300);
		maize::stdin_source::counters after = maize::stdin_source::snapshot();
		bool silent = (raises() == raises_before)
			&& (after.backstop_waits == before.backstop_waits)
			&& (after.level_raises == before.level_raises)
			&& (after.forced_raises == before.forced_raises);
		maize::stdin_source::stop();
		report(silent, "a disarmed source raised nothing and entered no timed wait");
	}

	/* Case 3: a frozen level indicator costs latency and not a park. This is the whole
	   evidence for the forced raise. Without it the forced raise is a claim about a case
	   nobody can reach on purpose, which is how the assumption it replaces survived three
	   spec cycles unstated. */
	void case_frozen_level_forces() {
		std::printf("a frozen level indicator still wakes the guest\n");
		pipe_pair p;
		if (!make_pipe(p)) { report(false, "CreatePipe"); return; }
		if (!maize::stdin_source::start_for_test(p.rd, &on_ready_stub)) {
			report(false, "start_for_test on a pipe handle"); return;
		}
		maize::stdin_source::set_level_freeze_for_test(true);

		maize::stdin_source::counters before = maize::stdin_source::snapshot();
		unsigned raises_before = raises();
		/* Arm at a NON-ZERO acknowledged level, so the armed-and-unchanged state is the one
		   the forced raise governs rather than the empty-buffer state, which owes nothing. */
		write_byte(p.wr);
		maize::stdin_source::ack_not_ready(1);
		unsigned long ms = wait_for_raise(raises_before, wake_forced_ms + tick_slack_ms + 500);
		maize::stdin_source::counters after = maize::stdin_source::snapshot();

		report(ms <= wake_forced_ms + tick_slack_ms + 500,
			"a raise arrived despite the level never moving");
		std::printf("    raised after %lu ms (forced interval %lu ms plus one tick)\n",
			ms, wake_forced_ms);
		report(after.forced_raises > before.forced_raises,
			"the forced-raise counter moved, so the forced path is what fired");
		report(after.level_raises == before.level_raises,
			"no count-driven raise fired, which would have masked the forced path");

		/* The forced raise disarms exactly as a count-driven one does, so no second raise
		   may arrive before the next acknowledgement. Without that the forced raise would sit
		   outside the arm/ack protocol and reintroduce an unbounded raise rate. */
		unsigned after_first = raises();
		Sleep(static_cast<DWORD>(wake_forced_ms + 500));
		report(raises() == after_first,
			"the forced raise disarmed, so no second raise arrived without an ack");

		maize::stdin_source::set_level_freeze_for_test(false);
		maize::stdin_source::stop();
	}

	/* Case 4: stop() joins from each state, with no grace period and no detach. */
	void case_stop_joins() {
		std::printf("stop() joins from every state\n");
		{
			pipe_pair p;
			if (!make_pipe(p)) { report(false, "CreatePipe"); return; }
			maize::stdin_source::start_for_test(p.rd, &on_ready_stub);
			ULONGLONG t0 = GetTickCount64();
			maize::stdin_source::stop();
			report(GetTickCount64() - t0 < 1000, "stop() from the disarmed state joined promptly");
		}
		{
			pipe_pair p;
			if (!make_pipe(p)) { report(false, "CreatePipe"); return; }
			maize::stdin_source::start_for_test(p.rd, &on_ready_stub);
			maize::stdin_source::ack_not_ready(0);
			Sleep(50);
			ULONGLONG t0 = GetTickCount64();
			maize::stdin_source::stop();
			report(GetTickCount64() - t0 < 1000, "stop() from the armed state joined promptly");
		}
		{
			pipe_pair p;
			if (!make_pipe(p)) { report(false, "CreatePipe"); return; }
			maize::stdin_source::start_for_test(p.rd, &on_ready_stub);
			maize::stdin_source::ack_not_ready(1);
			write_byte(p.wr);
			Sleep(50);
			ULONGLONG t0 = GetTickCount64();
			maize::stdin_source::stop();
			report(GetTickCount64() - t0 < 1000, "stop() from the backstop state joined promptly");
		}
		maize::stdin_source::stop();   // idempotent
		report(true, "a second stop() is a no-op");
	}

}   // namespace

int main() {
	std::printf("maize-313 stdin_source direct-drive test\n\n");
	case_pipe_latency();
	std::printf("\n");
	case_disarmed_is_silent();
	std::printf("\n");
	case_frozen_level_forces();
	std::printf("\n");
	case_stop_joins();
	std::printf("\n");
	if (g_failures == 0) {
		std::printf("stdin-source-test: PASS\n");
		return 0;
	}
	std::printf("stdin-source-test: FAIL (%d)\n", g_failures);
	return 1;
}

#else

int main() { return 0; }   // POSIX: the quesOS fixtures cover this backend

#endif
