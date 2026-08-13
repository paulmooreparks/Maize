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
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "interpreter_v2.h"
#include "memory_v2.h"
#include "mzvm_options.h"

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

// What each numeric option can actually use, which is not always what its type can hold. An
// address and a step count span the whole 64-bit range. A memory size is handed to a
// std::size_t, and on a host whose size_t is narrower than 64 bits a larger value would be
// truncated on the way in, so SIZE_MAX is the point past which the number the program acts on
// would stop being the number the user typed.
//
// Read that ceiling for exactly what it is: a TRUNCATION guard, and nothing more. On a 64-bit
// host SIZE_MAX is UINT64_MAX and this constant rejects nothing, so it is not what keeps an
// unallocatable size from reaching the allocation. Nothing here could be: whether a size can be
// allocated is a fact about the machine the program is running on this minute, and a constant
// picked at compile time either refuses sizes some host could serve or admits sizes the host in
// front of us cannot. main() therefore catches the allocation failure and reports it (maize-467,
// D-1), which is the only place the true answer exists.
constexpr std::uint64_t kMaxAddress = UINT64_MAX;
constexpr std::uint64_t kMaxSteps = UINT64_MAX;
constexpr std::uint64_t kMaxMemoryBytes = static_cast<std::uint64_t>(SIZE_MAX);

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

using maize::v2::parse_number;

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

// Allocate the machine's physical memory, or say why it could not be (maize-467, D-1).
//
// A size that passes every range check can still be one this host cannot give us, and the
// allocation is the only thing that knows. Left to itself the vector throws, nothing catches it,
// and the program dies on an abort with a C++ runtime message about a vector and no mention of
// the option the user typed, which is the same nothing-was-said failure this card exists to
// remove. std::length_error is what a request past the vector's max_size raises and
// std::bad_alloc is what a request the allocator cannot satisfy raises; both mean the memory
// asked for is not available, so both get the sentence.
//
// The memory is owned through a pointer only because the construction has to sit inside a try
// block while the machine below outlives it.
std::unique_ptr<maize::v2::MemoryV2> allocate_memory(std::uint64_t bytes) {
    try {
        return std::make_unique<maize::v2::MemoryV2>(static_cast<std::size_t>(bytes));
    } catch (const std::length_error&) {
    } catch (const std::bad_alloc&) {
    }
    std::fprintf(stderr, "mzvm: cannot allocate %" PRIu64 " bytes of memory for the machine\n",
                 bytes);
    return nullptr;
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
            // The lower bound is the option's own rule rather than a separate test after the
            // fact: a memory of zero bytes is as unusable as one of 2^70, and both are refused
            // by the same sentence.
            if (!parse_number("--memory", "a size", argv[++i], 1, kMaxMemoryBytes, memory_bytes)) {
                return 2;
            }
        } else if (argument == "--load-at" && has_value) {
            if (!parse_number("--load-at", "an address", argv[++i], 0, kMaxAddress, load_address)) {
                return 2;
            }
        } else if (argument == "--start" && has_value) {
            if (!parse_number("--start", "an address", argv[++i], 0, kMaxAddress, start_address)) {
                return 2;
            }
            start_given = true;
        } else if (argument == "--max-steps" && has_value) {
            // Zero is the documented "no limit" spelling, so it is in range here.
            if (!parse_number("--max-steps", "a count", argv[++i], 0, kMaxSteps, max_steps)) {
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

    const std::unique_ptr<maize::v2::MemoryV2> memory_owner = allocate_memory(memory_bytes);
    if (memory_owner == nullptr) {
        return 2;
    }
    maize::v2::MemoryV2& memory = *memory_owner;
    if (!memory.load_image(load_address, image.data(), image.size())) {
        std::fprintf(stderr, "mzvm: the image does not fit in memory at the load address\n");
        return 2;
    }

    maize::v2::InterpreterV2 machine(memory, start_given ? start_address : load_address);
    maize::v2::StepResult result = machine.run(max_steps);

    // A DELIVERED trap is a stopping point for this host and not for the machine (maize-464):
    // run() hands control back at the delivery so a caller can see it, and the machine's program
    // counter is already on the handler. Keep going until something actually stops it, which is
    // a halt, the step budget, or a trap that could not be delivered.
    while (result.status == maize::v2::StepStatus::Trapped &&
           result.disposition == maize::v2::TrapDisposition::Delivered) {
        if (max_steps != 0 && machine.steps_taken() >= max_steps) {
            break;
        }
        result = machine.run(max_steps == 0 ? 0 : max_steps - machine.steps_taken());
    }

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
            // Reaching here means the trap was NOT delivered, since the loop above runs on past
            // every one that was. Which of the two undeliverable outcomes it is decides what a
            // reader should go and look at: a zero vector-table entry is a missing handler, and
            // a double fault is a trap stack or vector table the machine could not reach.
            std::fprintf(stderr,
                         "mzvm: trap %u (%s) subcode %u, aux $%016" PRIX64
                         ", at $%016" PRIX64 "\n",
                         result.trap.cause, cause_name(result.trap.cause), result.trap.subcode,
                         result.trap.aux, result.trap.pc);
            std::fprintf(stderr, "mzvm: %s\n",
                         result.disposition == maize::v2::TrapDisposition::HaltedDoubleFault
                             ? "double fault: the vector read or the frame push could not be "
                               "performed, and the machine halted"
                             : "no handler installed for that cause, and the machine halted");
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
        case maize::v2::StepStatus::Suspended:
            // wait_for_interrupt with no cause that could ever become pending and enabled. The
            // machine is doing exactly what the chapter requires and the specification does not
            // bound how long a wait takes, so this is a host diagnostic in the same family as the
            // unimplemented-opcode report rather than a guest-visible trap. Saying so beats
            // spinning until a person kills the process or an outer timeout does.
            std::fprintf(stderr,
                         "mzvm: wait_for_interrupt at $%016" PRIX64
                         " can never complete, because no enabled cause is pending and no device "
                         "has anything scheduled\n",
                         result.pc);
            exit_code = 1;
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
