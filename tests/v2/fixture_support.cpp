// fixture_support.cpp (maize-418): registry, assertions, and the runner entry point.

#include "fixture_support.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace maize::v2::test {
namespace {

int failures = 0;

std::vector<std::pair<std::string, FixtureFunction>>& registry() {
    static std::vector<std::pair<std::string, FixtureFunction>> instance;
    return instance;
}

}  // namespace

void record_failure(const std::string& message) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
}

int failure_count() { return failures; }

void reset_failures() { failures = 0; }

void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        record_failure(std::string(file) + ":" + std::to_string(line) + ": " + expression);
    }
}

void check_equal_u64(std::uint64_t actual, std::uint64_t expected, const char* what,
                     const char* file, int line) {
    if (actual != expected) {
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer),
                      "%s:%d: %s is $%016" PRIX64 " (%" PRIu64 "), expected $%016" PRIX64
                      " (%" PRIu64 ")",
                      file, line, what, actual, actual, expected, expected);
        record_failure(buffer);
    }
}

void register_fixture(const char* name, FixtureFunction function) {
    registry().emplace_back(name, function);
}

const std::vector<std::pair<std::string, FixtureFunction>>& fixtures() { return registry(); }

void expect_trap(const StepResult& result, std::uint8_t cause_number, std::uint8_t subcode_number,
                 std::uint64_t aux, std::uint64_t pc, const char* what) {
    if (result.status != StepStatus::Trapped) {
        record_failure(std::string(what) + ": expected a trap, got status " +
                       std::to_string(static_cast<int>(result.status)));
        return;
    }
    if (result.trap.cause != cause_number || result.trap.subcode != subcode_number ||
        result.trap.aux != aux || result.trap.pc != pc) {
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer),
                      "%s: trap is cause %u subcode %u aux $%" PRIX64 " pc $%" PRIX64
                      ", expected cause %u subcode %u aux $%" PRIX64 " pc $%" PRIX64,
                      what, result.trap.cause, result.trap.subcode, result.trap.aux,
                      result.trap.pc, cause_number, subcode_number, aux, pc);
        record_failure(buffer);
    }
}

void expect_halted(const StepResult& result, const char* what) {
    if (result.status != StepStatus::Halted) {
        record_failure(std::string(what) + ": expected the machine to halt, got status " +
                       std::to_string(static_cast<int>(result.status)));
    }
}

void expect_disposition(const StepResult& result, TrapDisposition disposition, const char* what) {
    if (result.status != StepStatus::Trapped) {
        record_failure(std::string(what) + ": expected a trap, got status " +
                       std::to_string(static_cast<int>(result.status)));
        return;
    }
    if (result.disposition != disposition) {
        record_failure(std::string(what) + ": trap disposition is " +
                       std::to_string(static_cast<int>(result.disposition)) + ", expected " +
                       std::to_string(static_cast<int>(disposition)));
    }
}

void expect_halt_cause(Machine& machine, unsigned kind, std::uint8_t cause_number,
                       std::uint8_t subcode_number, const char* what) {
    const std::uint64_t expected = halt_cause::encode(kind, cause_number, subcode_number);
    const std::uint64_t actual = machine.interpreter().csr().host_read(csr::kHaltCause);
    if (actual != expected) {
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer),
                      "%s: halt_cause is $%016" PRIX64 ", expected $%016" PRIX64
                      " (kind %u, cause %u, subcode %u)",
                      what, actual, expected, kind, cause_number, subcode_number);
        record_failure(buffer);
    }
    if (!machine.interpreter().halted()) {
        record_failure(std::string(what) + ": the machine is still running");
    }
}

void expect_unimplemented(const StepResult& result, std::uint8_t opcode, const char* what) {
    if (result.status != StepStatus::Unimplemented) {
        record_failure(std::string(what) +
                       ": expected the host unimplemented-opcode diagnostic, got status " +
                       std::to_string(static_cast<int>(result.status)));
        return;
    }
    if (result.opcode != opcode) {
        record_failure(std::string(what) + ": diagnostic named opcode " +
                       std::to_string(result.opcode) + ", expected " + std::to_string(opcode));
    }
}

}  // namespace maize::v2::test

namespace {

void print_names() {
    for (const auto& entry : maize::v2::test::fixtures()) {
        std::printf("%s\n", entry.first.c_str());
    }
}

// Close the loop between the C++ registry and the CMake list that registers one ctest entry per
// fixture. A fixture added here but not to the list would otherwise run under nothing, and a
// name in the list with no fixture behind it would pass vacuously.
int verify_names(const std::string& expected_csv) {
    std::set<std::string> expected;
    std::string current;
    for (char c : expected_csv) {
        if (c == ',' || c == ';') {
            if (!current.empty()) {
                expected.insert(current);
            }
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        expected.insert(current);
    }

    std::set<std::string> actual;
    for (const auto& entry : maize::v2::test::fixtures()) {
        if (!actual.insert(entry.first).second) {
            std::fprintf(stderr, "FAIL: fixture '%s' is registered twice\n", entry.first.c_str());
            return 1;
        }
    }

    int problems = 0;
    for (const auto& name : actual) {
        if (expected.count(name) == 0) {
            std::fprintf(stderr, "FAIL: fixture '%s' is not in the CMake fixture list\n",
                         name.c_str());
            ++problems;
        }
    }
    for (const auto& name : expected) {
        if (actual.count(name) == 0) {
            std::fprintf(stderr, "FAIL: CMake lists fixture '%s', which is not registered\n",
                         name.c_str());
            ++problems;
        }
    }
    return problems == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--list") == 0) {
        print_names();
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--verify-names") == 0) {
        return verify_names(argv[2]);
    }

    const char* wanted = (argc >= 2) ? argv[1] : nullptr;
    int ran = 0;
    for (const auto& entry : maize::v2::test::fixtures()) {
        if (wanted != nullptr && entry.first != wanted) {
            continue;
        }
        ++ran;
        entry.second();
    }

    if (wanted != nullptr && ran == 0) {
        std::fprintf(stderr, "FAIL: no fixture named '%s'\n", wanted);
        return 2;
    }

    if (maize::v2::test::failure_count() != 0) {
        std::fprintf(stderr, "%d check(s) failed across %d fixture(s)\n",
                     maize::v2::test::failure_count(), ran);
        return 1;
    }
    std::printf("ok: %d fixture(s)\n", ran);
    return 0;
}
