// mzvm_main.cpp (maize-418): the command-line entry point for the Maize v2 virtual machine.
//
// The v2 machine is a clean break from v1, so this binary shares nothing with src/maize.cpp.
// It loads a flat image of instruction bytes into bare-mode physical memory at a chosen
// address, sets the program counter there, and runs until the machine halts or stops.
//
// There is no loader and no boot-information block here yet: those arrive with the rest of the
// machine. What exists is enough to run a program that ends in `halt`, to report where it got
// to, and, since maize-451, to emit whatever that program wrote to the console.
//
// STDOUT BELONGS TO THE GUEST. Every diagnostic this binary produces about the run itself goes
// to stderr, and the only thing written to stdout by default is the bytes the guest's console
// emitted. A fixture can therefore assert the guest's exact output rather than searching for it
// inside a status line, and a shell pipeline gets the program's output and nothing else.
// `--registers` is the one exception and is opt-in.

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "interpreter_v2.h"
#include "memory_v2.h"

namespace {

// Write the guest's console bytes to standard output with no transformation of any kind.
//
// On Windows a stream opened in text mode translates a line feed into a carriage return and a
// line feed on the way out, so a guest that emitted one byte would have two arrive. That is not a
// cosmetic difference: the console's contract is a byte-at-a-time output stream, and a machine
// that silently doubles a byte is not delivering the stream the guest wrote. _setmode with
// _O_BINARY is the documented CRT call that turns the translation off, and the mode is restored
// afterwards so the `--registers` dump keeps the host's line-ending convention.
void write_console_bytes(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return;
    }
#ifdef _WIN32
    std::fflush(stdout);
    const int previous_mode = _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
#ifdef _WIN32
    if (previous_mode != -1) {
        _setmode(_fileno(stdout), previous_mode);
    }
#endif
}

constexpr std::size_t kDefaultMemoryBytes = 1u << 20;  // 1 MiB
constexpr std::uint64_t kDefaultLoadAddress = 0x1000;

void print_usage(std::FILE* stream) {
    std::fprintf(stream,
                 "usage: mzvm [options] <image>\n"
                 "\n"
                 "Run a Maize v2 program. The image is a flat file of instruction bytes; it is\n"
                 "loaded into memory at the load address and execution starts there.\n"
                 "\n"
                 "options:\n"
                 "  --memory <bytes>   size of physical memory (default 1048576)\n"
                 "  --load-at <addr>   address to load the image at (default 0x1000)\n"
                 "  --start <addr>     address to start executing at (default the load address)\n"
                 "  --max-steps <n>    stop after n instructions (default 100000000, 0 for no limit)\n"
                 "  --registers        print the register file when the machine stops\n"
                 "  -h, --help         print this message\n"
                 "\n"
                 "The machine runs with paging off. It carries the machine block at port $0000\n"
                 "and the console class at ports $0010 through $001F, and no other device class,\n"
                 "so what the guest writes to the console port reaches standard output. Loading a\n"
                 "boot image, floating point and system instructions are not supported yet.\n");
}

bool parse_number(const char* text, std::uint64_t& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (end == text || *end != '\0') {
        return false;
    }
    out = static_cast<std::uint64_t>(value);
    return true;
}

bool read_file(const char* path, std::vector<std::uint8_t>& bytes) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    std::uint8_t buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        bytes.insert(bytes.end(), buffer, buffer + got);
    }
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

const char* cause_name(std::uint8_t cause_number) {
    switch (cause_number) {
        case maize::v2::cause::kIllegalInstruction: return "illegal instruction";
        case maize::v2::cause::kIllegalOperand: return "illegal operand";
        case maize::v2::cause::kDivideError: return "divide error";
        case maize::v2::cause::kBreakpoint: return "breakpoint";
        case maize::v2::cause::kPrivilegedOperation: return "privileged operation";
        case maize::v2::cause::kSyscall: return "syscall";
        case maize::v2::cause::kPhysicalMemoryFault: return "physical-memory fault";
        default: return "unknown cause";
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t memory_bytes = kDefaultMemoryBytes;
    std::uint64_t load_address = kDefaultLoadAddress;
    std::uint64_t start_address = 0;
    bool start_given = false;
    std::uint64_t max_steps = 100000000u;
    bool dump_registers = false;
    const char* image_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const bool has_value = (i + 1) < argc;
        if (argument == "-h" || argument == "--help") {
            print_usage(stdout);
            return 0;
        } else if (argument == "--registers") {
            dump_registers = true;
        } else if (argument == "--memory" && has_value) {
            if (!parse_number(argv[++i], memory_bytes) || memory_bytes == 0) {
                std::fprintf(stderr, "mzvm: --memory needs a positive size\n");
                return 2;
            }
        } else if (argument == "--load-at" && has_value) {
            if (!parse_number(argv[++i], load_address)) {
                std::fprintf(stderr, "mzvm: --load-at needs an address\n");
                return 2;
            }
        } else if (argument == "--start" && has_value) {
            if (!parse_number(argv[++i], start_address)) {
                std::fprintf(stderr, "mzvm: --start needs an address\n");
                return 2;
            }
            start_given = true;
        } else if (argument == "--max-steps" && has_value) {
            if (!parse_number(argv[++i], max_steps)) {
                std::fprintf(stderr, "mzvm: --max-steps needs a count\n");
                return 2;
            }
        } else if (!argument.empty() && argument[0] == '-') {
            std::fprintf(stderr, "mzvm: unrecognized option '%s'\n", argument.c_str());
            print_usage(stderr);
            return 2;
        } else if (image_path == nullptr) {
            image_path = argv[i];
        } else {
            std::fprintf(stderr, "mzvm: more than one image named\n");
            return 2;
        }
    }

    if (image_path == nullptr) {
        print_usage(stderr);
        return 2;
    }

    std::vector<std::uint8_t> image;
    if (!read_file(image_path, image)) {
        std::fprintf(stderr, "mzvm: cannot read '%s'\n", image_path);
        return 2;
    }

    maize::v2::MemoryV2 memory(static_cast<std::size_t>(memory_bytes));
    if (!memory.load_image(load_address, image.data(), image.size())) {
        std::fprintf(stderr, "mzvm: the image does not fit in memory at the load address\n");
        return 2;
    }

    maize::v2::InterpreterV2 machine(memory, start_given ? start_address : load_address);
    const maize::v2::StepResult result = machine.run(max_steps);

    // The guest's console output, whatever the machine's stopping reason: bytes the guest emitted
    // before a trap or a step limit genuinely left the console, and swallowing them would hide
    // the output of exactly the run a person most wants to see. Written as raw bytes rather than
    // through printf, so an embedded zero byte or a non-UTF-8 byte reaches stdout unreinterpreted.
    write_console_bytes(machine.device_surface().console_output());

    int exit_code = 0;
    switch (result.status) {
        case maize::v2::StepStatus::Halted:
            std::fprintf(stderr, "halted at $%016" PRIX64 " after %" PRIu64 " instructions\n",
                         result.pc, machine.steps_taken());
            break;
        case maize::v2::StepStatus::Trapped:
            std::fprintf(stderr,
                         "mzvm: trap %u (%s) subcode %u, aux $%016" PRIX64
                         ", at $%016" PRIX64 "\n",
                         result.trap.cause, cause_name(result.trap.cause), result.trap.subcode,
                         result.trap.aux, result.trap.pc);
            exit_code = 1;
            break;
        case maize::v2::StepStatus::Unimplemented:
            // A scaffold gap, not a guest-visible trap. This build decodes every assigned
            // opcode but executes only the families it owns, so reaching one of the others is
            // a defect in the program or in this build, and it says so loudly rather than
            // returning a plausible-looking trap record.
            std::fprintf(stderr,
                         "mzvm: opcode $%02X at $%016" PRIX64
                         " is not implemented in this build\n",
                         result.opcode, result.pc);
            exit_code = 3;
            break;
        case maize::v2::StepStatus::Advanced:
            std::fprintf(stderr, "mzvm: step limit reached at $%016" PRIX64 "\n", machine.pc());
            exit_code = 1;
            break;
    }

    if (dump_registers) {
        for (unsigned n = 0; n < maize::v2::kRegisterCount; ++n) {
            std::printf("r%-2u $%016" PRIX64 "%s", n, machine.registers().raw(n),
                        (n % 4 == 3) ? "\n" : "  ");
        }
    }

    return exit_code;
}
