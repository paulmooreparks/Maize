/* src/perf.h -- maize-222: per-device performance-counter registry.
 *
 * Replaces the earlier ad-hoc --show-perf reports (one in maize.cpp for headless /
 * console runs, one in the SDL display loop) with a single registry: each device that
 * has performance counters registers a `source` when it becomes active, and at exit
 * `emit()` walks the registered sources and prints one section each. A run with no
 * display registers no display source, so it prints no FPS section.
 *
 * Averages are device-owned (a counter over the elapsed wall-clock). Peak MIPS/FPS
 * are sampled by the SDL display event loop (the ~500ms sampler) and written into the
 * source's peak field; a headless run has no sampler, so it reports averages only.
 */
#ifndef MAIZE_PERF_H
#define MAIZE_PERF_H

#include <cstdint>
#include <ostream>
#include <functional>

namespace maize {
	namespace perf {

		struct source {
			virtual ~source() = default;
			virtual const char* section() const = 0;
			virtual void report(std::ostream& out, std::uint64_t elapsed_us) const = 0;
		};

		void reset();                    // clear the registry and stamp the run-start time
		void add(source* s);             // register a source (must outlive emit())
		std::uint64_t elapsed_us();      // microseconds since reset()
		void emit(std::ostream& out);    // "maize: performance report" + one block per source

		/* CPU: instructions retired + average MIPS. Always active under --show-perf. */
		struct cpu_source : source {
			int peak_mips = 0;           // set by the display sampler; 0 => averages only
			const char* section() const override { return "cpu"; }
			void report(std::ostream& out, std::uint64_t elapsed_us) const override;
		};

		/* Display: frames presented + average FPS. Registered ONLY when a window is
		   attached, so a headless / console run omits it (no FPS section). The frame count
		   is read through a getter so this header need not know the framebuffer type. */
		struct display_source : source {
			std::function<std::uint64_t()> frames;   // returns the present count
			int peak_fps = 0;                        // set by the display sampler
			const char* section() const override { return "display"; }
			void report(std::ostream& out, std::uint64_t elapsed_us) const override;
		};

	} // namespace perf
} // namespace maize

#endif // MAIZE_PERF_H
