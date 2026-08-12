// interpreter_v2.h (maize-418): the v2 execute stage.
//
// One machine: a register file, a bare-mode physical memory, a program counter. step() runs
// one instruction cycle (fetch, decode, execute, advance the program counter) and reports what
// happened.
//
// Two contracts in here are easy to implement almost-correctly, and both are built
// deliberately rather than discovered later.
//
// TRAP-WRITES-NOTHING. Every instruction that can trap checks BEFORE it computes, never after.
// A divide validates its divisor before touching the destination; a bitfield instruction
// validates width and position before reading or writing a register; a load validates the
// whole access range before writing the destination; a store validates the whole range before
// writing any byte. An implementation that computes first and checks second passes every test
// that does not probe the ordering and fails exactly the ones that do.
//
// THE BLOCK-MEMORY RESTART INVARIANT, WHICH IS DIRECTION-NEUTRAL. At every point the machine
// could stop mid-transfer, the count register holds the bytes NOT YET transferred and each
// pointer register holds the LOWEST address in its region not yet transferred. That is a
// description of the remaining work rather than of a direction of travel, which is what lets a
// low-to-high pass show progress by advancing pointers and decrementing the count together
// while a high-to-low pass shows it by decrementing the count alone. block_copy here uses both
// directions, choosing per overlap the way memmove does, and both converge on the same
// completion state: count zero, each pointer at its original value plus the original count.

#ifndef MAIZE_V2_INTERPRETER_V2_H
#define MAIZE_V2_INTERPRETER_V2_H

#include <cstdint>

#include "decode_v2.h"
#include "memory_v2.h"
#include "registers_v2.h"
#include "trap_v2.h"

namespace maize::v2 {

enum class StepStatus : std::uint8_t {
    Advanced,  // the instruction completed and the program counter moved
    Trapped,   // a guest-visible trap condition; see StepResult::trap
    Halted,    // halt executed; the machine is stopped and its state is final
    // A real assigned opcode whose family this build does not implement (D-2): the floating
    // point band, the system/CSR/TLB/port band other than halt, and breakpoint. This is a HOST
    // diagnostic about a scaffold gap, never a guest-visible trap. Inventing a trap cause for
    // "not implemented yet" would misrepresent the gap as conformant illegal-instruction
    // behaviour, and it would be wrong the moment maize-419 and maize-420 close it.
    Unimplemented,
};

struct StepResult {
    StepStatus status = StepStatus::Advanced;
    TrapV2 trap{};                 // meaningful when status is Trapped
    std::uint8_t opcode = 0;       // the opcode byte this cycle decoded, when it decoded one
    std::uint64_t pc = 0;          // address of the instruction this cycle ran
};

// boot.md: the machine begins in supervisor privilege with paging off. Nothing this build
// implements can leave supervisor, because the only path to user level is trap_return
// (maize-420), so this field is a placeholder that maize-420 replaces wholesale rather than a
// privilege mechanism.
enum class Privilege : std::uint8_t { Supervisor = 0, User = 1 };

class InterpreterV2 {
  public:
    explicit InterpreterV2(MemoryV2& memory, std::uint64_t reset_pc = 0)
        : memory_(memory), pc_(reset_pc) {}

    StepResult step();

    // Run until the machine halts, traps, hits an unimplemented opcode, or exhausts the step
    // budget. The budget exists so a fixture with a runaway loop fails instead of hanging; a
    // budget of zero runs without a bound.
    StepResult run(std::uint64_t max_steps = 0);

    RegistersV2& registers() { return registers_; }
    const RegistersV2& registers() const { return registers_; }
    MemoryV2& memory() { return memory_; }
    const MemoryV2& memory() const { return memory_; }

    std::uint64_t pc() const { return pc_; }
    void set_pc(std::uint64_t value) { pc_ = value; }
    bool halted() const { return halted_; }
    std::uint64_t steps_taken() const { return steps_taken_; }
    Privilege privilege() const { return privilege_; }

  private:
    StepResult execute(const DecodedV2& decoded);

    // Result constructors, so no execute path assembles a StepResult by hand.
    StepResult advance(const DecodedV2& decoded);
    StepResult branch_to(const DecodedV2& decoded, std::uint64_t target);
    StepResult raise(const DecodedV2& decoded, std::uint8_t cause_number,
                     std::uint8_t subcode_number, std::uint64_t aux);

    StepResult execute_load(const DecodedV2& decoded, unsigned width_bytes, bool sign_extended,
                            bool displaced);
    StepResult execute_store(const DecodedV2& decoded, unsigned width_bytes, bool displaced);
    StepResult execute_block(const DecodedV2& decoded);

    MemoryV2& memory_;
    RegistersV2 registers_{};
    std::uint64_t pc_ = 0;
    std::uint64_t steps_taken_ = 0;
    bool halted_ = false;
    Privilege privilege_ = Privilege::Supervisor;
};

// The ten predicates, in the order the compare band, the immediate compare band and the branch
// band all use. Shared so a compare and the branch of the same predicate cannot disagree.
bool evaluate_predicate(unsigned predicate, std::uint64_t left, std::uint64_t right);

// The exact 128-bit product helpers the high-half multiplies need, written out rather than
// taken from a 128-bit integer extension so the interpreter compiles the same way on every
// toolchain the tree supports.
void multiply_full_unsigned(std::uint64_t a, std::uint64_t b, std::uint64_t& low,
                            std::uint64_t& high);
std::uint64_t multiply_high_signed_value(std::uint64_t a, std::uint64_t b);

}  // namespace maize::v2

#endif  // MAIZE_V2_INTERPRETER_V2_H
