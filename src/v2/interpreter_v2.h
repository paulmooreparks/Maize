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
//
// EVERY GUEST ACCESS GOES THROUGH ONE TRANSLATION PATH (maize-465). The fetch, a load, a store,
// each byte of a block-memory transfer, the vector read and the four frame stores of trap
// delivery all reach memory through plan_access below, which translates first and judges
// physical reachability second. There is no second road to memory, which is the only reason a
// fetch from a non-executable page and a store to a read-only page cannot come to disagree
// about what translation means.

#ifndef MAIZE_V2_INTERPRETER_V2_H
#define MAIZE_V2_INTERPRETER_V2_H

#include <array>
#include <cstdint>

#include "csr_v2.h"
#include "decode_v2.h"
#include "device_v2.h"
#include "memory_v2.h"
#include "registers_v2.h"
#include "translate_v2.h"
#include "trap_v2.h"

namespace maize::v2 {

// One access, translated byte by byte and judged whole before any of it happens (maize-465).
//
// The plan holds a physical address per byte rather than one base address, because an access
// that straddles a page boundary is contiguous in virtual addresses and need not be contiguous
// in physical ones. Judging every byte before touching any is the same trap-writes-nothing
// discipline this file already applied to the physical range check, extended to translation:
// a load whose second page is unmapped writes no destination register, and a store whose second
// page is read-only writes no byte at all.
//
// The largest access the machine makes is the four-word trap frame, so the capacity is 32. The
// assertions below state that in a form the compiler checks, rather than leaving it to a comment
// that goes stale the first time one of those accesses grows.
struct AccessPlanV2 {
    static constexpr unsigned kMaxBytes = 32;
    // The widest single load or store the instruction set has, which is one 64-bit word.
    static constexpr unsigned kMaxDataBytes = 8;
    std::array<std::uint64_t, kMaxBytes> physical{};
    unsigned count = 0;
};

static_assert(trap_frame::kBytes <= AccessPlanV2::kMaxBytes,
              "a trap frame is planned as one access, so the plan must hold all of it");
static_assert(vector_table::kEntryBytes <= AccessPlanV2::kMaxBytes,
              "a vector-table entry is planned as one access");
static_assert(AccessPlanV2::kMaxDataBytes <= AccessPlanV2::kMaxBytes,
              "the widest load or store is planned as one access");

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
    // point band, and wait_for_interrupt, which is the last member of the system band still
    // waiting on its own card. This is a HOST
    // diagnostic about a scaffold gap, never a guest-visible trap. Inventing a trap cause for
    // "not implemented yet" would misrepresent the gap as conformant illegal-instruction
    // behaviour, and it would be wrong the moment maize-419 and maize-420 close it.
    Unimplemented,
    // wait_for_interrupt suspended the machine and no device has anything scheduled, so no
    // cause can ever become pending and the wait can never end (maize-466). The program counter
    // still names the wait_for_interrupt, so a host that arranges a device event and steps again
    // resumes the wait exactly where it stopped.
    //
    // This is a HOST diagnostic, like Unimplemented and unlike a trap. The machine is behaving
    // exactly as trap-model.md's "Waiting" section requires, which is to suspend until some cause
    // has both its pending bit and its enable bit set; the specification simply does not bound
    // how long that takes, and a kernel that waits with nothing armed has genuinely stopped.
    // Reporting it beats spinning, because a fixture that made this mistake would otherwise hang
    // until its timeout rather than name what it did wrong.
    Suspended,
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

    // The translation cache (maize-465), owned the way the register file is owned. It is
    // architecturally invisible, so nothing a guest can execute observes it; this accessor
    // exists for a host inspecting the machine and for the fixtures that check THIS
    // implementation neither over-flushes nor under-flushes, which is a question the chapter's
    // own rules cannot settle.
    TranslatorV2& translator() { return translator_; }
    const TranslatorV2& translator() const { return translator_; }

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

    // Sample every device's interrupt line into the pending registers (maize-466). The machine
    // does this at each instruction boundary on its own account; a fixture calls it to observe
    // the pending state a device has asserted without having to retire an instruction first.
    void host_sample_device_interrupts() { sample_device_interrupts(); }

  private:
    StepResult execute(const DecodedV2& decoded);

    // Translate every byte of one access and judge its physical reachability, before any of the
    // access happens (maize-465). Returns false with `trap`'s cause, subcode and auxiliary word
    // set and its captured program counter left to the raise site.
    //
    // `level` is a parameter rather than being read off the status register, because the machine
    // makes accesses on its own account that are not the running program's: the vector read and
    // the four frame stores of trap delivery are supervisor-privilege accesses whatever level
    // the interrupted program was at (trap-model.md, "Vectored dispatch" step 2 and "The
    // frame").
    bool plan_access(std::uint64_t address, unsigned length, AccessKind kind, Privilege level,
                     AccessPlanV2& plan, TrapV2& trap);
    // The one-byte form, for the block-memory instructions, whose restart contract judges each
    // byte on its own rather than the transfer as a whole.
    bool plan_byte(std::uint64_t address, AccessKind kind, std::uint64_t& physical, TrapV2& trap);

    std::uint64_t read_planned(const AccessPlanV2& plan, unsigned offset, unsigned width) const;
    void write_planned(const AccessPlanV2& plan, unsigned offset, unsigned width,
                       std::uint64_t value);

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

    // External interrupts (maize-466).
    //
    // Every device line asserted right now becomes a pending bit. Called at each instruction
    // boundary and at each block-memory mid-operation boundary, which is where the machine is
    // allowed to notice the world.
    void sample_device_interrupts();
    // The cause the machine would take at a boundary right now, or CsrFileV2::kNoCause. This is
    // the one place the status register's interrupt-enable bit joins the pending and enable
    // bits, because it is the one question that needs all three.
    unsigned deliverable_interrupt() const;
    // Clear the pending bit, then run the ordinary delivery sequence. `resume_pc` is the address
    // the interrupted program resumes at, which is the following instruction's at an ordinary
    // boundary and the block instruction's own at a mid-operation one.
    StepResult deliver_interrupt(unsigned cause_number, std::uint64_t resume_pc);
    StepResult execute_wait_for_interrupt(const DecodedV2& decoded);
    // A block-memory mid-operation boundary: advance the clock, sample, and report the cause the
    // machine would take, or CsrFileV2::kNoCause. `transferred` is the byte count completed so
    // far, which is what selects the boundaries.
    unsigned block_mid_operation_interrupt(std::uint64_t transferred);
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
    TranslatorV2 translator_{};
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
