// fixture_support.h (maize-418): the fixture registry, the assertions, and the machine helper.
//
// Each fixture is a named function that builds a byte sequence, runs it, and asserts on the
// final register and memory state or on the trap record. Fixtures self-register at static
// initialization, and the runner takes one name on its command line, so CMake registers one
// add_test() per fixture and a failure names the specific behaviour that broke rather than a
// bundled pass or fail.

#ifndef MAIZE_V2_TESTS_FIXTURE_SUPPORT_H
#define MAIZE_V2_TESTS_FIXTURE_SUPPORT_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "encode_v2.h"
#include "interpreter_v2.h"
#include "memory_v2.h"

namespace maize::v2::test {

// Failure reporting. A fixture keeps going after a failed check so one run reports every
// mismatch it can find rather than only the first.
void record_failure(const std::string& message);
int failure_count();
void reset_failures();

void check(bool condition, const char* expression, const char* file, int line);
void check_equal_u64(std::uint64_t actual, std::uint64_t expected, const char* what,
                     const char* file, int line);

#define V2_CHECK(condition) \
    ::maize::v2::test::check((condition), #condition, __FILE__, __LINE__)
#define V2_CHECK_EQ(actual, expected) \
    ::maize::v2::test::check_equal_u64((actual), (expected), #actual, __FILE__, __LINE__)

using FixtureFunction = void (*)();

void register_fixture(const char* name, FixtureFunction function);
const std::vector<std::pair<std::string, FixtureFunction>>& fixtures();

struct FixtureRegistrar {
    FixtureRegistrar(const char* name, FixtureFunction function) {
        register_fixture(name, function);
    }
};

#define V2_FIXTURE(name)                                                       \
    static void name();                                                        \
    static const ::maize::v2::test::FixtureRegistrar name##_registrar(#name, name); \
    static void name()

// A machine with an image loaded at a chosen address and the program counter parked there.
// Fixtures load their bytes straight into the flat buffer, since the loader and the
// boot-information block are maize-421.
class Machine {
  public:
    explicit Machine(std::size_t memory_bytes = 4096) : memory_(memory_bytes) {}

    void load(const Encoder& program) {
        const bool ok = memory_.load_image(program.base_address(), program.bytes().data(),
                                           program.bytes().size());
        V2_CHECK(ok);
        interpreter_.set_pc(program.base_address());
    }

    MemoryV2& memory() { return memory_; }
    RegistersV2& registers() { return interpreter_.registers(); }
    InterpreterV2& interpreter() { return interpreter_; }

    void set(unsigned number, std::uint64_t value) { interpreter_.registers().set_raw(number, value); }
    std::uint64_t get(unsigned number) const { return interpreter_.registers().raw(number); }

    StepResult step() { return interpreter_.step(); }
    StepResult run(std::uint64_t budget = 10000) { return interpreter_.run(budget); }

  private:
    MemoryV2 memory_;
    InterpreterV2 interpreter_{memory_, 0};
};

// Assertions on a StepResult, spelled once so every fixture reports the same way.
//
// expect_trap asserts the trap RECORD, which is the cause, the subcode, the auxiliary word and
// the captured program counter. What the machine then did with that record is a separate
// question that expect_disposition and expect_halt_cause below ask, because a machine can get
// the enumeration exactly right and still deliver it to the wrong place.
void expect_trap(const StepResult& result, std::uint8_t cause_number, std::uint8_t subcode_number,
                 std::uint64_t aux, std::uint64_t pc, const char* what);
void expect_halted(const StepResult& result, const char* what);
void expect_unimplemented(const StepResult& result, std::uint8_t opcode, const char* what);
void expect_disposition(const StepResult& result, TrapDisposition disposition, const char* what);

// The halt-cause register, read through the host rather than through csr_read, because the
// machine it describes has stopped and cannot execute a csr_read to report on itself
// (trap-model.md, "No handler installed").
void expect_halt_cause(Machine& machine, unsigned kind, std::uint8_t cause_number,
                       std::uint8_t subcode_number, const char* what);

}  // namespace maize::v2::test

#endif  // MAIZE_V2_TESTS_FIXTURE_SUPPORT_H
