/* src/fault_inject.h -- maize-313: the named fault-injection hook.
 *
 * Three of this card's acceptance criteria are about states that no ordinary run can be
 * steered into: a host stdin source that refuses to start, a Windows console that folds
 * new input into an existing record so its record count never moves, and an end-of-input
 * transition taken with the readiness edge already latched. Each of those is a state the
 * design refuses to DEPEND on being impossible, so each has to be reachable on purpose or
 * the criterion is a claim about a case nobody can build.
 *
 * The hook is one environment variable, MAIZE_FAULT, holding a comma-separated list of
 * token names. It is read once, on first use, and the parsed set is immutable thereafter,
 * so no hot path pays more than one relaxed load. An empty or absent variable leaves every
 * token off, which is every ordinary run and every CI run that does not name a token.
 *
 * The tokens:
 *
 *   no_source     stdin_source::start() returns false without starting anything, which is
 *                 spec section 6's H7 degradation (a failed start).
 *   freeze_level  the Windows watcher's level read answers the acknowledged level forever,
 *                 which is the console-folds-a-record fault the forced raise exists for.
 *   latch_ready   console_device::on_input_tick sets readable_signaled_ at the instant the
 *                 readiness answer goes negative, which builds the "end of input taken with
 *                 the readiness edge already spent" leaf directly.
 *   source_die    the source thread fails its first wait or count, which drives the single
 *                 exit protocol (latch end of input, raise once, return) on a failure path
 *                 rather than on the clean drained-pipe path.
 *   no_pump       cpu::set_active_input installs nothing, so the instruction-tick pump never
 *                 runs and the park hook is the only caller of on_input_tick left. That is
 *                 the isolation the park hook's own criterion needs: with the pump present,
 *                 a guest that retires 16384 instructions between servicing one reader and
 *                 parking again is rescued by the pump, so removing the hook strands the
 *                 second reader only some of the time and the negative control is a race.
 *   fake_wake     the console status port reports CON_STAT_WAKE set whatever the source did.
 *                 Composed with no_source it builds the one state the guest half is unsafe
 *                 in, which is a guest that parks on a VM with nothing able to wake it, and
 *                 that state is the negative control the slow-stdin fixture needs.
 *
 * This is a test hook rather than a policy knob. Nothing in the VM's ordinary behaviour
 * branches on it, and no guest can reach it.
 */
#ifndef MAIZE_FAULT_INJECT_H
#define MAIZE_FAULT_INJECT_H

#include <cstdlib>
#include <cstring>

namespace maize {
	namespace fault {

		enum : unsigned {
			none         = 0u,
			no_source    = 1u << 0,
			freeze_level = 1u << 1,
			latch_ready  = 1u << 2,
			source_die   = 1u << 3,
			fake_wake    = 1u << 4,
			no_pump      = 1u << 5
		};

		namespace detail {
			inline bool token_present(const char* spec, const char* name) {
				std::size_t n = std::strlen(name);
				for (const char* p = spec; *p != '\0'; ) {
					const char* end = std::strchr(p, ',');
					std::size_t len = (end != nullptr)
						? static_cast<std::size_t>(end - p)
						: std::strlen(p);
					if (len == n && std::strncmp(p, name, n) == 0) { return true; }
					if (end == nullptr) { break; }
					p = end + 1;
				}
				return false;
			}

			inline unsigned parse_once() {
				const char* spec = std::getenv("MAIZE_FAULT");
				if (spec == nullptr) { return none; }
				unsigned bits = none;
				if (token_present(spec, "no_source"))    { bits |= no_source; }
				if (token_present(spec, "freeze_level")) { bits |= freeze_level; }
				if (token_present(spec, "latch_ready"))  { bits |= latch_ready; }
				if (token_present(spec, "source_die"))   { bits |= source_die; }
				if (token_present(spec, "fake_wake"))    { bits |= fake_wake; }
				if (token_present(spec, "no_pump"))      { bits |= no_pump; }
				return bits;
			}
		}

		/* True when the named fault is armed for this process. The parse runs once, on the
		   first call, and C++ guarantees that initialization is thread-safe. */
		inline bool armed(unsigned which) {
			static const unsigned bits = detail::parse_once();
			return (bits & which) != 0u;
		}

	}   // namespace fault
}   // namespace maize

#endif   // MAIZE_FAULT_INJECT_H
