// mzasm_test_support.cpp (maize-422): the fixture registry, the assertions, and the runner.

#include "mzasm_test_support.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace maize::v2::test {

namespace {

std::vector<std::pair<std::string, FixtureFunction>>& registry() {
    static std::vector<std::pair<std::string, FixtureFunction>> instance;
    return instance;
}

int g_failures = 0;
std::string g_mzasm_path;
std::string g_repo_root;

// A command line for the host shell. Quoting every argument keeps a path with a space in it
// from splitting, which matters because the repository sits under a user profile directory.
std::string quote(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"') {
            out += "\\\"";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace

void record_failure(const std::string& message) {
    ++g_failures;
    std::cerr << "FAIL: " << message << "\n";
}

int failure_count() { return g_failures; }

void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        std::ostringstream message;
        message << file << ":" << line << ": " << expression;
        record_failure(message.str());
    }
}

void check_equal(std::uint64_t actual, std::uint64_t expected, const char* what, const char* file,
                 int line) {
    if (actual != expected) {
        std::ostringstream message;
        message << file << ":" << line << ": " << what << " is " << actual << " ($" << std::hex
                << std::uppercase << actual << "), expected " << std::dec << expected << " ($"
                << std::hex << std::uppercase << expected << ")";
        record_failure(message.str());
    }
}

void check_equal_text(const std::string& actual, const std::string& expected, const char* what,
                      const char* file, int line) {
    if (actual != expected) {
        std::ostringstream message;
        message << file << ":" << line << ": " << what << " is '" << actual << "', expected '"
                << expected << "'";
        record_failure(message.str());
    }
}

void register_fixture(const char* name, FixtureFunction function) {
    registry().emplace_back(name, function);
}

const std::vector<std::pair<std::string, FixtureFunction>>& fixtures() { return registry(); }

const std::string& mzasm_path() { return g_mzasm_path; }
const std::string& repo_root() { return g_repo_root; }

void set_paths(std::string mzasm, std::string root) {
    g_mzasm_path = std::move(mzasm);
    g_repo_root = std::move(root);
}

ScratchDir::ScratchDir(const std::string& tag) {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec) / ("mzasm-" + tag);
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base, ec);
    path_ = base.string();
}

ScratchDir::~ScratchDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

std::string ScratchDir::file(const std::string& name) const {
    return (std::filesystem::path(path_) / name).string();
}

std::string ScratchDir::write(const std::string& name, const std::string& contents) const {
    const std::string full = file(name);
    std::ofstream out(full, std::ios::binary);
    out << contents;
    out.close();
    return full;
}

std::string sibling_binary(const std::string& name) {
    std::filesystem::path path(g_mzasm_path);
    const std::string suffix = path.extension().string();
    path.replace_filename(name + suffix);
    return path.string();
}

RunResult run_mzasm(const std::vector<std::string>& arguments) {
    return run_binary(g_mzasm_path, arguments);
}

RunResult run_binary(const std::string& binary, const std::vector<std::string>& arguments) {
    std::error_code ec;
    static int sequence = 0;
    const int serial = ++sequence;
    // The two streams are captured to two files rather than to one (maize-451), because a
    // fixture asserting a guest program's exact standard output cannot separate the streams
    // after the fact. `output` is then rebuilt by concatenation, so every fixture written
    // against the combined field keeps working unchanged.
    const std::filesystem::path capture_out =
        std::filesystem::temp_directory_path(ec) /
        ("mzasm-run-output-" + std::to_string(serial) + ".txt");
    const std::filesystem::path capture_err =
        std::filesystem::temp_directory_path(ec) /
        ("mzasm-run-error-" + std::to_string(serial) + ".txt");

    std::ostringstream command;
    command << quote(binary);
    for (const std::string& argument : arguments) {
        command << " " << quote(argument);
    }
    command << " > " << quote(capture_out.string()) << " 2> " << quote(capture_err.string());

    RunResult result;
    // system() hands the string to the host shell, which on Windows means cmd.exe; the whole
    // command is wrapped in one more pair of quotes there because cmd strips the outermost pair.
#ifdef _WIN32
    const std::string full = "\"" + command.str() + "\"";
#else
    const std::string full = command.str();
#endif
    result.exit_code = std::system(full.c_str());
    read_file_text(capture_out.string(), result.standard_output);
    read_file_text(capture_err.string(), result.standard_error);
    result.output = result.standard_output + result.standard_error;
    std::filesystem::remove(capture_out, ec);
    std::filesystem::remove(capture_err, ec);
    return result;
}

bool read_file_bytes(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool read_file_text(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

bool file_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string hex_dump(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) out << " ";
        out << "$" << (bytes[i] < 16 ? "0" : "") << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}

}  // namespace maize::v2::test

int main(int argc, char* argv[]) {
    using namespace maize::v2::test;

    if (argc < 2) {
        std::cerr << "usage: mzasm_tests <fixture-name> <mzasm-path> <repo-root>\n"
                     "       mzasm_tests --verify-names <csv> \n";
        return 2;
    }

    const std::string first = argv[1];

    // The CMake list and the C++ registry have to agree, and neither can quietly drift: a
    // fixture added in C++ but missing from CMake would run under nothing, and a name in CMake
    // with no fixture behind it would pass vacuously.
    if (first == "--verify-names") {
        if (argc < 3) {
            std::cerr << "--verify-names needs the comma-separated list\n";
            return 2;
        }
        std::vector<std::string> declared;
        std::string current;
        for (const char* p = argv[2]; *p != '\0'; ++p) {
            if (*p == ',') {
                if (!current.empty()) declared.push_back(current);
                current.clear();
            } else {
                current.push_back(*p);
            }
        }
        if (!current.empty()) declared.push_back(current);

        for (const auto& [name, function] : fixtures()) {
            (void)function;
            if (std::find(declared.begin(), declared.end(), name) == declared.end()) {
                record_failure("fixture '" + name + "' is registered in C++ but not in CMake");
            }
        }
        for (const std::string& name : declared) {
            const bool found = std::any_of(
                fixtures().begin(), fixtures().end(),
                [&](const std::pair<std::string, FixtureFunction>& entry) {
                    return entry.first == name;
                });
            if (!found) {
                record_failure("fixture '" + name + "' is listed in CMake but not registered");
            }
        }
        return failure_count() == 0 ? 0 : 1;
    }

    if (argc < 4) {
        std::cerr << "usage: mzasm_tests <fixture-name> <mzasm-path> <repo-root>\n";
        return 2;
    }
    set_paths(argv[2], argv[3]);

    for (const auto& [name, function] : fixtures()) {
        if (name == first) {
            function();
            if (failure_count() != 0) {
                std::cerr << failure_count() << " check(s) failed in " << name << "\n";
                return 1;
            }
            std::cout << name << ": ok\n";
            return 0;
        }
    }
    std::cerr << "no fixture named '" << first << "'\n";
    return 2;
}
