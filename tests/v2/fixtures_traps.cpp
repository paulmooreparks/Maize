// fixtures_traps.cpp (maize-464): the trap model, against trap-model.md "Fault, trap, and
// interrupt" through "Restartability", with execution-model.md's program-counter and
// restartability sections read alongside it.
//
// WHAT THESE FIXTURES ASSERT IS THE ENUMERATION, not that a trap was taken. A machine with every
// cause misnumbered still reaches a handler, so a fixture that checked only "control arrived
// somewhere" would pass on it. Every fixture below names the cause number, the subcode, the
// auxiliary word and the captured program counter it expects.
//
// FOUR THINGS HERE ARE DELIBERATE BREAKS FROM MAIZE v1, and each one is the shape a port
// reproduces from habit rather than from the chapter:
//
//   1. THE FRAME SAVES NO GENERAL REGISTER. It is four words: the captured program counter, the
//      status word, the cause word, the auxiliary word. Maize v1 pushed thirteen registers on
//      the way into every handler and popped them on the way out, and v2 has no equivalent and
//      no instruction that saves a fixed register set. registers_survive_a_trap_untouched is the
//      fixture, and it checks all thirty-two.
//   2. THE CAUSES ARE RENUMBERED. Every number in this file is the v2 number from "The cause
//      enumeration", and no v1 handler-table index appears anywhere in it.
//   3. THE PAGE FAULT SPLITS THREE WAYS BY ACCESS KIND, causes 8 on fetch, 9 on load and 10 on
//      store, where v1 had one page-fault cause. maize-465 brings the translation that raises
//      them; this card builds the delivery they travel through, and
//      page_fault_causes_deliver_through_the_same_mechanism proves that path is cause-generic
//      rather than shaped around the causes bare mode happens to produce.
//   4. CAUSES 5 AND 6 ARE DARK ON PURPOSE, so that a handler table carried over from v1, which
//      spent them on a segment-bounds violation and a stack fault, cannot silently alias an old
//      cause onto a new one. Causes 12 through 31 carry the same guarantee.
//      reserved_cause_is_never_delivered is the fixture, and it is a sweep rather than a
//      spot-check because the claim is about every input rather than about a chosen one.
//
// Cause 11, the physical-memory fault, has no v1 equivalent at all. It reports an access that
// reached a physical address the machine does not populate, it exists in every translation mode,
// and it is the only way a trap-stack address can fail in bare mode, which is what makes the
// bare-mode double fault reachable here.

#include <cstdio>
#include <vector>

#include "fixture_support.h"

namespace maize::v2::test {
namespace {

// The address map these fixtures run on. Populated memory is one contiguous region in this build
// and the boot-information block that will define the real map is maize-421, so the layout is
// chosen here and the sizes are what the chapter requires: the vector table is 2 KiB and
// 2 KiB-aligned, and the trap stack is 16-byte aligned and descends.
constexpr std::uint64_t kMemoryBytes = 0x4000;
constexpr std::uint64_t kProgramBase = 0x100;
constexpr std::uint64_t kHandlerBase = 0x800;
constexpr std::uint64_t kVectorTable = 0x1000;  // through $17FF
constexpr std::uint64_t kTrapStackTop = 0x2000;

// The register the kernel preamble loads its values through. A fixture that cares what the
// general registers hold sets them after the preamble has run, or accounts for this one.
constexpr unsigned kSetupRegister = 1;

constexpr std::uint64_t kSentinel = 0x0123456789ABCDEFull;

// The two instructions a kernel uses to load a control and status register, emitted through the
// guest's own instruction set rather than through a host accessor, so the register's own value
// validation runs on the way in and a fixture cannot install a trap stack the machine would have
// rejected.
void emit_csr_load(Encoder& program, std::uint16_t number, std::uint64_t value,
                   unsigned via = kSetupRegister) {
    program.op_r_i8(op::kMoveW, reg(via), value);
    program.op_r_i2(op::kCsrWrite, reg(via), number);
}

// A machine with a kernel's trap state installed. The preamble runs before any fixture's own
// instruction does, so every fixture below starts from a machine whose trap-vector base and trap
// stack arrived through csr_write.
class Kernel {
  public:
    explicit Kernel(std::uint64_t stack_top = kTrapStackTop) : stack_top_(stack_top) {
        emit_csr_load(program_, csr::kTrapVectorBase, kVectorTable);
        emit_csr_load(program_, csr::kTrapStack, stack_top);
    }

    Machine& machine() { return machine_; }
    Encoder& program() { return program_; }

    // The address the next emitted instruction will occupy, which is how a fixture records the
    // address it expects a fault to capture without a label mechanism.
    std::uint64_t here() const { return program_.current_address(); }
    std::uint64_t stack_top() const { return stack_top_; }

    // Point cause `number`'s vector-table entry at `address`. Host-side memory setup, the same
    // kind of setup as loading the program image, since the table is data the kernel would have
    // laid down before enabling anything.
    void install_handler(std::uint8_t number, std::uint64_t address) {
        machine_.memory().write_little_endian(
            vector_table::entry_address(kVectorTable, number), 8, address);
    }

    // Load an image somewhere other than the main program, for a handler body.
    void load_at(const Encoder& image) {
        V2_CHECK(machine_.memory().load_image(image.base_address(), image.bytes().data(),
                                              image.bytes().size()));
    }

    // Load the program and run the preamble, leaving the program counter on the fixture's own
    // first instruction.
    void start() {
        machine_.load(program_);
        for (unsigned i = 0; i < kPreambleInstructions; ++i) {
            const StepResult result = machine_.step();
            if (result.status != StepStatus::Advanced) {
                record_failure("the kernel preamble did not run");
                return;
            }
        }
        V2_CHECK_EQ(csr().host_read(csr::kTrapVectorBase), kVectorTable);
        V2_CHECK_EQ(csr().host_read(csr::kTrapStack), stack_top_);
    }

    CsrFileV2& csr() { return machine_.interpreter().csr(); }
    std::uint64_t trap_stack() { return csr().host_read(csr::kTrapStack); }
    StepResult step() { return machine_.step(); }
    StepResult run(std::uint64_t budget = 10000) { return machine_.run(budget); }

  private:
    static constexpr unsigned kPreambleInstructions = 4;  // two move.w, two csr_write

    Machine machine_{kMemoryBytes};
    Encoder program_{kProgramBase};
    std::uint64_t stack_top_ = kTrapStackTop;
};

std::uint64_t frame_word(Machine& machine, std::uint64_t frame, std::uint64_t offset) {
    return machine.memory().read_little_endian(frame + offset, 8);
}

// The whole frame, asserted as four exact values at the four fixed offsets, plus the trap-stack
// register's own new value (trap-model.md, "The frame"; "Conformance notes").
void expect_frame(Kernel& kernel, std::uint64_t expected_frame, std::uint64_t pc,
                  std::uint64_t status, std::uint8_t cause_number, std::uint8_t subcode_number,
                  std::uint64_t aux, const char* what) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s: trap_stack", what);
    check_equal_u64(kernel.trap_stack(), expected_frame, buffer, __FILE__, __LINE__);

    Machine& machine = kernel.machine();
    std::snprintf(buffer, sizeof(buffer), "%s: frame pc word", what);
    check_equal_u64(frame_word(machine, expected_frame, trap_frame::kPcOffset), pc, buffer,
                    __FILE__, __LINE__);
    std::snprintf(buffer, sizeof(buffer), "%s: frame status word", what);
    check_equal_u64(frame_word(machine, expected_frame, trap_frame::kStatusOffset), status,
                    buffer, __FILE__, __LINE__);
    std::snprintf(buffer, sizeof(buffer), "%s: frame cause word", what);
    check_equal_u64(frame_word(machine, expected_frame, trap_frame::kCauseOffset),
                    encode_cause_word(cause_number, subcode_number), buffer, __FILE__, __LINE__);
    std::snprintf(buffer, sizeof(buffer), "%s: frame aux word", what);
    check_equal_u64(frame_word(machine, expected_frame, trap_frame::kAuxOffset), aux, buffer,
                    __FILE__, __LINE__);
}

}  // namespace

V2_FIXTURE(trap_cause_enumeration_delivers_exact_values) {
    // THE NUMBERS THEMSELVES, AS LITERALS, against "The cause enumeration". Everything else in
    // this file names the constants from trap_v2.h, which reads better and guards nothing on its
    // own: a machine whose enumeration was shifted, which is the shape a v1-derived port has,
    // moves the constant and every expectation written in terms of it together and passes. This
    // block is the one place the numbers stop being symbols, so a renumbering has to break it.
    V2_CHECK_EQ(cause::kIllegalInstruction, 0);
    V2_CHECK_EQ(cause::kIllegalOperand, 1);
    V2_CHECK_EQ(cause::kDivideError, 2);
    V2_CHECK_EQ(cause::kBreakpoint, 3);
    V2_CHECK_EQ(cause::kPrivilegedOperation, 4);
    V2_CHECK_EQ(cause::kSyscall, 7);
    V2_CHECK_EQ(cause::kPageFaultFetch, 8);
    V2_CHECK_EQ(cause::kPageFaultLoad, 9);
    V2_CHECK_EQ(cause::kPageFaultStore, 10);
    V2_CHECK_EQ(cause::kPhysicalMemoryFault, 11);

    // And the subcodes, for the same reason.
    V2_CHECK_EQ(subcode::kOperandForm, 0);
    V2_CHECK_EQ(subcode::kInvalidImmediate, 1);
    V2_CHECK_EQ(subcode::kReservedRoundingMode, 2);
    V2_CHECK_EQ(subcode::kUnimplementedCsr, 3);
    V2_CHECK_EQ(subcode::kReadOnlyCsr, 4);
    V2_CHECK_EQ(subcode::kBlockMemoryOperands, 5);
    V2_CHECK_EQ(subcode::kInvalidCsrValue, 6);
    V2_CHECK_EQ(subcode::kReservedCsrPrivilege, 7);
    V2_CHECK_EQ(subcode::kDivideByZero, 0);
    V2_CHECK_EQ(subcode::kQuotientOverflow, 1);

    // No cause this machine defines lands on a number the chapter holds dark. Causes 5 and 6 are
    // v1's segment-bounds violation and stack fault, and 12 through 31 are reserved for future
    // synchronous causes, so a constant sitting on any of them is an aliasing defect whatever
    // else the machine gets right.
    const std::uint8_t defined[] = {
        cause::kIllegalInstruction, cause::kIllegalOperand, cause::kDivideError,
        cause::kBreakpoint,         cause::kPrivilegedOperation, cause::kSyscall,
        cause::kPageFaultFetch,     cause::kPageFaultLoad,  cause::kPageFaultStore,
        cause::kPhysicalMemoryFault};
    for (std::uint8_t number : defined) {
        if (number == 5 || number == 6 || (number >= 12 && number <= 31)) {
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer),
                          "a defined cause sits on reserved number %u", number);
            record_failure(buffer);
        }
    }

    // "The cause enumeration", every row this build can provoke, each delivering the cause
    // number, the subcode and the auxiliary word the table assigns, and each capturing the
    // program counter its CLASS assigns: a fault captures its own first byte, a trap in the
    // narrow sense captures the following instruction's address.
    //
    // Causes 8, 9 and 10 are absent because bare mode performs no translation and nothing in
    // this machine can raise them; page_fault_causes_deliver_through_the_same_mechanism carries
    // them, and maize-465 brings the conditions.
    // `prepare` emits whatever the case needs in front of the instruction under test, and `emit`
    // emits that one instruction, so the address recorded between them is the faulting one
    // whatever the setup cost.
    struct Case {
        const char* what;
        std::uint8_t cause;
        std::uint8_t subcode;
        std::uint64_t aux;
        bool trap_class;   // true captures the FOLLOWING instruction, false captures its own
        bool at_user;      // the case needs user level to arise
        void (*prepare)(Kernel&);
        void (*emit)(Kernel&);
    };

    const auto nothing = [](Kernel&) {};

    const Case cases[] = {
        // Cause 0, a reserved opcode byte, with the offending byte zero-extended in the aux word.
        {"illegal instruction", cause::kIllegalInstruction, 0, 0x00, false, false, nothing,
         [](Kernel& k) { k.program().raw_byte(0x00); }},
        // Cause 1, here through an unimplemented control-and-status-register number, whose aux
        // word is the offending register number.
        {"illegal operand", cause::kIllegalOperand, subcode::kUnimplementedCsr, 0x0001, false,
         false, nothing, [](Kernel& k) { k.program().op_r_i2(op::kCsrRead, reg(5), 0x0001); }},
        // Cause 2, whose aux word is zero because the condition is in the subcode.
        {"divide error", cause::kDivideError, subcode::kDivideByZero, 0, false, false, nothing,
         [](Kernel& k) { k.program().op_r_r_r(op::kDivideSigned, reg(4), reg(0), reg(5)); }},
        // Cause 3, trap-class, aux zero.
        {"breakpoint", cause::kBreakpoint, 0, 0, true, false, nothing,
         [](Kernel& k) { k.program().op(op::kBreakpoint); }},
        // Cause 4, whose aux word is the offending opcode byte for an instruction and the
        // register number for a control-and-status-register access.
        {"privileged operation", cause::kPrivilegedOperation, 0, op::kHalt, false, true, nothing,
         [](Kernel& k) { k.program().op(op::kHalt); }},
        // Cause 7, trap-class, carrying the syscall number zero-extended from 8 bits.
        {"syscall", cause::kSyscall, 0, 0x2A, true, false, nothing,
         [](Kernel& k) { k.program().op_i1(op::kSysImm, 0x2A); }},
        // Cause 11, whose aux word is the offending PHYSICAL address rather than the address the
        // instruction started from. In bare mode the two coincide, because bare mode maps every
        // virtual address to itself.
        {"physical-memory fault", cause::kPhysicalMemoryFault, 0, kMemoryBytes, false, false,
         [](Kernel& k) { k.program().op_r_i8(op::kMoveW, reg(6), kMemoryBytes); },
         [](Kernel& k) { k.program().op_r_r(op::kStore, reg(7), reg(6)); }},
    };

    for (const Case& one : cases) {
        Kernel kernel;
        // A handler for every cause, so a machine that vectored through the WRONG entry lands on
        // a handler too and is caught by the frame's cause word rather than by a halt.
        for (unsigned number = 0; number < vector_table::kEntryCount; ++number) {
            kernel.install_handler(static_cast<std::uint8_t>(number), kHandlerBase);
        }
        Encoder handler(kHandlerBase);
        handler.halt();
        kernel.load_at(handler);

        if (one.at_user) {
            emit_csr_load(kernel.program(), csr::kStatus, 0x0);  // user level, interrupts off
        }
        one.prepare(kernel);
        const std::uint64_t faulting = kernel.here();
        one.emit(kernel);
        const std::uint64_t following = kernel.here();
        kernel.start();

        if (one.at_user) {
            // The two preamble-shaped instructions that drop to user level are the fixture's,
            // not the machine's, so they run before the case's own instruction does.
            V2_CHECK(kernel.step().status == StepStatus::Advanced);
            V2_CHECK(kernel.step().status == StepStatus::Advanced);
            V2_CHECK(kernel.machine().interpreter().privilege() == Privilege::User);
        }

        // The cause-11 case builds its address in a register first, so the last instruction
        // emitted is the faulting one and the ones before it just run.
        StepResult result = kernel.step();
        while (result.status == StepStatus::Advanced && kernel.machine().interpreter().pc() <
                                                            following) {
            result = kernel.step();
        }

        const std::uint64_t expected_pc = one.trap_class ? following : faulting;
        expect_trap(result, one.cause, one.subcode, one.aux, expected_pc, one.what);
        expect_disposition(result, TrapDisposition::Delivered, one.what);
        V2_CHECK_EQ(kernel.machine().interpreter().pc(), kHandlerBase);
        expect_frame(kernel, kTrapStackTop - trap_frame::kBytes, expected_pc,
                     one.at_user ? 0x0u : 0x1u, one.cause, one.subcode, one.aux, one.what);
    }
}

V2_FIXTURE(trap_subcodes_are_the_documented_ones) {
    // "Subcodes". Cause 1 defines eight and cause 2 defines two, and every one of them is a
    // condition a handler wants to tell apart from the others under the same cause number.
    //
    // ONE OF THE EIGHT IS OUT OF REACH IN THIS BUILD. Subcode 2 is a reserved floating-point
    // rounding-mode encoding, and it arises when a rounding operation runs, not when the fcsr
    // write that set the encoding runs (floating-point.md is explicit that the write succeeds).
    // The floating-point band is maize-419 and no instruction in this machine rounds, so the
    // condition cannot be constructed here. It is named rather than skipped quietly, and the
    // fcsr write that sets the encoding IS exercised below so the half of the path this build
    // owns is covered.
    struct Case {
        const char* what;
        std::uint8_t cause;
        std::uint8_t subcode;
        std::uint64_t aux;
        void (*prepare)(Kernel&);  // whatever runs in front of the instruction under test
        void (*emit)(Kernel&);
    };

    const auto nothing = [](Kernel&) {};

    const Case cases[] = {
        // Subcode 0, an operand byte whose form field is undefined for its slot class. move's
        // first slot is plain, so any nonzero form is undefined there, and the aux word is the
        // offending operand BYTE rather than the register number inside it.
        {"undefined operand form", cause::kIllegalOperand, subcode::kOperandForm, 0x24, nothing,
         [](Kernel& k) { k.program().raw({op::kMove, 0x24, 0x05}); }},
        // Subcode 1, an immediate the instruction defines as invalid. A bitfield width of zero
        // is one, and the aux word is the width field, since the width alone is the defect.
        {"invalid immediate", cause::kIllegalOperand, subcode::kInvalidImmediate, 0, nothing,
         [](Kernel& k) {
             k.program().op_r_r_i1_i1(op::kBitfieldExtract, reg(4), reg(5), 0, 0);
         }},
        // Subcode 3, a well-formed control-and-status-register number this machine does not
        // implement. Maize v1 read one of these as zero, and that is the defect conformance.md
        // names; the aux word is the register number.
        {"unimplemented register number", cause::kIllegalOperand, subcode::kUnimplementedCsr,
         0x0FFF, nothing, [](Kernel& k) { k.program().op_r_i2(op::kCsrRead, reg(5), 0x0FFF); }},
        // Subcode 4, a write to a read-only register, whose aux word is the register number.
        {"write to a read-only register", cause::kIllegalOperand, subcode::kReadOnlyCsr,
         csr::kBootInfo, nothing,
         [](Kernel& k) { k.program().op_r_i2(op::kCsrWrite, reg(4), csr::kBootInfo); }},
        // Subcode 5, a block-memory encoding naming one register in more than one slot, or
        // naming r0 in a pointer or count slot.
        {"block-memory register aliasing", cause::kIllegalOperand, subcode::kBlockMemoryOperands,
         5, nothing, [](Kernel& k) { k.program().op_r_r_r(op::kBlockCopy, reg(4), reg(5), reg(5)); }},
        {"block-memory r0 in a pointer slot", cause::kIllegalOperand,
         subcode::kBlockMemoryOperands, 0, nothing,
         [](Kernel& k) { k.program().op_r_r_r(op::kBlockCopy, reg(0), reg(5), reg(6)); }},
        // Subcode 6, a value the register does not accept, whose aux word is the offending VALUE
        // rather than the register number. That distinction is the whole difference between this
        // subcode and subcodes 3, 4 and 7.
        {"a value the register rejects", cause::kIllegalOperand, subcode::kInvalidCsrValue, 0x8,
         [](Kernel& k) { k.program().op_r_i8(op::kMoveW, reg(4), 0x8); },
         [](Kernel& k) { k.program().op_r_i2(op::kCsrWrite, reg(4), csr::kStatus); }},
        // Subcode 7, a register number whose privilege field holds a reserved encoding. It traps
        // at supervisor level too, because the number names no level to check an access against.
        {"reserved privilege field", cause::kIllegalOperand, subcode::kReservedCsrPrivilege,
         0x8000, nothing, [](Kernel& k) { k.program().op_r_i2(op::kCsrRead, reg(5), 0x8000); }},
        // Cause 2's two, both with a zero auxiliary word, because the condition is in the
        // subcode and the operands are already in registers the handler can read.
        {"divide by zero", cause::kDivideError, subcode::kDivideByZero, 0, nothing,
         [](Kernel& k) { k.program().op_r_r_r(op::kDivideUnsigned, reg(4), reg(0), reg(5)); }},
        {"quotient overflow", cause::kDivideError, subcode::kQuotientOverflow, 0, nothing,
         [](Kernel& k) { k.program().op_r_r_r(op::kDivideSigned, reg(4), reg(5), reg(6)); }},
    };

    for (const Case& one : cases) {
        Kernel kernel;
        kernel.install_handler(one.cause, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.halt();
        kernel.load_at(handler);

        one.prepare(kernel);
        const std::uint64_t faulting = kernel.here();
        one.emit(kernel);
        const std::uint64_t following = kernel.here();
        kernel.start();
        kernel.machine().set(4, 0x8000000000000000ull);  // the most negative word
        kernel.machine().set(5, 0xFFFFFFFFFFFFFFFFull);  // and -1, for the overflow case

        StepResult result = kernel.step();
        while (result.status == StepStatus::Advanced &&
               kernel.machine().interpreter().pc() < following) {
            result = kernel.step();
        }
        expect_trap(result, one.cause, one.subcode, one.aux, faulting, one.what);
        expect_frame(kernel, kTrapStackTop - trap_frame::kBytes, faulting, 0x1, one.cause,
                     one.subcode, one.aux, one.what);
    }

    // The half of subcode 2's path this build does own: floating-point.md says a reserved
    // rounding-mode encoding is ACCEPTED by the fcsr write and that the next rounding operation
    // is what traps. A machine that rejected the write here would never deliver subcode 2 at
    // all, so this is the check that keeps maize-419's half reachable.
    {
        Kernel kernel;
        emit_csr_load(kernel.program(), csr::kFcsr, 0x00000000000000E0ull, 4);  // frm = %111
        kernel.program().op_r_i2(op::kCsrRead, reg(5), csr::kFcsr);
        kernel.program().halt();
        kernel.start();
        expect_halted(kernel.run(), "a reserved rounding-mode encoding is stored, not rejected");
        V2_CHECK_EQ(kernel.machine().get(5), 0x00000000000000E0ull);
    }
}

V2_FIXTURE(page_fault_causes_deliver_through_the_same_mechanism) {
    // Causes 8, 9 and 10 split the single v1 page fault three ways by ACCESS KIND: 8 on an
    // instruction fetch, 9 on a load or a block-memory read or a page-table read on behalf of
    // one, 10 on a store or a block-memory write. Each uses subcode 0 when translation found no
    // valid mapping and 1 when it found a mapping the access lacks permission for, and each
    // carries the faulting VIRTUAL address in the auxiliary word, exactly as the instruction
    // computed it rather than the page base.
    //
    // Bare mode performs no translation, so no instruction in this build can raise any of them;
    // maize-465 brings the conditions and its own fixtures construct the page tables. What this
    // card owns is the delivery path, and what this fixture proves is that the path is
    // CAUSE-GENERIC: the vector index, the frame's cause word and the auxiliary word are
    // computed from the record rather than from the handful of causes bare mode can produce.
    // Without it, maize-465's first page fault would be the first thing ever to travel this path
    // for those numbers.
    struct Case {
        const char* what;
        std::uint8_t cause;
        std::uint8_t subcode;
    };

    const Case cases[] = {
        {"page fault on fetch, no mapping", cause::kPageFaultFetch, 0},
        {"page fault on fetch, permission", cause::kPageFaultFetch, 1},
        {"page fault on load, no mapping", cause::kPageFaultLoad, 0},
        {"page fault on load, permission", cause::kPageFaultLoad, 1},
        {"page fault on store, no mapping", cause::kPageFaultStore, 0},
        {"page fault on store, permission", cause::kPageFaultStore, 1},
    };

    constexpr std::uint64_t kFaultingVirtual = 0x0000000012345678ull;
    constexpr std::uint64_t kFaultingInstruction = 0x00000000000002A0ull;

    for (const Case& one : cases) {
        Kernel kernel;
        // Distinct handler addresses per cause, so a machine that indexed the table by anything
        // other than the cause number lands somewhere this fixture can name.
        for (unsigned number = 0; number < vector_table::kEntryCount; ++number) {
            kernel.install_handler(static_cast<std::uint8_t>(number),
                                   kHandlerBase + number * 0x10);
        }
        kernel.program().halt();
        kernel.start();

        TrapV2 trap;
        trap.cause = one.cause;
        trap.subcode = one.subcode;
        trap.aux = kFaultingVirtual;
        trap.pc = kFaultingInstruction;  // fault class: the faulting instruction's own address
        const StepResult result = kernel.machine().interpreter().host_deliver_trap(trap);

        expect_disposition(result, TrapDisposition::Delivered, one.what);
        V2_CHECK_EQ(kernel.machine().interpreter().pc(), kHandlerBase + one.cause * 0x10);
        expect_frame(kernel, kTrapStackTop - trap_frame::kBytes, kFaultingInstruction, 0x1,
                     one.cause, one.subcode, kFaultingVirtual, one.what);
    }
}

V2_FIXTURE(physical_memory_fault_reports_the_offending_physical_address) {
    // Cause 11 has no Maize v1 equivalent. It reports an access that reached a physical address
    // no populated region of the address map covers, its auxiliary word is that PHYSICAL
    // address, and its subcode is always 0 because the cause defines one subcode.
    //
    // Populated memory is a single contiguous region in this build and the boot-information
    // block that will define the real map is maize-421, so "above the highest address the map
    // covers" is the region's size here.
    {
        Kernel kernel;
        kernel.install_handler(cause::kPhysicalMemoryFault, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.halt();
        kernel.load_at(handler);

        const std::uint64_t faulting = kernel.here();
        kernel.program().op_r_r(op::kStore, reg(7), reg(6));
        kernel.program().halt();
        kernel.start();
        kernel.machine().set(6, kMemoryBytes);  // one byte above the top of populated memory
        kernel.machine().set(7, kSentinel);

        // Every byte the store would have written, before and after, so "the store leaves memory
        // unchanged" is a comparison rather than a claim. The eight bytes just BELOW the bound
        // are the ones a machine that clamped or wrapped the address would have damaged.
        std::vector<std::uint8_t> before;
        for (std::uint64_t i = kMemoryBytes - 8; i < kMemoryBytes; ++i) {
            before.push_back(kernel.machine().memory().read_byte(i));
        }

        const StepResult result = kernel.step();
        expect_trap(result, cause::kPhysicalMemoryFault, 0, kMemoryBytes, faulting,
                    "a store above the top of populated memory");
        expect_frame(kernel, kTrapStackTop - trap_frame::kBytes, faulting, 0x1,
                     cause::kPhysicalMemoryFault, 0, kMemoryBytes,
                     "a store above the top of populated memory");

        for (std::uint64_t i = kMemoryBytes - 8; i < kMemoryBytes; ++i) {
            V2_CHECK_EQ(kernel.machine().memory().read_byte(i),
                        before[static_cast<std::size_t>(i - (kMemoryBytes - 8))]);
        }
    }

    // A load reports the same cause and the same address, because cause 11 is about the access
    // reaching unpopulated memory rather than about the direction it travels.
    {
        Kernel kernel;
        kernel.install_handler(cause::kPhysicalMemoryFault, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.halt();
        kernel.load_at(handler);

        const std::uint64_t faulting = kernel.here();
        kernel.program().op_r_r(op::kLoad, reg(6), reg(7));
        kernel.program().halt();
        kernel.start();
        kernel.machine().set(6, kMemoryBytes + 0x40);
        kernel.machine().set(7, kSentinel);

        const StepResult result = kernel.step();
        expect_trap(result, cause::kPhysicalMemoryFault, 0, kMemoryBytes + 0x40, faulting,
                    "a load above the top of populated memory");
        V2_CHECK_EQ(kernel.machine().get(7), kSentinel);  // and the destination is untouched
    }
}

V2_FIXTURE(trap_frame_layout_and_trap_stack_discipline) {
    // "The frame". Four words at +0 pc, +8 status, +16 cause, +24 aux from the trap-stack
    // register's value on handler entry, and that value is exactly 32 below its value before the
    // trap. The stack is full-descending, so the machine subtracts first and writes upward from
    // the new value.
    {
        Kernel kernel;
        kernel.install_handler(cause::kBreakpoint, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.halt();
        kernel.load_at(handler);

        const std::uint64_t before_trap = kTrapStackTop;
        const std::uint64_t faulting = kernel.here();
        kernel.program().op(op::kBreakpoint);
        kernel.start();
        V2_CHECK_EQ(kernel.trap_stack(), before_trap);

        // The 32 bytes the frame will occupy, before the trap, so each word is checked to have
        // CHANGED rather than to have happened to match.
        for (std::uint64_t offset = 0; offset < trap_frame::kBytes; ++offset) {
            kernel.machine().memory().write_byte(before_trap - trap_frame::kBytes + offset, 0xEE);
        }

        const StepResult result = kernel.step();
        expect_trap(result, cause::kBreakpoint, 0, 0, faulting + 1, "the frame layout");
        V2_CHECK_EQ(kernel.trap_stack(), before_trap - trap_frame::kBytes);
        expect_frame(kernel, before_trap - trap_frame::kBytes, faulting + 1, 0x1,
                     cause::kBreakpoint, 0, 0, "the frame layout");

        // Nothing above the frame moved. A machine that pushed ascending from the OLD value
        // would have written here instead.
        for (std::uint64_t offset = 0; offset < trap_frame::kBytes; ++offset) {
            V2_CHECK_EQ(kernel.machine().memory().read_byte(before_trap + offset), 0u);
        }
    }

    // The 16-byte alignment requirement, which is mistake-proofing of the same kind that makes a
    // reserved opcode trap rather than execute: a misaligned trap stack is rejected at the
    // csr_write, so no trap can ever be delivered onto one.
    for (std::uint64_t misaligned : {std::uint64_t{0x2008}, std::uint64_t{0x2001},
                                     std::uint64_t{0x200F}}) {
        Kernel kernel;
        kernel.install_handler(cause::kIllegalOperand, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.halt();
        kernel.load_at(handler);

        kernel.program().op_r_i8(op::kMoveW, reg(4), misaligned);
        const std::uint64_t faulting = kernel.here();
        kernel.program().op_r_i2(op::kCsrWrite, reg(4), csr::kTrapStack);
        kernel.start();

        V2_CHECK(kernel.step().status == StepStatus::Advanced);
        const StepResult result = kernel.step();
        expect_trap(result, cause::kIllegalOperand, subcode::kInvalidCsrValue, misaligned,
                    faulting, "a misaligned trap stack");
        // The register kept its aligned value, so the frame this very trap pushed landed where
        // the accepted value put it.
        expect_frame(kernel, kTrapStackTop - trap_frame::kBytes, faulting, 0x1,
                     cause::kIllegalOperand, subcode::kInvalidCsrValue, misaligned,
                     "a misaligned trap stack");
    }

    // A 2 KiB-aligned vector base is required the same way, and for the same reason: the table
    // never straddles more pages than it has to.
    for (std::uint64_t misaligned : {std::uint64_t{0x1001}, std::uint64_t{0x1800 - 1},
                                     std::uint64_t{0x0400}}) {
        Kernel kernel;
        kernel.install_handler(cause::kIllegalOperand, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.halt();
        kernel.load_at(handler);

        kernel.program().op_r_i8(op::kMoveW, reg(4), misaligned);
        const std::uint64_t faulting = kernel.here();
        kernel.program().op_r_i2(op::kCsrWrite, reg(4), csr::kTrapVectorBase);
        kernel.start();

        V2_CHECK(kernel.step().status == StepStatus::Advanced);
        const StepResult result = kernel.step();
        expect_trap(result, cause::kIllegalOperand, subcode::kInvalidCsrValue, misaligned,
                    faulting, "a misaligned vector base");
        V2_CHECK_EQ(kernel.csr().host_read(csr::kTrapVectorBase), kVectorTable);
    }
}

V2_FIXTURE(frame_status_word_is_the_interrupted_context) {
    // "The status word". The word on the frame is a snapshot of the live status register taken
    // BEFORE the machine changes anything, and the layout is priv in bits 1:0, ie in bit 2, and
    // bits 63:3 reserved and read and written as zero.
    //
    // THE PRIVILEGE FIELD IS AT BITS 1:0. A neighbouring chapter puts a privilege field with the
    // same name and the same encodings at bits 15:14 of a control-and-status-register NUMBER,
    // and the two are unrelated. This word is written on every trap entry and restored on every
    // trap_return, so the wrong field position here would be a defect every later card builds on.
    struct Case {
        const char* what;
        std::uint64_t status;  // what the interrupted context runs with
    };

    const Case cases[] = {
        {"supervisor, interrupts off", 0x1},
        {"supervisor, interrupts on", 0x5},
        {"user, interrupts off", 0x0},
        {"user, interrupts on", 0x4},
    };

    for (const Case& one : cases) {
        Kernel kernel;
        kernel.install_handler(cause::kSyscall, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.halt();
        kernel.load_at(handler);

        emit_csr_load(kernel.program(), csr::kStatus, one.status, 4);
        const std::uint64_t faulting = kernel.here();
        kernel.program().op_i1(op::kSysImm, 0x07);
        kernel.start();
        V2_CHECK(kernel.step().status == StepStatus::Advanced);
        V2_CHECK(kernel.step().status == StepStatus::Advanced);
        V2_CHECK_EQ(kernel.csr().host_read(csr::kStatus), one.status);

        const StepResult result = kernel.step();
        expect_trap(result, cause::kSyscall, 0, 0x07, faulting + 2, one.what);
        expect_frame(kernel, kTrapStackTop - trap_frame::kBytes, faulting + 2, one.status,
                     cause::kSyscall, 0, 0x07, one.what);

        // And the LIVE register is now supervisor with interrupts off, whatever it held before,
        // with every other bit unchanged.
        V2_CHECK_EQ(kernel.csr().host_read(csr::kStatus), 0x1u);
        V2_CHECK(kernel.machine().interpreter().privilege() == Privilege::Supervisor);
        V2_CHECK(!kernel.csr().interrupts_enabled());
    }

    // A value naming a reserved privilege encoding, or setting any reserved bit, raises the
    // illegal-operand trap with subcode 6 and changes NOTHING. Both reserved encodings are
    // tested, since %10 is the room the privileged architecture keeps for a third level and %11
    // is reserved with it.
    for (std::uint64_t rejected : {std::uint64_t{0x2}, std::uint64_t{0x3}, std::uint64_t{0x6},
                                   std::uint64_t{0x8}, std::uint64_t{0x8000000000000000ull}}) {
        Kernel kernel;
        kernel.install_handler(cause::kIllegalOperand, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.halt();
        kernel.load_at(handler);

        kernel.program().op_r_i8(op::kMoveW, reg(4), rejected);
        const std::uint64_t faulting = kernel.here();
        kernel.program().op_r_i2(op::kCsrWrite, reg(4), csr::kStatus);
        kernel.start();

        V2_CHECK(kernel.step().status == StepStatus::Advanced);
        const StepResult result = kernel.step();
        expect_trap(result, cause::kIllegalOperand, subcode::kInvalidCsrValue, rejected, faulting,
                    "a rejected status word");
        // The status register still held its old value when the frame was pushed, which the
        // frame's own status word is the evidence for.
        expect_frame(kernel, kTrapStackTop - trap_frame::kBytes, faulting, 0x1,
                     cause::kIllegalOperand, subcode::kInvalidCsrValue, rejected,
                     "a rejected status word");
    }
}

V2_FIXTURE(cause_word_packs_cause_and_subcode) {
    // "The cause word": cause in bits 7:0, subcode in bits 15:8, bits 63:16 written as zero. The
    // chapter says a handler recovers the cause with a mask and the subcode with a shift then the
    // same mask, so the fixture recovers them THAT way, through the guest's own instructions,
    // rather than by reading the word out through the host and doing the arithmetic in C++.
    Kernel kernel;
    kernel.install_handler(cause::kIllegalOperand, kHandlerBase);

    // handler: read the frame, load the cause word, recover both fields, halt.
    Encoder handler(kHandlerBase);
    handler.op_r_i2(op::kCsrRead, reg(20), csr::kTrapStack);
    handler.op_r_r_i2(op::kLoadDisp, reg(20), reg(21), trap_frame::kCauseOffset);
    handler.op_r_r_i4(op::kAndImm, reg(21), reg(22), 0xFF);            // the cause
    handler.op_r_r_i1(op::kShiftRightLogicalImm, reg(21), reg(23), 8);
    handler.op_r_r_i4(op::kAndImm, reg(23), reg(23), 0xFF);            // the subcode
    handler.halt();
    kernel.load_at(handler);

    // An illegal-operand fault with subcode 7, chosen because both fields are nonzero and
    // different, so a machine that packed them the other way round fails rather than agreeing.
    kernel.program().op_r_i2(op::kCsrRead, reg(5), 0x8000);
    kernel.start();

    V2_CHECK(kernel.step().status == StepStatus::Trapped);
    expect_halted(kernel.run(), "the handler that unpacks the cause word");

    const std::uint64_t packed = kernel.machine().get(21);
    V2_CHECK_EQ(packed, encode_cause_word(cause::kIllegalOperand, subcode::kReservedCsrPrivilege));
    V2_CHECK_EQ(kernel.machine().get(22), cause::kIllegalOperand);
    V2_CHECK_EQ(kernel.machine().get(23), subcode::kReservedCsrPrivilege);
    // Bits 63:16 are zero, so a handler that tests the whole word against a constant behaves the
    // same on every conforming machine.
    V2_CHECK_EQ(packed >> 16, 0u);
}

V2_FIXTURE(auxiliary_word_is_zero_where_the_table_says_zero) {
    // "The auxiliary word": a cause with nothing to report writes zero, and zero is a REAL value
    // rather than an unspecified one, so a conformance binary tests for it. The frame word is
    // poisoned before each trap, so a machine that left the aux word untouched fails here while
    // passing any check that only looked at the cause.
    struct Case {
        const char* what;
        std::uint8_t cause;
        std::uint8_t subcode;
        void (*emit)(Encoder&);
    };

    const Case cases[] = {
        {"breakpoint", cause::kBreakpoint, 0, [](Encoder& e) { e.op(op::kBreakpoint); }},
        {"divide by zero", cause::kDivideError, subcode::kDivideByZero,
         [](Encoder& e) { e.op_r_r_r(op::kDivideUnsigned, reg(4), reg(0), reg(5)); }},
        {"quotient overflow", cause::kDivideError, subcode::kQuotientOverflow,
         [](Encoder& e) { e.op_r_r_r(op::kDivideSigned, reg(4), reg(5), reg(6)); }},
    };

    for (const Case& one : cases) {
        Kernel kernel;
        kernel.install_handler(one.cause, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.halt();
        kernel.load_at(handler);

        one.emit(kernel.program());
        kernel.start();
        kernel.machine().set(4, 0x8000000000000000ull);
        kernel.machine().set(5, 0xFFFFFFFFFFFFFFFFull);
        kernel.machine().memory().write_little_endian(
            kTrapStackTop - trap_frame::kBytes + trap_frame::kAuxOffset, 8, kSentinel);

        V2_CHECK(kernel.step().status == StepStatus::Trapped);
        V2_CHECK_EQ(frame_word(kernel.machine(), kTrapStackTop - trap_frame::kBytes,
                               trap_frame::kAuxOffset),
                    0u);
    }
}

V2_FIXTURE(vectored_dispatch_follows_the_chapters_order) {
    // "Vectored dispatch". Delivery proceeds in a fixed order and every step is observable:
    //
    //   1. the cause, subcode and auxiliary value are determined,
    //   2. the handler address is read from the vector table,
    //   3. a zero entry halts instead of delivering,
    //   4. the four-word frame is pushed,
    //   5. the privilege level goes to supervisor and the interrupt-enable bit is cleared,
    //   6. the program counter takes the handler address.
    //
    // Steps 2 and 4 are ordered so an uninstalled handler is a clean halt with the machine's
    // state untouched rather than a halt with a half-built frame in memory, which
    // no_handler_installed_halts_with_kind_one checks from the other side. Steps 4 and 5 are
    // ordered so the frame carries the INTERRUPTED context's status word, which is what
    // trap_return restores and therefore the whole of how a machine gets back to user level.

    // Entry `c` lives at the base plus `c` times 8, and the table has 256 of them. Every cause
    // this build can provoke gets a DIFFERENT handler address, so a machine that indexed by
    // anything else lands somewhere the fixture names rather than somewhere plausible.
    const std::uint8_t causes[] = {cause::kIllegalInstruction, cause::kIllegalOperand,
                                   cause::kDivideError,        cause::kBreakpoint,
                                   cause::kSyscall,            cause::kPhysicalMemoryFault};
    for (std::uint8_t number : causes) {
        Kernel kernel;
        for (unsigned entry = 0; entry < vector_table::kEntryCount; ++entry) {
            kernel.install_handler(static_cast<std::uint8_t>(entry), kHandlerBase + entry * 0x10);
        }
        kernel.program().op(op::kBreakpoint);
        kernel.start();

        TrapV2 trap;
        trap.cause = number;
        trap.pc = kernel.here();
        const StepResult result = kernel.machine().interpreter().host_deliver_trap(trap);
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "the vector entry for cause %u", number);
        expect_disposition(result, TrapDisposition::Delivered, buffer);
        check_equal_u64(kernel.machine().interpreter().pc(), kHandlerBase + number * 0x10, buffer,
                        __FILE__, __LINE__);
        check_equal_u64(result.handler, kHandlerBase + number * 0x10, buffer, __FILE__, __LINE__);
    }

    // The whole sequence on one trap, with what each step changed checked against what it was.
    {
        Kernel kernel;
        kernel.install_handler(cause::kSyscall, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.halt();
        kernel.load_at(handler);

        emit_csr_load(kernel.program(), csr::kPagingRoot, 0x0000000000003001ull, 4);  // Sv48
        emit_csr_load(kernel.program(), csr::kStatus, 0x5, 4);  // supervisor, interrupts ON
        const std::uint64_t faulting = kernel.here();
        kernel.program().op_i1(op::kSysImm, 0x11);
        kernel.start();
        for (unsigned i = 0; i < 4; ++i) {
            V2_CHECK(kernel.step().status == StepStatus::Advanced);
        }

        const std::uint64_t paging_root_before = kernel.csr().host_read(csr::kPagingRoot);
        const std::uint64_t flushes_before = kernel.csr().translation_flushes();
        const std::uint64_t vector_base_before = kernel.csr().host_read(csr::kTrapVectorBase);

        const StepResult result = kernel.step();
        expect_trap(result, cause::kSyscall, 0, 0x11, faulting + 2, "the delivery sequence");
        expect_frame(kernel, kTrapStackTop - trap_frame::kBytes, faulting + 2, 0x5,
                     cause::kSyscall, 0, 0x11, "the delivery sequence");
        V2_CHECK_EQ(kernel.machine().interpreter().pc(), kHandlerBase);
        V2_CHECK_EQ(kernel.csr().host_read(csr::kStatus), 0x1u);

        // "Address translation stays on across the whole sequence, and the paging root does not
        // change, so the handler runs in the interrupted context's address space." A kernel that
        // wants a separate address space per handler switches the root as its first act, which
        // is software policy rather than machine behaviour.
        V2_CHECK_EQ(kernel.csr().host_read(csr::kPagingRoot), paging_root_before);
        V2_CHECK_EQ(kernel.csr().translation_flushes(), flushes_before);
        V2_CHECK_EQ(kernel.csr().host_read(csr::kTrapVectorBase), vector_base_before);
    }
}

V2_FIXTURE(no_handler_installed_halts_with_kind_one) {
    // "No handler installed". A vector-table entry of zero means no handler is installed for
    // that cause. The machine halts: it pushes no frame, changes no register other than the
    // halt-cause register, and executes nothing further. That is what makes a bare-metal program
    // debuggable, since a divide by zero with no kernel under it stops the machine with cause 2
    // recorded rather than wandering into whatever follows.
    Kernel kernel;
    // A handler for every cause EXCEPT the one about to fire, so the fixture proves the machine
    // consulted the right entry rather than finding an empty table.
    for (unsigned entry = 0; entry < vector_table::kEntryCount; ++entry) {
        kernel.install_handler(static_cast<std::uint8_t>(entry), kHandlerBase);
    }
    kernel.install_handler(cause::kDivideError, 0);

    const std::uint64_t faulting = kernel.here();
    kernel.program().op_r_r_r(op::kDivideUnsigned, reg(4), reg(0), reg(5));
    kernel.program().halt();
    kernel.start();
    kernel.machine().set(4, 0x2A);
    kernel.machine().set(5, kSentinel);

    // Everything the halt is required not to touch, recorded first.
    std::uint64_t registers_before[kRegisterCount];
    for (unsigned n = 0; n < kRegisterCount; ++n) {
        registers_before[n] = kernel.machine().get(n);
    }
    const std::uint64_t trap_stack_before = kernel.trap_stack();
    const std::uint64_t status_before = kernel.csr().host_read(csr::kStatus);
    for (std::uint64_t offset = 0; offset < trap_frame::kBytes; ++offset) {
        kernel.machine().memory().write_byte(trap_stack_before - trap_frame::kBytes + offset,
                                             0xEE);
    }

    const StepResult result = kernel.step();
    expect_trap(result, cause::kDivideError, subcode::kDivideByZero, 0, faulting,
               "a divide error with no handler installed");
    expect_disposition(result, TrapDisposition::HaltedNoHandler, "no handler installed");
    expect_halt_cause(kernel.machine(), halt_cause::kKindNoHandler, cause::kDivideError,
                      subcode::kDivideByZero, "no handler installed");

    // No frame: the 32 bytes below the trap-stack register still hold the poison.
    for (std::uint64_t offset = 0; offset < trap_frame::kBytes; ++offset) {
        V2_CHECK_EQ(kernel.machine().memory().read_byte(trap_stack_before - trap_frame::kBytes +
                                                        offset),
                    0xEEu);
    }
    // No register other than the halt-cause register.
    V2_CHECK_EQ(kernel.trap_stack(), trap_stack_before);
    V2_CHECK_EQ(kernel.csr().host_read(csr::kStatus), status_before);
    for (unsigned n = 0; n < kRegisterCount; ++n) {
        V2_CHECK_EQ(kernel.machine().get(n), registers_before[n]);
    }
    // And nothing further executes, including the halt instruction sitting after the divide,
    // which would have recorded kind 0 over the top of kind 1 had it run.
    expect_halted(kernel.step(), "a machine that halted on an uninstalled handler");
    expect_halt_cause(kernel.machine(), halt_cause::kKindNoHandler, cause::kDivideError,
                      subcode::kDivideByZero, "after a further step");
}

V2_FIXTURE(double_fault_halts_with_the_original_cause) {
    // "Nested traps and double faults". A double fault is a page fault or a physical-memory
    // fault raised by the VECTOR-TABLE READ or by any of the FOUR FRAME STORES. The machine does
    // not attempt to deliver it, because delivering it would take the same failing path again.
    // It halts, records the ORIGINAL cause and subcode with a kind of 2, and executes nothing
    // further.
    //
    // In bare mode there is no page fault, so cause 11 is the only way a bad trap-stack or
    // vector-table address can fail, which is what makes both cases below reachable here at all.

    // The frame stores fail: the trap stack sits above populated memory.
    {
        Kernel kernel(kMemoryBytes + 0x100);  // a legal, 16-byte-aligned, unpopulated address
        kernel.install_handler(cause::kBreakpoint, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.halt();
        kernel.load_at(handler);

        const std::uint64_t faulting = kernel.here();
        kernel.program().op(op::kBreakpoint);
        kernel.start();

        const StepResult result = kernel.step();
        expect_trap(result, cause::kBreakpoint, 0, 0, faulting + 1,
                    "a breakpoint whose frame push cannot be written");
        expect_disposition(result, TrapDisposition::HaltedDoubleFault, "an unwritable trap stack");
        // The ORIGINAL cause, not cause 11 of the frame-push failure. A handler debugging this
        // machine wants to know what it was trying to service; the kind field already says the
        // trap stack was the problem.
        expect_halt_cause(kernel.machine(), halt_cause::kKindDoubleFault, cause::kBreakpoint, 0,
                          "an unwritable trap stack");
        V2_CHECK_EQ(kernel.trap_stack(), kMemoryBytes + 0x100);
        // Nothing here asserts a program-counter value. The chapter says the machine changes no
        // register other than the halt-cause register, and the program counter is explicitly not
        // a register, so no conformance property pins it after a halt and a fixture that pinned
        // one would be inventing a rule (the card's OQ-2).
    }

    // The vector-table read fails, which is the earlier of the two failing steps and carries the
    // same rule.
    {
        Kernel kernel;
        kernel.program().op_r_i8(op::kMoveW, reg(4), kMemoryBytes + 0x800);
        kernel.program().op_r_i2(op::kCsrWrite, reg(4), csr::kTrapVectorBase);
        const std::uint64_t faulting = kernel.here();
        kernel.program().op_r_r_r(op::kDivideSigned, reg(5), reg(0), reg(6));
        kernel.start();
        V2_CHECK(kernel.step().status == StepStatus::Advanced);
        V2_CHECK(kernel.step().status == StepStatus::Advanced);

        const std::uint64_t trap_stack_before = kernel.trap_stack();
        const StepResult result = kernel.step();
        expect_trap(result, cause::kDivideError, subcode::kDivideByZero, 0, faulting,
                    "a divide error whose vector entry cannot be read");
        expect_disposition(result, TrapDisposition::HaltedDoubleFault,
                           "an unreadable vector table");
        expect_halt_cause(kernel.machine(), halt_cause::kKindDoubleFault, cause::kDivideError,
                          subcode::kDivideByZero, "an unreadable vector table");
        // The vector read precedes the frame push, so nothing was pushed and the trap-stack
        // register did not move.
        V2_CHECK_EQ(kernel.trap_stack(), trap_stack_before);
    }

    // Nothing else is a double fault. A fault raised by a handler's own instructions, however
    // soon after entry, is an ordinary nested trap, which the next fixture covers, and a fault
    // raised while trap_return pops the frame is an ordinary fault, which
    // trap_return_fault_while_popping_is_an_ordinary_fault covers.
}

V2_FIXTURE(nested_trap_lands_beneath_the_outer_frame) {
    // "Nested traps and double faults". A trap taken inside a handler is an ORDINARY trap: its
    // frame lands 32 bytes beneath the outer frame when the handler has not moved the trap-stack
    // register, and delivery is otherwise identical. The machine's only rule is the mechanical
    // one, that a frame is pushed at the trap-stack register's current value minus 32, always.
    Kernel kernel;
    kernel.install_handler(cause::kSyscall, kHandlerBase);
    kernel.install_handler(cause::kBreakpoint, kHandlerBase + 0x40);

    // The outer handler runs a breakpoint before doing anything else, so the nested trap arrives
    // with the trap-stack register exactly where the machine left it.
    Encoder outer(kHandlerBase);
    outer.op(op::kBreakpoint);
    outer.halt();
    kernel.load_at(outer);

    Encoder inner(kHandlerBase + 0x40);
    inner.halt();
    kernel.load_at(inner);

    const std::uint64_t syscall_at = kernel.here();
    kernel.program().op_i1(op::kSysImm, 0x09);
    kernel.start();

    const StepResult first = kernel.step();
    expect_trap(first, cause::kSyscall, 0, 0x09, syscall_at + 2, "the outer syscall");
    const std::uint64_t outer_frame = kTrapStackTop - trap_frame::kBytes;
    expect_frame(kernel, outer_frame, syscall_at + 2, 0x1, cause::kSyscall, 0, 0x09,
                 "the outer frame");

    const StepResult second = kernel.step();
    expect_trap(second, cause::kBreakpoint, 0, 0, kHandlerBase + 1, "the nested breakpoint");
    expect_disposition(second, TrapDisposition::Delivered, "the nested breakpoint");
    // Exactly 32 bytes beneath, and the outer frame is untouched underneath it.
    const std::uint64_t inner_frame = outer_frame - trap_frame::kBytes;
    expect_frame(kernel, inner_frame, kHandlerBase + 1, 0x1, cause::kBreakpoint, 0, 0,
                 "the nested frame");
    V2_CHECK_EQ(frame_word(kernel.machine(), outer_frame, trap_frame::kPcOffset), syscall_at + 2);
    V2_CHECK_EQ(frame_word(kernel.machine(), outer_frame, trap_frame::kCauseOffset),
                encode_cause_word(cause::kSyscall, 0));
    V2_CHECK_EQ(kernel.machine().interpreter().pc(), kHandlerBase + 0x40);
}

V2_FIXTURE(trap_return_is_privileged_and_validates_before_committing) {
    // "Returning from a trap". trap_return is privileged, so a user-mode program cannot forge a
    // frame and return into supervisor mode with it, and executing it at user level raises the
    // privileged-operation fault and touches nothing else.
    {
        Kernel kernel;
        kernel.install_handler(cause::kPrivilegedOperation, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.halt();
        kernel.load_at(handler);

        emit_csr_load(kernel.program(), csr::kStatus, 0x0, 4);  // user level
        const std::uint64_t faulting = kernel.here();
        kernel.program().op(op::kTrapReturn);
        kernel.start();
        const std::uint64_t trap_stack_before = kernel.trap_stack();
        V2_CHECK(kernel.step().status == StepStatus::Advanced);
        V2_CHECK(kernel.step().status == StepStatus::Advanced);

        const StepResult result = kernel.step();
        expect_trap(result, cause::kPrivilegedOperation, 0, op::kTrapReturn, faulting,
                    "trap_return at user level");
        // Nothing was popped: the frame this trap pushed is the FIRST thing on the stack, so the
        // trap-stack register is exactly 32 below where it started rather than 32 above or back
        // where it was.
        V2_CHECK_EQ(kernel.trap_stack(), trap_stack_before - trap_frame::kBytes);
    }

    // Validation comes first and it is TOTAL. A frame whose status word names a reserved
    // privilege encoding or sets a reserved bit raises the illegal-operand trap with subcode 6
    // and changes nothing at all, including the trap-stack register. A frame the machine itself
    // wrote always passes, so only a frame software has edited can fail.
    const std::uint64_t rejected_words[] = {0x2, 0x3, 0x8, 0xFFFFFFFFFFFFFFFFull};
    for (std::uint64_t rejected : rejected_words) {
        Kernel kernel;
        kernel.install_handler(cause::kSyscall, kHandlerBase);
        kernel.install_handler(cause::kIllegalOperand, kHandlerBase + 0x40);

        // The handler edits the frame's status word and returns on it, which is the only way to
        // reach this condition: the machine never writes a word that fails.
        Encoder handler(kHandlerBase);
        handler.op_r_i2(op::kCsrRead, reg(20), csr::kTrapStack);
        handler.op_r_i8(op::kMoveW, reg(21), rejected);
        handler.op_r_r_i2(op::kStoreDisp, reg(21), reg(20), trap_frame::kStatusOffset);
        const std::uint64_t trap_return_at = handler.current_address();
        handler.op(op::kTrapReturn);
        kernel.load_at(handler);

        Encoder second(kHandlerBase + 0x40);
        second.halt();
        kernel.load_at(second);

        kernel.program().op_i1(op::kSysImm, 0x05);
        kernel.start();
        V2_CHECK(kernel.step().status == StepStatus::Trapped);
        const std::uint64_t frame = kernel.trap_stack();
        for (unsigned i = 0; i < 3; ++i) {
            V2_CHECK(kernel.step().status == StepStatus::Advanced);
        }

        const StepResult result = kernel.step();
        // Cause 1 is fault-class, so the captured program counter is the trap_return
        // instruction's own address. The chapter states that rule for the class rather than at
        // this passage, and OQ-1 on the card records that this is derived from it.
        expect_trap(result, cause::kIllegalOperand, subcode::kInvalidCsrValue, rejected,
                    trap_return_at, "trap_return on an edited status word");
        // Nothing at all changed, INCLUDING the trap-stack register, which is 32 below the
        // outer frame because this trap pushed its own rather than 32 above it because the pop
        // partly happened.
        V2_CHECK_EQ(kernel.trap_stack(), frame - trap_frame::kBytes);
        V2_CHECK_EQ(kernel.machine().interpreter().pc(), kHandlerBase + 0x40);
        // The outer frame is still there, with the edited word still in it.
        V2_CHECK_EQ(frame_word(kernel.machine(), frame, trap_frame::kStatusOffset), rejected);
        // And the STATUS REGISTER never took the rejected word, not even momentarily. The new
        // frame is the evidence: its status word is the snapshot the machine took on the way
        // into this fault, so a machine that wrote the word and then validated leaves the
        // rejected value here even though it recovers the privilege level a step later.
        expect_frame(kernel, frame - trap_frame::kBytes, trap_return_at, 0x1,
                     cause::kIllegalOperand, subcode::kInvalidCsrValue, rejected,
                     "the frame the rejected trap_return pushed");
    }
}

V2_FIXTURE(trap_return_restores_and_resumes) {
    // "Returning from a trap". On a valid frame the machine writes the frame's status word into
    // the status register, sets the program counter to the frame's pc word, adds 32 to the
    // trap-stack register, and resumes. The cause and auxiliary words are not read back into
    // anything.
    {
        Kernel kernel;
        kernel.install_handler(cause::kSyscall, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.op(op::kTrapReturn);
        kernel.load_at(handler);

        const std::uint64_t syscall_at = kernel.here();
        kernel.program().op_i1(op::kSysImm, 0x03);
        kernel.program().op_r_i8(op::kMoveW, reg(9), kSentinel);  // the instruction resumed on
        kernel.program().halt();
        kernel.start();

        V2_CHECK(kernel.step().status == StepStatus::Trapped);
        V2_CHECK_EQ(kernel.trap_stack(), kTrapStackTop - trap_frame::kBytes);
        V2_CHECK(kernel.step().status == StepStatus::Advanced);  // the trap_return itself

        // Resumed at the instruction AFTER the sys, because cause 7 is trap-class.
        V2_CHECK_EQ(kernel.machine().interpreter().pc(), syscall_at + 2);
        V2_CHECK_EQ(kernel.trap_stack(), kTrapStackTop);
        V2_CHECK_EQ(kernel.csr().host_read(csr::kStatus), 0x1u);
        expect_halted(kernel.run(), "the resumed program");
        V2_CHECK_EQ(kernel.machine().get(9), kSentinel);
    }

    // Restoring the status word is what returns the machine to USER mode and what re-enables
    // interrupts, since both live in that word, and no separate instruction does either.
    {
        Kernel kernel;
        kernel.install_handler(cause::kSyscall, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.op(op::kTrapReturn);
        kernel.load_at(handler);

        emit_csr_load(kernel.program(), csr::kStatus, 0x4, 4);  // user level, interrupts on
        kernel.program().op_i1(op::kSysImm, 0x03);
        kernel.program().halt();  // privileged, so reaching it at user level proves the level
        kernel.start();
        V2_CHECK(kernel.step().status == StepStatus::Advanced);
        V2_CHECK(kernel.step().status == StepStatus::Advanced);

        V2_CHECK(kernel.step().status == StepStatus::Trapped);
        V2_CHECK(kernel.machine().interpreter().privilege() == Privilege::Supervisor);
        V2_CHECK(!kernel.csr().interrupts_enabled());

        V2_CHECK(kernel.step().status == StepStatus::Advanced);  // trap_return
        V2_CHECK_EQ(kernel.csr().host_read(csr::kStatus), 0x4u);
        V2_CHECK(kernel.machine().interpreter().privilege() == Privilege::User);
        V2_CHECK(kernel.csr().interrupts_enabled());
    }

    // A handler that wants to resume somewhere other than where the trap happened edits the pc
    // word on the frame before executing trap_return. That is how a kernel implements signal
    // delivery, single-stepping and instruction emulation, and it needs no separate mechanism.
    {
        Kernel kernel;
        kernel.install_handler(cause::kBreakpoint, kHandlerBase);

        kernel.program().op(op::kBreakpoint);
        const std::uint64_t skipped = kernel.here();
        kernel.program().op_r_i8(op::kMoveW, reg(8), 0xBAD);  // must not run
        const std::uint64_t resume_at = kernel.here();
        kernel.program().op_r_i8(op::kMoveW, reg(9), kSentinel);
        kernel.program().halt();

        Encoder handler(kHandlerBase);
        handler.op_r_i2(op::kCsrRead, reg(20), csr::kTrapStack);
        handler.op_r_i8(op::kMoveW, reg(21), resume_at);
        handler.op_r_r_i2(op::kStoreDisp, reg(21), reg(20), trap_frame::kPcOffset);
        handler.op(op::kTrapReturn);
        kernel.load_at(handler);

        kernel.start();
        kernel.machine().set(8, kSentinel);
        V2_CHECK(kernel.step().status == StepStatus::Trapped);
        expect_halted(kernel.run(), "a handler that redirected the resumption");
        V2_CHECK_EQ(kernel.machine().get(9), kSentinel);
        V2_CHECK_EQ(kernel.machine().get(8), kSentinel);  // the skipped instruction never ran
        V2_CHECK(skipped != resume_at);
    }
}

V2_FIXTURE(trap_return_fault_while_popping_is_an_ordinary_fault) {
    // "Returning from a trap". A page fault or a physical-memory fault raised while popping the
    // frame abandons the pop, leaves the trap-stack register unchanged, and is delivered as an
    // ORDINARY fault, so the trap_return re-executes cleanly once the fault is serviced. Neither
    // is a double-fault condition, because it occurs at trap_return rather than at trap entry,
    // and that distinction is the whole reason this fixture exists beside the double-fault one.
    Kernel kernel;
    kernel.install_handler(cause::kSyscall, kHandlerBase);
    kernel.install_handler(cause::kPhysicalMemoryFault, kHandlerBase + 0x40);

    // The handler moves the trap-stack register to the very top of populated memory and then
    // returns on it. The POP reads the four words at that address and upward, which is off the
    // end, while the PUSH the resulting fault performs writes the 32 bytes below it, which are
    // populated. That is what makes the ordinary delivery this fixture is about reachable: a
    // trap stack that failed in both directions would double-fault instead, which is
    // double_fault_halts_with_the_original_cause's case rather than this one.
    Encoder handler(kHandlerBase);
    handler.op_r_i8(op::kMoveW, reg(20), kMemoryBytes);
    handler.op_r_i2(op::kCsrWrite, reg(20), csr::kTrapStack);
    const std::uint64_t trap_return_at = handler.current_address();
    handler.op(op::kTrapReturn);
    kernel.load_at(handler);

    Encoder second(kHandlerBase + 0x40);
    second.halt();
    kernel.load_at(second);

    kernel.program().op_i1(op::kSysImm, 0x06);
    kernel.program().halt();
    kernel.start();

    V2_CHECK(kernel.step().status == StepStatus::Trapped);  // the syscall
    V2_CHECK(kernel.step().status == StepStatus::Advanced);
    V2_CHECK(kernel.step().status == StepStatus::Advanced);

    const StepResult result = kernel.step();
    expect_trap(result, cause::kPhysicalMemoryFault, 0, kMemoryBytes, trap_return_at,
                "a trap_return whose frame cannot be read");
    // ORDINARY delivery, not a halt: the machine is running and a frame was pushed. A machine
    // that treated this as a double fault would have stopped instead, and the chapter is
    // explicit that it is not one, because it occurs at trap_return rather than at trap entry.
    expect_disposition(result, TrapDisposition::Delivered,
                       "a trap_return whose frame cannot be read");
    V2_CHECK(!kernel.machine().interpreter().halted());
    V2_CHECK_EQ(kernel.machine().interpreter().pc(), kHandlerBase + 0x40);
    // The pop was abandoned, so the trap-stack register still named the value the handler
    // installed when the fault's own frame was pushed 32 below it, rather than 32 above it as a
    // completed pop would leave it.
    V2_CHECK_EQ(kernel.trap_stack(), kMemoryBytes - trap_frame::kBytes);

    // Serviced, the trap_return re-executes cleanly. The host stands in for the kernel that
    // makes the region populated, which is what the fault handler would have done, and then puts
    // the machine back on the trap_return with the register it left unchanged.
    kernel.machine().memory().host_set_size(kMemoryBytes + 0x400);
    kernel.machine().interpreter().set_pc(trap_return_at);
    kernel.csr().machine_set_trap_stack(kMemoryBytes);
    kernel.machine().memory().write_little_endian(kMemoryBytes + trap_frame::kPcOffset, 8,
                                                  kProgramBase);
    kernel.machine().memory().write_little_endian(kMemoryBytes + trap_frame::kStatusOffset, 8,
                                                  0x1);
    const StepResult again = kernel.step();
    V2_CHECK(again.status == StepStatus::Advanced);
    V2_CHECK_EQ(kernel.machine().interpreter().pc(), kProgramBase);
    V2_CHECK_EQ(kernel.trap_stack(), kMemoryBytes + trap_frame::kBytes);
}

V2_FIXTURE(registers_survive_a_trap_untouched) {
    // "Registers across a trap". The machine saves no general-purpose register on a trap and
    // restores none on trap_return. Every one of r0 through r31 holds, at the first instruction
    // of the handler, exactly what it held when the trap fired.
    //
    // MAIZE v1 PUSHED THIRTEEN REGISTERS INTO EVERY HANDLER and popped them on the way out. A
    // port that carries that prologue forward traps at the right instruction with the right
    // cause and corrupts the interrupted program, which is why this fixture checks all
    // thirty-two rather than a sample.
    Kernel kernel;
    kernel.install_handler(cause::kSyscall, kHandlerBase);
    Encoder handler(kHandlerBase);
    handler.op(op::kTrapReturn);
    kernel.load_at(handler);

    kernel.program().op_i1(op::kSysImm, 0x0C);
    kernel.program().halt();
    kernel.start();

    // A distinct value per register, so a machine that saved and restored the wrong one, or
    // shuffled them, fails rather than agreeing by symmetry.
    for (unsigned n = 1; n < kRegisterCount; ++n) {
        kernel.machine().set(n, kSentinel ^ (std::uint64_t{n} << 56) ^ n);
    }
    std::uint64_t before[kRegisterCount];
    for (unsigned n = 0; n < kRegisterCount; ++n) {
        before[n] = kernel.machine().get(n);
    }

    V2_CHECK(kernel.step().status == StepStatus::Trapped);
    // At the handler's first instruction. Nothing was saved, so nothing moved.
    for (unsigned n = 0; n < kRegisterCount; ++n) {
        if (kernel.machine().get(n) != before[n]) {
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer), "r%u changed on the way into the handler", n);
            check_equal_u64(kernel.machine().get(n), before[n], buffer, __FILE__, __LINE__);
        }
    }
    // The handler's own work then shows up in the interrupted program, because trap_return
    // restores nothing either. A machine that restored thirteen registers would undo this.
    kernel.machine().set(4, 0xC0FFEE);
    V2_CHECK(kernel.step().status == StepStatus::Advanced);  // trap_return
    V2_CHECK_EQ(kernel.machine().get(4), 0xC0FFEEu);
    for (unsigned n = 0; n < kRegisterCount; ++n) {
        if (n == 4) {
            continue;
        }
        if (kernel.machine().get(n) != before[n]) {
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer), "r%u changed across trap_return", n);
            check_equal_u64(kernel.machine().get(n), before[n], buffer, __FILE__, __LINE__);
        }
    }
}

V2_FIXTURE(syscall_boundary_carries_number_and_arguments) {
    // "The syscall boundary". sys raises cause 7, captures the address of the following
    // instruction, and copies the syscall number into the auxiliary word zero-extended from 8
    // bits, so a dispatcher reads the number off the frame without decoding the instruction that
    // made the call.
    struct Case {
        const char* what;
        std::uint64_t operand;   // the immediate, or the register's value
        std::uint64_t expected;  // the syscall number that reaches the frame
        bool register_form;
    };

    const Case cases[] = {
        {"sys #0", 0, 0, false},
        {"sys #42", 42, 42, false},
        {"sys #255", 0xFF, 0xFF, false},
        {"sys rs", 0x0C, 0x0C, true},
        // The register form takes the LOW BYTE and ignores the upper 56 bits rather than
        // rejecting them, so a computed syscall number pays for no range test.
        {"sys rs with a dirty upper half", 0xFFFFFFFFFFFFFF2Aull, 0x2A, true},
    };

    for (const Case& one : cases) {
        Kernel kernel;
        kernel.install_handler(cause::kSyscall, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.op(op::kTrapReturn);
        kernel.load_at(handler);

        const std::uint64_t syscall_at = kernel.here();
        if (one.register_form) {
            kernel.program().op_r(op::kSysReg, reg(10));
        } else {
            kernel.program().op_i1(op::kSysImm, one.operand);
        }
        const std::uint64_t following = kernel.here();
        kernel.program().halt();
        kernel.start();
        kernel.machine().set(10, one.operand);

        // The calling convention's argument registers, r2 through r9, hold the arguments when
        // sys executes and still hold them at the handler's first instruction.
        for (unsigned n = 2; n <= 9; ++n) {
            kernel.machine().set(n, 0xA0 + n);
        }

        const StepResult result = kernel.step();
        expect_trap(result, cause::kSyscall, 0, one.expected, following, one.what);
        expect_frame(kernel, kTrapStackTop - trap_frame::kBytes, following, 0x1, cause::kSyscall,
                     0, one.expected, one.what);
        for (unsigned n = 2; n <= 9; ++n) {
            V2_CHECK_EQ(kernel.machine().get(n), 0xA0 + n);
        }

        // The result lands in r2, with r3 carrying the second half of a two-word result, and it
        // is still there when the interrupted program resumes, because trap_return restores
        // nothing.
        kernel.machine().set(2, 0x1234);
        kernel.machine().set(3, 0x5678);
        V2_CHECK(kernel.step().status == StepStatus::Advanced);  // trap_return
        V2_CHECK_EQ(kernel.machine().interpreter().pc(), following);
        V2_CHECK_EQ(kernel.machine().get(2), 0x1234u);
        V2_CHECK_EQ(kernel.machine().get(3), 0x5678u);
    }
}

V2_FIXTURE(fault_restart_leaves_no_partial_effect) {
    // "Restartability". For a single-step instruction the contract is simple: the instruction has
    // taken no architectural effect, so its destination register and every byte of memory it
    // would have written are unchanged, and re-executing it after the handler repairs the
    // condition completes it once.
    //
    // The repair here is the handler's own, through the instruction set: the fault is a divide by
    // zero, and the handler fixes the divisor and returns onto the same instruction, which is the
    // shape a kernel servicing a page fault has.
    {
        Kernel kernel;
        kernel.install_handler(cause::kDivideError, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.op_r_i8(op::kMoveW, reg(5), 7);  // repair the divisor
        handler.op(op::kTrapReturn);
        kernel.load_at(handler);

        const std::uint64_t faulting = kernel.here();
        kernel.program().op_r_r_r(op::kDivideUnsigned, reg(4), reg(5), reg(6));
        kernel.program().halt();
        kernel.start();
        kernel.machine().set(4, 70);
        kernel.machine().set(5, 0);
        kernel.machine().set(6, kSentinel);

        const StepResult result = kernel.step();
        expect_trap(result, cause::kDivideError, subcode::kDivideByZero, 0, faulting,
                    "a divide by zero, restarted");
        // The destination is untouched: the fault happened before the instruction took effect.
        V2_CHECK_EQ(kernel.machine().get(6), kSentinel);
        // And the frame's pc word is the faulting instruction's OWN address, which is what makes
        // the return a re-execution rather than a skip.
        V2_CHECK_EQ(frame_word(kernel.machine(), kTrapStackTop - trap_frame::kBytes,
                               trap_frame::kPcOffset),
                    faulting);

        expect_halted(kernel.run(), "the repaired divide");
        V2_CHECK_EQ(kernel.machine().get(6), 10u);  // 70 / 7, computed exactly once
    }

    // "Returning from the handler without repairing the condition delivers the identical trap
    // again", which is the other half of the same property and the one that catches a machine
    // that captured the following instruction's address for a fault.
    {
        Kernel kernel;
        kernel.install_handler(cause::kDivideError, kHandlerBase);
        Encoder handler(kHandlerBase);
        handler.op(op::kTrapReturn);  // repairs nothing
        kernel.load_at(handler);

        const std::uint64_t faulting = kernel.here();
        kernel.program().op_r_r_r(op::kDivideUnsigned, reg(4), reg(0), reg(6));
        kernel.program().halt();
        kernel.start();

        for (unsigned round = 0; round < 3; ++round) {
            const StepResult result = kernel.step();
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "the identical trap, round %u", round);
            expect_trap(result, cause::kDivideError, subcode::kDivideByZero, 0, faulting, buffer);
            check_equal_u64(kernel.trap_stack(), kTrapStackTop - trap_frame::kBytes, buffer,
                            __FILE__, __LINE__);
            V2_CHECK(kernel.step().status == StepStatus::Advanced);  // trap_return
        }
    }
}

V2_FIXTURE(block_memory_fault_restart_through_a_real_handler) {
    // "Restartability", the block-memory half. Those instructions move an unbounded number of
    // bytes, so they define their visible mid-operation register state instead of being
    // all-or-nothing: at every point the machine can stop, the count register holds the bytes not
    // yet transferred and each pointer register holds the LOWEST address in its region not yet
    // transferred. A fault captures the block instruction's OWN address, and re-executing it
    // after the handler runs finishes the job with no byte copied twice and none skipped.
    //
    // fixtures_memory.cpp already pins the register invariant against a machine with no kernel
    // under it. What this fixture adds is the whole loop through a real handler: fault, deliver,
    // repair, trap_return, complete.
    constexpr std::uint64_t kSource = 0x600;
    constexpr std::uint64_t kCount = 0x40;
    const std::uint64_t destination = kMemoryBytes - 0x20;  // half of it lies off the top

    Kernel kernel;
    kernel.install_handler(cause::kPhysicalMemoryFault, kHandlerBase);

    // The handler cannot make memory populated from inside the machine, so it stands in for the
    // part of the repair a kernel does with a page table by pointing the copy at a destination
    // that IS populated, then returning onto the same instruction. The block instruction resumes
    // from the registers it left behind either way, which is the property under test.
    Encoder handler(kHandlerBase);
    handler.op_r_i8(op::kMoveW, reg(5), 0x700 + 0x20);  // the repaired destination
    handler.op(op::kTrapReturn);
    kernel.load_at(handler);

    const std::uint64_t block_at = kernel.here();
    kernel.program().op_r_r_r(op::kBlockCopyForward, reg(4), reg(5), reg(6));
    kernel.program().halt();
    kernel.start();

    for (std::uint64_t i = 0; i < kCount; ++i) {
        kernel.machine().memory().write_byte(kSource + i, static_cast<std::uint8_t>(0x40 + i));
    }
    kernel.machine().set(4, kSource);
    kernel.machine().set(5, destination);
    kernel.machine().set(6, kCount);

    const StepResult result = kernel.step();
    expect_trap(result, cause::kPhysicalMemoryFault, 0, kMemoryBytes, block_at,
                "a block copy that runs off the top of memory");
    // The captured program counter is the BLOCK instruction's own address, not the following
    // instruction's, which is what lets trap_return resume the transfer.
    expect_frame(kernel, kTrapStackTop - trap_frame::kBytes, block_at, 0x1,
                 cause::kPhysicalMemoryFault, 0, kMemoryBytes,
                 "a block copy that runs off the top of memory");
    // 0x20 bytes fit before the boundary, so 0x20 remain and both pointers name the lowest
    // address in their region not yet transferred.
    V2_CHECK_EQ(kernel.machine().get(6), 0x20u);
    V2_CHECK_EQ(kernel.machine().get(4), kSource + 0x20);
    V2_CHECK_EQ(kernel.machine().get(5), destination + 0x20);

    expect_halted(kernel.run(), "the repaired block copy");
    V2_CHECK_EQ(kernel.machine().get(6), 0u);
    // Every byte transferred exactly once: the first half went to the original destination
    // before the fault and the second half to the repaired one after it, and the two halves are
    // consecutive halves of the source with nothing repeated and nothing skipped.
    for (std::uint64_t i = 0; i < 0x20; ++i) {
        V2_CHECK_EQ(kernel.machine().memory().read_byte(destination + i),
                    static_cast<std::uint8_t>(0x40 + i));
        V2_CHECK_EQ(kernel.machine().memory().read_byte(0x700 + 0x20 + i),
                    static_cast<std::uint8_t>(0x40 + 0x20 + i));
    }
}

V2_FIXTURE(reserved_cause_is_never_delivered) {
    // "A reserved cause number is never delivered by any input." Causes 5 and 6 are held empty on
    // purpose, because Maize v1 spent them on a segment-bounds violation and a stack fault and
    // leaving the numbers dark means a handler table carried over from v1 cannot silently alias
    // an old cause onto a new one. Causes 12 through 31 carry the same guarantee for future
    // synchronous causes.
    //
    // The claim is about EVERY input, so the fixture is a sweep rather than a spot-check: every
    // one of the 256 opcode bytes is executed with operand bytes behind it, at both privilege
    // levels, and any trap that arrives must carry a cause this chapter assigns. A machine whose
    // enumeration was shifted by one, which is the shape a v1-derived port has, delivers a
    // reserved number here on the first opcode that traps.
    // The permitted set, written as the chapter's LITERAL numbers rather than as the constants
    // from trap_v2.h. A machine that renumbered a cause onto a dark one would move the constant
    // and this predicate together if the predicate named constants, which would let the very
    // defect this fixture exists to catch redefine what counts as passing.
    auto allowed = [](std::uint8_t number) {
        switch (number) {
            case 0:   // illegal instruction
            case 1:   // illegal operand
            case 2:   // divide error
            case 3:   // breakpoint
            case 4:   // privileged operation
            case 7:   // syscall
            case 8:   // page fault on fetch
            case 9:   // page fault on load
            case 10:  // page fault on store
            case 11:  // physical-memory fault
                return true;
            default:  // 5, 6, 12 through 31, and every interrupt number, none of which this
                      // card's machine can deliver
                return false;
        }
    };

    const Privilege levels[] = {Privilege::Supervisor, Privilege::User};
    for (Privilege level : levels) {
        for (unsigned byte = 0; byte < 256; ++byte) {
            Kernel kernel;
            for (unsigned entry = 0; entry < vector_table::kEntryCount; ++entry) {
                kernel.install_handler(static_cast<std::uint8_t>(entry), kHandlerBase);
            }
            Encoder handler(kHandlerBase);
            handler.halt();
            kernel.load_at(handler);

            // The opcode byte, then six operand-shaped bytes, which is one more than the longest
            // instruction in the base needs. Register 4 and a form field of zero, so a plain slot
            // accepts them and the instruction reaches its own execution rather than stopping at
            // a form-field check every time.
            kernel.program().raw_byte(static_cast<std::uint8_t>(byte));
            for (unsigned i = 0; i < 6; ++i) {
                kernel.program().raw_byte(0x04);
            }
            kernel.program().halt();
            kernel.start();
            if (level == Privilege::User) {
                kernel.machine().interpreter().host_set_privilege(Privilege::User);
            }
            // Operand values that make the arithmetic conditions reachable: a zero divisor is
            // r0, and the most negative word over -1 is the overflow pair.
            kernel.machine().set(4, 0x8000000000000000ull);

            const StepResult result = kernel.step();
            if (result.status == StepStatus::Trapped && !allowed(result.trap.cause)) {
                char buffer[192];
                std::snprintf(buffer, sizeof(buffer),
                              "opcode $%02X at %s level delivered reserved cause %u",
                              byte, level == Privilege::User ? "user" : "supervisor",
                              result.trap.cause);
                record_failure(buffer);
            }
        }
    }

    // The same claim from the other side: a handler table with entries ONLY at the reserved
    // numbers is dead code under v2. Every trap this machine can take finds a zero entry and
    // halts, so no reserved handler ever runs, and the halt-cause register names the cause that
    // would have aliased onto it.
    {
        Kernel kernel;
        for (unsigned number = 5; number <= 6; ++number) {
            kernel.install_handler(static_cast<std::uint8_t>(number), kHandlerBase);
        }
        for (unsigned number = 12; number <= 31; ++number) {
            kernel.install_handler(static_cast<std::uint8_t>(number), kHandlerBase);
        }
        Encoder handler(kHandlerBase);
        handler.halt();
        kernel.load_at(handler);

        kernel.program().op(op::kBreakpoint);
        kernel.start();

        const StepResult result = kernel.step();
        expect_disposition(result, TrapDisposition::HaltedNoHandler,
                           "a table installed only at the reserved numbers");
        expect_halt_cause(kernel.machine(), halt_cause::kKindNoHandler, cause::kBreakpoint, 0,
                          "a table installed only at the reserved numbers");
        // The handler at kHandlerBase is a halt instruction, so had the machine vectored to it
        // the halt-cause register would read kind 0 with a zero cause instead of kind 1 with
        // cause 3. That is the evidence the reserved handler did not run, and it is architectural
        // rather than a claim about where the program counter came to rest.
    }
}

}  // namespace maize::v2::test
