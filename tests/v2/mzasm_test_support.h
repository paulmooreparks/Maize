// mzasm_test_support.h (maize-422): the fixture registry for the mzasm suite.
//
// The shape follows maize-418's fixture harness: each fixture is a named function that
// self-registers at static initialization, and the runner takes one name on its command line so
// CMake registers one add_test() per fixture and a failure names the specific behaviour that
// broke rather than a bundled pass or fail.
//
// This runner differs from the interpreter's in one way that matters. Several criteria demand
// evidence about the SHIPPED BINARY rather than about a library call, so those fixtures shell
// out to mzasm.exe and read the file it wrote back off disk. A test that asked the assembler
// in-process what it thinks it emitted would not be testing the same thing.

#ifndef MAIZE_V2_TESTS_MZASM_TEST_SUPPORT_H
#define MAIZE_V2_TESTS_MZASM_TEST_SUPPORT_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace maize::v2::test {

void record_failure(const std::string& message);
int failure_count();

void check(bool condition, const char* expression, const char* file, int line);
void check_equal(std::uint64_t actual, std::uint64_t expected, const char* what, const char* file,
                 int line);
void check_equal_text(const std::string& actual, const std::string& expected, const char* what,
                      const char* file, int line);

#define MZ_CHECK(condition) ::maize::v2::test::check((condition), #condition, __FILE__, __LINE__)
#define MZ_CHECK_EQ(actual, expected) \
    ::maize::v2::test::check_equal((actual), (expected), #actual, __FILE__, __LINE__)
#define MZ_CHECK_TEXT(actual, expected) \
    ::maize::v2::test::check_equal_text((actual), (expected), #actual, __FILE__, __LINE__)

using FixtureFunction = void (*)();

void register_fixture(const char* name, FixtureFunction function);
const std::vector<std::pair<std::string, FixtureFunction>>& fixtures();

struct FixtureRegistrar {
    FixtureRegistrar(const char* name, FixtureFunction function) {
        register_fixture(name, function);
    }
};

#define MZ_FIXTURE(name)                                                             \
    static void name();                                                              \
    static const ::maize::v2::test::FixtureRegistrar name##_registrar(#name, name);  \
    static void name()

// Paths the runner is given on its command line: the mzasm binary under test and the repository
// root, which is where the specification chapters and the shipped asm/v2 sources live.
const std::string& mzasm_path();
const std::string& repo_root();
void set_paths(std::string mzasm, std::string root);

// A scratch directory unique to the running fixture, removed when it goes out of scope.
class ScratchDir {
  public:
    explicit ScratchDir(const std::string& tag);
    ~ScratchDir();
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    std::string file(const std::string& name) const;
    std::string write(const std::string& name, const std::string& contents) const;
    const std::string& path() const { return path_; }

  private:
    std::string path_;
};

struct RunResult {
    int exit_code = 0;
    std::string output;  // standard output and standard error, together
};

RunResult run_mzasm(const std::vector<std::string>& arguments);

// Another binary from the same build directory, named without its host suffix. AC-13 needs mzvm
// to run what mzasm wrote, and mzvm is built beside it.
std::string sibling_binary(const std::string& name);
RunResult run_binary(const std::string& binary, const std::vector<std::string>& arguments);

bool read_file_bytes(const std::string& path, std::vector<std::uint8_t>& out);
bool read_file_text(const std::string& path, std::string& out);
bool file_exists(const std::string& path);

std::string hex_dump(const std::vector<std::uint8_t>& bytes);

}  // namespace maize::v2::test

#endif  // MAIZE_V2_TESTS_MZASM_TEST_SUPPORT_H
