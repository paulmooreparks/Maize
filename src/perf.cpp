/* src/perf.cpp -- maize-222: per-device performance-counter registry (see perf.h). */

#include "perf.h"
#include "maize.h"      // maize::cpu::instruction_count()
#include "stdin_source.h"

#include <chrono>
#include <vector>

namespace maize {
	namespace perf {

		namespace {
			std::vector<source*> g_sources;
			std::chrono::steady_clock::time_point g_start;

			/* Lines use CRLF so the report renders correctly when written into the maize text
			   console (which needs CR to return to column 0), and is harmless on a host terminal. */
			void emit_elapsed(std::ostream& out, std::uint64_t us) {
				if (us < 10000) { out << "    elapsed      : " << us << " us\r\n"; }
				else            { out << "    elapsed      : " << (us / 1000) << " ms\r\n"; }
			}
		}

		void reset() {
			g_sources.clear();
			g_start = std::chrono::steady_clock::now();
		}

		void add(source* s) { g_sources.push_back(s); }

		std::uint64_t elapsed_us() {
			return static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - g_start).count());
		}

		void emit(std::ostream& out) {
			std::uint64_t us = elapsed_us();
			out << "maize: performance report\r\n";
			for (source* s : g_sources) {
				out << "  [" << s->section() << "]\r\n";
				s->report(out, us);
			}
		}

		void cpu_source::report(std::ostream& out, std::uint64_t us) const {
			std::uint64_t insns = static_cast<std::uint64_t>(maize::cpu::instruction_count());
			/* insn / microsecond == million-insn / second == MIPS. */
			long long avg = us ? static_cast<long long>(insns / us) : 0;
			out << "    instructions : " << insns << "\r\n";
			emit_elapsed(out, us);
			out << "    MIPS         : " << avg << " avg";
			if (peak_mips) { out << " / " << peak_mips << " peak"; }
			out << "\r\n";
		}

		void stdin_source_report::report(std::ostream& out, std::uint64_t /*us*/) const {
			maize::cpu::park_counters p = maize::cpu::read_park_counters();
			maize::stdin_source::counters c = maize::stdin_source::snapshot();
			/* The first two are equal exactly when every path into the park runs the park
			   hook first. A fixture compares them; printing them side by side is what lets a
			   human see WHICH one moved when they diverge. */
			out << "    tick returns : " << p.tick_returns << "\r\n";
			out << "    park hooks   : " << p.hook_calls << "\r\n";
			out << "    relatches    : " << p.relatch_expiries << "\r\n";
			out << "    infinite wait: " << c.infinite_waits << "\r\n";
			out << "    backstop wait: " << c.backstop_waits << "\r\n";
			out << "    level raises : " << c.level_raises << "\r\n";
			out << "    forced raises: " << c.forced_raises << "\r\n";
			out << "    idle raises  : " << c.idle_raises << "\r\n";
		}

		void display_source::report(std::ostream& out, std::uint64_t us) const {
			std::uint64_t f = frames ? frames() : 0;
			long long avg = us ? static_cast<long long>(f * 1000000ull / us) : 0;
			out << "    frames       : " << f << "\r\n";
			out << "    FPS          : " << avg << " avg";
			if (peak_fps) { out << " / " << peak_fps << " peak"; }
			out << "\r\n";
		}

	} // namespace perf
} // namespace maize
