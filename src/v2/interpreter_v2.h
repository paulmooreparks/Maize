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
// THE FRAME SAVES NO GENERAL REGISTER (maize-464). Trap delivery pushes exactly four words, the
// captured program counter, the status word, the cause word and the auxiliary word, and touches
// r0 through r31 not at all. Maize v1 pushed thirteen registers into every handler and
// trap_return popped them back; v2 has no equivalent and no instruction that saves a fixed
// register set, so a port that carries v1's prologue forward traps correctly and corrupts the
// interrupted program. Every general register holds, at the handler's first instruction, exactly
// what it held when the trap fired.
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

#include "csr_v2.h"
#include "decode_v2.h"
#include "device_v2.h"
#include "memory_v2.h"
#include "registers_v2.h"
#include "trap_v2.h"

namespace maize::v2 {

enum class StepStatus : std::uint8_t {
    Advanced,  // the instruction completed and the program counter moved
    // A guest-visible trap condition arose this cycle; see StepResult::trap for what it was and
    // StepResult::disposition for what the machine then did with it. Since maize-464 a trap is
    // DELIVERED rather than merely reported, so this status is a stopping point for the host
    // rather than a stopping point for the machine: unless the disposition says the machine
    // halted, the program counter now holds a handler address and run() called again continues
    // in the handler.
    Trapped,
    Halted,    // halt executed; the machine is stopped and its state is final
    // A real assigned opcode whose family this build does not implement (D-2): the floating
    // point band, the system/TLB band other than halt, the three control-and-status-register
    // instructions and the two port instructions,
    // and breakpoint. This is a HOST
    // diagnostic about a scaffold gap, never a guest-visible trap. Inventing a trap cause for
    // "not implemented yet" would misrepresent the gap as conformant illegal-instruction
    // behaviour, and it would be wrong the moment maize-419 and maize-420 close it.
    Unimplemented,
};

// What the machine did with a trap it raised (trap-model.md, "Vectored dispatch", "No handler
// installed", "Nested traps and double faults"). Three outcomes and no fourth: either the
// machine found a handler and entered it, or it could not and stopped, by one of the two routes
// the chapter names. Both halts are terminal and both write the halt-cause register.
enum class TrapDisposition : std::uint8_t {
    None,                // no trap this cycle
    Delivered,           // the frame is pushed and the program counter holds the handler address
    HaltedNoHandler,     // the vector-table entry was zero: halt kind 1, and no frame was written
    HaltedDoubleFault,   // the vector read or a frame store faulted: halt kind 2, original cause
};

struct StepResult {
    StepStatus status = StepStatus::Advanced;
    TrapV2 trap{};                 // meaningful when status is Trapped
    std::uint8_t opcode = 0;       // the opcode byte this cycle decoded, when it decoded one
    std::uint64_t pc = 0;          // address of the instruction this cycle ran
    // Meaningful when status is Trapped. The trap RECORD says what condition arose; this says
    // what became of it, which is a separate question with its own observable consequences.
    TrapDisposition disposition = TrapDisposition::None;
    std::uint64_t handler = 0;     // the handler address, when the disposition is Delivered
};

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

    // The port space (maize-451). Owned the way the register file is owned, so a machine's
    // devices are constructed with the machine and are in their reset state before the first
    // instruction executes, which is what boot.md's device clause requires.
    DeviceSurfaceV2& device_surface() { return devices_; }
    const DeviceSurfaceV2& device_surface() const { return devices_; }

    // The control-and-status-register space (maize-463), owned the way the register file is
    // owned so a machine's architectural state is in its reset state before the first
    // instruction executes.
    CsrFileV2& csr() { return csr_; }
    const CsrFileV2& csr() const { return csr_; }

    std::uint64_t pc() const { return pc_; }
    void set_pc(std::uint64_t value) { pc_ = value; }
    bool halted() const { return halted_; }
    std::uint64_t steps_taken() const { return steps_taken_; }

    // The live privilege level is the status register's privilege field (maize-463). There is
    // no separate copy of it, so a csr_write to status IS a privilege change and cannot be
    // implemented correctly in one place and forgotten in another.
    Privilege privilege() const { return csr_.privilege(); }

    // Host-side, reachable from no instruction, and named the way MemoryV2::host_set_size is
    // named and for the same reason: it stands up a machine state this build has no guest-visible
    // path into. The only path from supervisor down to user is trap_return, which is maize-464,
    // so without this the privileged-operation guards on the privileged instructions and on a
    // supervisor control-and-status-register number would be code that is written and never once
    // executed, which is a guard nobody can distinguish from a missing one.
    void host_set_privilege(Privilege level) { csr_.host_set_privilege(level); }

    // Resume a halted machine at a chosen address. Host-side and reachable from no instruction,
    // in the same family as MemoryV2::host_set_size and for the same reason: a machine that
    // halted because no handler was installed has no guest-visible way back, and a fixture that
    // wants to check "the condition was repaired and the instruction re-executed cleanly" is
    // standing in for the kernel that a real machine would have had. Where a fixture DOES
    // install a handler, the guest-visible path is trap_return and this is not used.
    void host_resume_at(std::uint64_t address) {
        halted_ = false;
        pc_ = address;
    }

    // Run one trap through the delivery sequence without an instruction having raised it
    // (maize-464). Host-side, reachable from no instruction, and it exists because delivery is
    // cause-generic while this build's conditions are not: bare mode performs no translation, so
    // causes 8, 9 and 10 cannot be raised here at all and maize-465 brings the conditions that
    // raise them. Without this seam the page-fault causes would travel a delivery path no test
    // had ever run, and the first thing to run it would be maize-465's own new code.
    StepResult host_deliver_trap(const TrapV2& trap) { return deliver(trap, 0, trap.pc); }

  private:
    StepResult execute(const DecodedV2& decoded);

    // Result constructors, so no execute path assembles a StepResult by hand.
    StepResult advance(const DecodedV2& decoded);
    StepResult branch_to(const DecodedV2& decoded, std::uint64_t target);

    // Raise a FAULT-class condition: the captured program counter is the faulting instruction's
    // own first byte, so a handler that repairs the condition returns and the instruction runs
    // again (trap-model.md, "Fault, trap, and interrupt").
    StepResult raise(const DecodedV2& decoded, std::uint8_t cause_number,
                     std::uint8_t subcode_number, std::uint64_t aux);

    // Raise a TRAP-class condition, which the instruction asked for: the captured program
    // counter is the address of the FOLLOWING instruction, because there is nothing to retry.
    // `sys` and `breakpoint` are the two members of the class in the base.
    StepResult raise_trap_class(const DecodedV2& decoded, std::uint8_t cause_number,
                                std::uint8_t subcode_number, std::uint64_t aux);

    // The delivery sequence itself, in the chapter's fixed order.
    StepResult deliver(const TrapV2& trap, std::uint8_t opcode, std::uint64_t instruction_pc);
    StepResult halt_without_delivering(const TrapV2& trap, std::uint8_t opcode,
                                       std::uint64_t instruction_pc, TrapDisposition disposition,
                                       unsigned halt_kind);

    StepResult execute_trap_return(const DecodedV2& decoded);

    StepResult execute_load(const DecodedV2& decoded, unsigned width_bytes, bool sign_extended,
                            bool displaced);
    StepResult execute_store(const DecodedV2& decoded, unsigned width_bytes, bool displaced);
    StepResult execute_block(const DecodedV2& decoded);
    StepResult execute_csr(const DecodedV2& decoded);

    MemoryV2& memory_;
    RegistersV2 registers_{};
    DeviceSurfaceV2 devices_{};
    CsrFileV2 csr_{};
    std::uint64_t pc_ = 0;
    std::uint64_t steps_taken_ = 0;
    bool halted_ = false;
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
