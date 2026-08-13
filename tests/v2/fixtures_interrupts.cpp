// fixtures_interrupts.cpp (maize-466): external interrupts, against trap-model.md's "External
// interrupts" section and its three subsections, with execution-model.md's "Interrupts and the
// instruction boundary" read alongside it.
//
// DELIVERY IS A TIMING PROPERTY, AND THE OBVIOUS FIXTURE IS WORTHLESS. A test that arms an
// interrupt, runs, and checks that a flag got set passes on a machine that delivers at the wrong
// instruction boundary, on one that delivers the same assertion twice, on one that never
// suspends in wait_for_interrupt, and on one that wakes on a masked cause. Every fixture below
// names the specific wrong machine it exists to catch, and the naming is not decoration: each was
// written by asking what a subtly wrong machine would do differently and then asserting on that
// difference rather than on the outcome the right machine happens to produce.
//
// THREE OBSERVATIONS DO MOST OF THE WORK, and they are worth stating once rather than at each
// site.
//
//   1. THE CAPTURED PROGRAM COUNTER. An interrupt belongs to no instruction, so it captures the
//      address of the instruction the machine has not yet run. That address is the whole
//      difference between a machine that takes interrupts between instructions and one that
//      folds them into an instruction, and it is the only thing that distinguishes them from
//      outside.
//   2. THE PENDING BIT AT THE HANDLER'S FIRST INSTRUCTION. Delivery clears it before the vector
//      fetch, so the delivered cause reads zero there and a cause still waiting reads one. A
//      machine that cleared it later, or that gated the bit on the CPU-level enable, differs here
//      and nowhere else that a binary can see.
//   3. THE ORDER TWO SITES RAN IN. A machine that does not really suspend in wait_for_interrupt
//      is indistinguishable from one that does if the fixture only checks that the handler
//      eventually ran. It is distinguishable the moment the fixture records WHICH ran first.
//
// THE TIMER IS THE INSTRUMENT. device-surface.md says outright that the timer "is the device the
// conformance suite uses to exercise interrupt delivery end to end", and this build counts its
// nanoseconds in retired instructions, so arming it with a period of two instruction-times
// immediately before a chosen instruction puts the delivery boundary exactly after that
// instruction. That is what lets these fixtures name a boundary instead of hoping for one, and
// it is the reason none of them depends on how long anything takes, which conformance.md
// prohibits outright.

#include <cstdio>
#include <vector>

#include "fixture_support.h"

namespace maize::v2::test {
namespace {

// The address map these fixtures run on, laid out the way fixtures_traps.cpp lays its out and
// for the same reasons: the vector table is 2 KiB and 2 KiB-aligned, the trap stack is 16-byte
// aligned and descends, and the boot-information block that will define the real map is
// maize-421.
constexpr std::uint64_t kMemoryBytes = 0x4000;
constexpr std::uint64_t kProgramBase = 0x100;
constexpr std::uint64_t kHandlerBase = 0x800;
constexpr std::uint64_t kVectorTable = 0x1000;  // through $17FF
constexpr std::uint64_t kTrapStackTop = 0x2000;

// The data area. Each cell is a word, and a fixture reads them off memory after the machine has
// halted, which is execution-model.md's own discipline for observing a run.
constexpr std::uint64_t kSaveArea = 0x2400;   // 64 bytes, the handler's own register save area
constexpr std::uint64_t kSequence = 0x2500;   // a counter two sites increment, to record order
constexpr std::uint64_t kMark0 = 0x2508;
constexpr std::uint64_t kMark1 = 0x2510;
constexpr std::uint64_t kMark2 = 0x2518;
constexpr std::uint64_t kMark3 = 0x2520;
constexpr std::uint64_t kBlockSource = 0x2800;
constexpr std::uint64_t kBlockDestination = 0x2C00;

// The two setup registers. A fixture that cares what these hold sets them after the setup has
// run, or accounts for them.
constexpr unsigned kValueRegister = 1;
constexpr unsigned kPortRegister = 30;

// The ports these fixtures drive, spelled as literals rather than composed from the class code,
// because a machine that moved a class block would move a composed constant with it and pass.
// device-surface.md's class table: the console is class 1 based at $0010, the timer is class 3
// based at $0030, and each class's first three offsets are the common skeleton's.
constexpr std::uint16_t kConsoleStatus = 0x0011;
constexpr std::uint16_t kConsoleControl = 0x0012;
constexpr std::uint16_t kConsoleData = 0x0013;
constexpr std::uint16_t kTimerStatus = 0x0031;
constexpr std::uint16_t kTimerControl = 0x0032;
constexpr std::uint16_t kTimerPeriod = 0x0033;
constexpr std::uint16_t kTimerMode = 0x0034;

// The status words a kernel writes, spelled as the literals trap-model.md's "The status word"
// fixes: privilege in bits 1:0 and the interrupt-enable bit at bit 2. NOT bits 15:14, which is
// where a reader of maize-463's spec prose would put the privilege field; the code is right and
// that prose is wrong, and these two literals are what would catch a machine that believed it.
constexpr std::uint64_t kSupervisorInterruptsOff = 0x1;
constexpr std::uint64_t kSupervisorInterruptsOn = 0x5;

// One instruction's worth of the machine's clock. Arming the timer with two of these immediately
// before an instruction puts the delivery boundary immediately after that instruction, which is
// derived in this file's header comment and used by name below so no fixture repeats the
// arithmetic.
constexpr std::uint64_t kOneInstruction = kNanosecondsPerInstruction;
constexpr std::uint64_t kExpireAfterNextInstruction = 2 * kNanosecondsPerInstruction;

constexpr std::uint64_t kSentinel = 0x0123456789ABCDEFull;

// The two instructions a kernel uses to load a control and status register, emitted through the
// guest's own instruction set rather than through a host accessor, so the register's own value
// validation runs on the way in.
void emit_csr_load(Encoder& program, std::uint16_t number, std::uint64_t value,
                   unsigned via = kValueRegister) {
    program.op_r_i8(op::kMoveW, reg(via), value);
    program.op_r_i2(op::kCsrWrite, reg(via), number);
}

// Three instructions, and the LAST of them is the port_out. A fixture that needs the timer armed
// immediately before a chosen instruction relies on that, so nothing may be appended here.
void emit_port_out(Encoder& program, std::uint16_t port, std::uint64_t value) {
    program.op_r_i8(op::kMoveW, reg(kValueRegister), value);
    program.op_r_i8(op::kMoveW, reg(kPortRegister), port);
    program.op_r_r(op::kPortOut, reg(kValueRegister), reg(kPortRegister));
}

void emit_port_in(Encoder& program, std::uint16_t port, unsigned destination) {
    program.op_r_i8(op::kMoveW, reg(kPortRegister), port);
    program.op_r_r(op::kPortIn, reg(kPortRegister), reg(destination));
}

// Arm the timer: period first, then mode. The mode write is what arms, and it is the last
// instruction this emits.
void emit_timer_arm(Encoder& program, std::uint64_t period_ns, std::uint64_t mode) {
    emit_port_out(program, kTimerPeriod, period_ns);
    emit_port_out(program, kTimerMode, mode);
}

// Store a word from a register into an absolute address, through the port register, which is
// free once the setup phase is over.
void emit_store_absolute(Encoder& program, unsigned source, std::uint64_t address,
                         unsigned via = kPortRegister) {
    program.op_r_i8(op::kMoveW, reg(via), address);
    program.op_r_r(op::kStore, reg(source), reg(via));
}

// THE HANDLER PROLOGUE AND EPILOGUE, which are what maize-463's D8 addendum exists for.
//
// An interrupt arrives at an instruction boundary the interrupted program did not choose, and
// the frame saves no general register, so a handler that touches any register has already
// corrupted its victim before it can save anything. csr_swap and the supervisor scratch register
// are the way out: one atomic exchange gives the handler a register it owns and stows the
// interrupted program's value where nothing can reach it. The scratch register holds the address
// of a save area, seeded once at setup, so the first swap costs the handler nothing and buys it
// a pointer it can bank everything else through.
//
// This is emitted rather than described because AC-11 tests the discipline itself: a handler
// that wrote r2 without banking it first is a real, catchable corruption, and it is exactly the
// corruption a syscall handler is allowed to commit and an interrupt handler is not.
constexpr unsigned kHandlerBase2 = 2;  // banked by the swap itself
constexpr unsigned kHandlerScratch3 = 3;
constexpr unsigned kHandlerScratch4 = 4;

void emit_handler_prologue(Encoder& handler) {
    // csr_swap r2 r2 $4009: r2 becomes the save-area address and the scratch register takes the
    // interrupted program's r2. One instruction, and nothing was destroyed by it.
    handler.op_r_r_i2(op::kCsrSwap, reg(kHandlerBase2), reg(kHandlerBase2), csr::kScratch);
    handler.op_r_r_i2(op::kStoreDisp, reg(kHandlerScratch3), reg(kHandlerBase2), 0);
    handler.op_r_r_i2(op::kStoreDisp, reg(kHandlerScratch4), reg(kHandlerBase2), 8);
}

void emit_handler_epilogue(Encoder& handler) {
    handler.op_r_r_i2(op::kLoadDisp, reg(kHandlerBase2), reg(kHandlerScratch3), 0);
    handler.op_r_r_i2(op::kLoadDisp, reg(kHandlerBase2), reg(kHandlerScratch4), 8);
    // The second swap puts the interrupted program's r2 back and returns the save-area address
    // to the scratch register, so the next interrupt finds the machine exactly as this one did.
    handler.op_r_r_i2(op::kCsrSwap, reg(kHandlerBase2), reg(kHandlerBase2), csr::kScratch);
    handler.op(op::kTrapReturn);
}

// Acknowledge the timer, which clears the device's own expiry latch. Uses the two registers the
// prologue banked, so it may only appear between a prologue and an epilogue.
void emit_timer_acknowledge(Encoder& handler) {
    handler.op_r_i8(op::kMoveW, reg(kHandlerScratch3), 1);
    handler.op_r_i8(op::kMoveW, reg(kHandlerScratch4), kTimerStatus);
    handler.op_r_r(op::kPortOut, reg(kHandlerScratch3), reg(kHandlerScratch4));
}

// A machine with a kernel's trap state installed, and with the two devices reachable.
class Kernel {
  public:
    Kernel() {
        emit_csr_load(program_, csr::kTrapVectorBase, kVectorTable);
        emit_csr_load(program_, csr::kTrapStack, kTrapStackTop);
        emit_csr_load(program_, csr::kScratch, kSaveArea);
    }

    Machine& machine() { return machine_; }
    Encoder& program() { return program_; }
    CsrFileV2& csr() { return machine_.interpreter().csr(); }
    DeviceSurfaceV2& devices() { return machine_.interpreter().device_surface(); }
    std::uint64_t here() const { return program_.current_address(); }
    std::uint64_t trap_stack() { return csr().host_read(csr::kTrapStack); }

    void install_handler(std::uint8_t number, std::uint64_t address) {
        machine_.memory().write_little_endian(
            vector_table::entry_address(kVectorTable, number), 8, address);
    }

    void load_at(const Encoder& image) {
        V2_CHECK(machine_.memory().load_image(image.base_address(), image.bytes().data(),
                                              image.bytes().size()));
    }

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
        V2_CHECK_EQ(csr().host_read(csr::kTrapStack), kTrapStackTop);
        V2_CHECK_EQ(csr().host_read(csr::kScratch), kSaveArea);
    }

    StepResult step() { return machine_.step(); }
    StepResult run(std::uint64_t budget = 100000) { return machine_.run(budget); }

    // Run until the machine stops for a reason that is not "a trap was delivered". A delivered
    // trap is a stopping point for the HOST rather than for the machine, so a fixture with a
    // handler asks for the next one rather than treating the first as the end of the run.
    StepResult run_to_halt(unsigned delivery_budget = 64) {
        for (unsigned taken = 0; taken <= delivery_budget; ++taken) {
            const StepResult result = run();
            if (result.status != StepStatus::Trapped ||
                result.disposition != TrapDisposition::Delivered) {
                return result;
            }
        }
        record_failure("the machine delivered more traps than the fixture's budget allowed");
        return StepResult{};
    }

    std::uint64_t word(std::uint64_t address) {
        return machine_.memory().read_little_endian(address, 8);
    }

  private:
    static constexpr unsigned kPreambleInstructions = 6;  // three move.w, three csr_write

    Machine machine_{kMemoryBytes};
    Encoder program_{kProgramBase};
};

std::uint64_t frame_word(Kernel& kernel, std::uint64_t frame, std::uint64_t offset) {
    return kernel.machine().memory().read_little_endian(frame + offset, 8);
}

// The frame an interrupt pushes, asserted as four exact values. The cause word carries the cause
// number and a subcode of zero, and the auxiliary word is zero, which is trap-model.md's cause
// table for rows 32 through 255: "Zero, unless the source defines a subcode word", and no source
// in this build defines one.
void expect_interrupt_frame(Kernel& kernel, std::uint64_t frame, std::uint64_t resume_pc,
                            std::uint64_t status, std::uint8_t cause_number, const char* what) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s: frame pc word", what);
    check_equal_u64(frame_word(kernel, frame, trap_frame::kPcOffset), resume_pc, buffer, __FILE__,
                    __LINE__);
    std::snprintf(buffer, sizeof(buffer), "%s: frame status word", what);
    check_equal_u64(frame_word(kernel, frame, trap_frame::kStatusOffset), status, buffer,
                    __FILE__, __LINE__);
    std::snprintf(buffer, sizeof(buffer), "%s: frame cause word", what);
    check_equal_u64(frame_word(kernel, frame, trap_frame::kCauseOffset),
                    encode_cause_word(cause_number, 0), buffer, __FILE__, __LINE__);
    std::snprintf(buffer, sizeof(buffer), "%s: frame aux word", what);
    check_equal_u64(frame_word(kernel, frame, trap_frame::kAuxOffset), 0, buffer, __FILE__,
                    __LINE__);
}

}  // namespace

// ---------------------------------------------------------------------------------------------

V2_FIXTURE(interrupt_cause_numbers_and_register_layout_are_the_specified_ones) {
    // THE NUMBERS THEMSELVES, AS LITERALS. Everything else in this file names constants, which
    // reads better and guards nothing on its own: a machine whose line-to-cause mapping was
    // shifted moves the constant and every expectation written in terms of it together, and
    // passes. maize-464's fixtures learned this the hard way. This block is the one place the
    // numbers stop being symbols, so a renumbering has to break it.
    //
    // trap-model.md, "External interrupts": "The cause number of a device interrupt is 32 plus
    // the device's interrupt line index, and the line index equals the device class code."
    V2_CHECK_EQ(cause::kFirstExternalInterrupt, 32);
    V2_CHECK_EQ(cause::kConsoleInterrupt, 33);
    V2_CHECK_EQ(cause::kTimerInterrupt, 35);
    V2_CHECK_EQ(device_class::kConsole, 1);
    V2_CHECK_EQ(device_class::kTimer, 3);

    // privileged-architecture.md's base table. These four register numbers are what a kernel
    // hard-codes, so a build that moved one would break every guest ever written for it.
    V2_CHECK_EQ(csr::kInterruptEnable0, 0x4004);
    V2_CHECK_EQ(csr::kInterruptEnable3, 0x4007);
    V2_CHECK_EQ(csr::kInterruptPending0, 0x6000);
    V2_CHECK_EQ(csr::kInterruptPending3, 0x6003);
    V2_CHECK_EQ(csr::kScratch, 0x4009);

    // trap-model.md, "The status word": privilege in bits 1:0, the interrupt-enable bit at bit 2.
    // maize-463's spec prose says bits 15:14, which is the CONTROL AND STATUS REGISTER NUMBER's
    // privilege field and not this one; that prose is wrong and this line is what keeps the
    // machine from being made to agree with it.
    V2_CHECK_EQ(status_word::kPrivilegeMask, 0x3);
    V2_CHECK_EQ(status_word::kInterruptEnableBit, 0x4);

    // "Register `n` of an array holds causes `64n` through `64n + 63`, with the cause's number
    // modulo 64 selecting the bit." Checked as arithmetic on the numbers rather than by calling
    // the machine's own helper, which would agree with itself whatever it did.
    Machine machine;
    CsrFileV2& csr_file = machine.interpreter().csr();
    csr_file.host_set_interrupt_pending(0, std::uint64_t{1} << 35);
    V2_CHECK_EQ(csr_file.host_read(0x6000), std::uint64_t{1} << 35);
    V2_CHECK_EQ(csr_file.host_read(0x6001), 0);
    csr_file.host_set_interrupt_pending(2, std::uint64_t{1} << 7);
    V2_CHECK_EQ(csr_file.host_read(0x6002), std::uint64_t{1} << 7);  // cause 135
    V2_CHECK(csr_file.pending(135));
    V2_CHECK(!csr_file.pending(134));
}

V2_FIXTURE(interrupt_enable_zero_rejects_the_non_maskable_synchronous_causes) {
    // AC-3. "Bits 0 through 31 of the first enable register correspond to synchronous causes,
    // which are never maskable, so those bits read as zero and a write to them is required to be
    // zero; a write that sets any of them raises the illegal-operand trap with subcode 6."
    //
    // THE DISCRIMINATOR IS THE TRAP, not the read-back. A machine that silently accepted the
    // write and discarded the bits would read back zero exactly like a conforming one, so a
    // fixture that only checked the read-back could not tell them apart. Every case below checks
    // the trap first and the read-back second.
    struct Case {
        const char* what;
        std::uint64_t value;
        bool rejected;
    };

    const Case cases[] = {
        {"cause 0's bit", std::uint64_t{1} << 0, true},
        {"cause 11's bit, a cause this machine really raises", std::uint64_t{1} << 11, true},
        {"cause 31's bit, the last non-maskable one", std::uint64_t{1} << 31, true},
        {"cause 31 and cause 32 together, which is rejected whole", 0x1'8000'0000ull, true},
        {"cause 32's bit, the lowest maskable one", std::uint64_t{1} << 32, false},
        {"cause 33 and cause 35, the console and the timer", 0xA'0000'0000ull, false},
        {"every maskable bit of the register at once", 0xFFFFFFFF00000000ull, false},
    };

    for (const Case& one : cases) {
        Kernel kernel;
        const std::uint64_t write_pc = kernel.here();
        emit_csr_load(kernel.program(), csr::kInterruptEnable0, one.value);
        kernel.program().halt();
        kernel.install_handler(cause::kIllegalOperand, kHandlerBase);
        // A handler that only halts, so a delivered trap stops the machine somewhere the fixture
        // can identify rather than running off into the image.
        Encoder handler(kHandlerBase);
        handler.halt();
        kernel.load_at(handler);
        kernel.start();

        const StepResult result = kernel.run();
        if (one.rejected) {
            // Subcode 6, spelled as a digit. The auxiliary word is "the offending VALUE, not the
            // register number", which is the row of trap-model.md's cause table that a machine
            // reporting the number instead would pass every other check in this fixture.
            expect_trap(result, cause::kIllegalOperand, 6, one.value,
                        write_pc + 10 /* past the move.w that loaded the value */, one.what);
            V2_CHECK_EQ(kernel.csr().host_read(csr::kInterruptEnable0), 0);
        } else {
            V2_CHECK(result.status != StepStatus::Trapped);
            // "those bits read as zero" is a statement about the READ, so the accepted value
            // reads back exactly and nothing above bit 31 was trimmed on the way through.
            V2_CHECK_EQ(kernel.csr().host_read(csr::kInterruptEnable0), one.value);
        }
    }

    // And the read rule holds independently of the write rule. A machine whose write path was
    // correct and whose read path handed back a stored low bit would pass everything above.
    Machine machine;
    CsrFileV2& csr_file = machine.interpreter().csr();
    V2_CHECK(!csr_file.enabled(11));
    V2_CHECK(!csr_file.enabled(31));
}

V2_FIXTURE(pending_is_set_while_the_cause_is_masked_at_the_cpu) {
    // AC-4. The console's own port-level interrupt-enable bit is set, so it asserts; the
    // console's bit in interrupt_enable0 is clear, so the machine may not deliver. The pending
    // bit must read 1 anyway.
    //
    // THE WRONG MACHINE THIS CATCHES gates the setting of the pending bit on the CPU-level enable
    // rather than gating only DELIVERY on it. device-surface.md forbids that outright: "A device
    // whose interrupt-enable bit is clear still records the condition in its status port, so
    // every class is fully usable by polling." A guest that polls rather than taking interrupts
    // is a supported guest, and on the wrong machine it sees nothing.
    Kernel kernel;
    emit_port_out(kernel.program(), kConsoleControl, 1);
    // Interrupts enabled globally and at the CPU level for the TIMER but not for the console, so
    // the only reason cause 33 is not delivered is its own enable bit.
    emit_csr_load(kernel.program(), csr::kInterruptEnable0, std::uint64_t{1} << 35);
    emit_csr_load(kernel.program(), csr::kStatus, kSupervisorInterruptsOn);
    // Read the pending register twice, into two different registers, with an instruction between
    // them. "reading the pending register has no side effect on it" is a claim about the read,
    // and one read cannot test it.
    kernel.program().op_r_i2(op::kCsrRead, reg(10), csr::kInterruptPending0);
    kernel.program().op_r_i8(op::kMoveW, reg(12), kSentinel);
    kernel.program().op_r_i2(op::kCsrRead, reg(11), csr::kInterruptPending0);
    kernel.program().halt();

    // Nothing installs a handler for cause 33 on purpose: its vector entry stays zero, so a
    // machine that delivered it would halt with kind 1 and could not reach the halt below.
    kernel.start();
    kernel.devices().console().host_push_input('Z');

    const StepResult result = kernel.run();
    expect_halted(result, "the polling program");
    expect_halt_cause(kernel.machine(), halt_cause::kKindHaltInstruction, 0, 0,
                      "no trap was taken while the cause was masked");

    // Bit 33, as a digit. A machine that mapped the console to some other cause number would
    // still set A pending bit, and a fixture asserting through cause::kConsoleInterrupt would
    // move with it.
    const std::uint64_t expected = std::uint64_t{1} << 33;
    V2_CHECK_EQ(kernel.machine().get(10) & expected, expected);
    V2_CHECK_EQ(kernel.machine().get(11), kernel.machine().get(10));
    V2_CHECK_EQ(kernel.machine().get(12), kSentinel);

    // The device is still asserting, because nothing consumed the byte, and the status port
    // reports the condition whether or not the CPU was ever going to act on it.
    V2_CHECK((kernel.devices().port_in(kConsoleStatus) &
              status_mask(console_status_bit::kInputAvailable)) != 0);
    // And the trap stack never moved, which is the independent evidence that no frame was pushed.
    V2_CHECK_EQ(kernel.trap_stack(), kTrapStackTop);
}

V2_FIXTURE(lowest_numbered_deliverable_cause_wins) {
    // AC-1. Two causes pending and enabled at the same boundary: the machine takes the
    // lower-numbered one, its pending bit reads clear at the handler's first instruction, and the
    // other's still reads set.
    //
    // BOTH CAUSES ARE MADE PENDING WITH INTERRUPTS GLOBALLY OFF, then the global gate is opened
    // in one instruction. That is the only way to get two causes deliverable at the same
    // boundary on a correct machine: with the gate already open, the first one to become pending
    // is delivered at the next boundary and the two never coincide. A machine that got the
    // priority rule backwards enters the higher cause's handler here instead.
    //
    // NEITHER CAUSE HAS A DEVICE BEHIND IT, and that is what makes the second half of the
    // criterion, the pending bit reading clear at the handler's first instruction, a test of
    // delivery rather than of a device. A device's pending bit follows its line as a level, so a
    // device that is still asserting because the guest has not acknowledged it yet has its bit
    // back before the handler's first instruction runs, which is the re-raise the chapter calls
    // expected behaviour and not the property under test here. Causes 40 and 45 belong to no
    // class this machine carries, so nothing but delivery ever touches their bits, and the
    // pending register the handler reads is the delivery sequence's own work.
    Kernel kernel;
    emit_csr_load(kernel.program(), csr::kInterruptEnable0,
                  (std::uint64_t{1} << 40) | (std::uint64_t{1} << 45));
    kernel.program().op_r_i8(op::kMoveW, reg(10), kSentinel);
    // Open the gate. Both causes are pending by now and both are enabled.
    emit_csr_load(kernel.program(), csr::kStatus, kSupervisorInterruptsOn);
    kernel.program().halt();

    // Two handlers that differ only in the mark they leave, so which one ran is recorded rather
    // than inferred. Each records the pending register as it found it, which is the observation
    // that carries the whole criterion.
    Encoder lower_handler(kHandlerBase);
    emit_handler_prologue(lower_handler);
    lower_handler.op_r_i2(op::kCsrRead, reg(kHandlerScratch3), csr::kInterruptPending0);
    lower_handler.op_r_i8(op::kMoveW, reg(kHandlerScratch4), kMark0);
    lower_handler.op_r_r(op::kStore, reg(kHandlerScratch3), reg(kHandlerScratch4));
    emit_handler_epilogue(lower_handler);
    kernel.load_at(lower_handler);

    Encoder higher_handler(kHandlerBase + 0x100);
    emit_handler_prologue(higher_handler);
    higher_handler.op_r_i2(op::kCsrRead, reg(kHandlerScratch3), csr::kInterruptPending0);
    higher_handler.op_r_i8(op::kMoveW, reg(kHandlerScratch4), kMark1);
    higher_handler.op_r_r(op::kStore, reg(kHandlerScratch3), reg(kHandlerScratch4));
    emit_handler_epilogue(higher_handler);
    kernel.load_at(higher_handler);

    kernel.install_handler(40, kHandlerBase);
    kernel.install_handler(45, kHandlerBase + 0x100);
    kernel.start();
    // Both pending in one write, so neither became pending before the other and the machine has
    // no arrival order to break the tie with. Only the cause NUMBERS can decide it.
    kernel.csr().host_set_interrupt_pending(0, (std::uint64_t{1} << 40) | (std::uint64_t{1} << 45));

    const StepResult first = kernel.run();
    V2_CHECK_EQ(first.trap.cause, 40);
    V2_CHECK(first.disposition == TrapDisposition::Delivered);
    V2_CHECK_EQ(first.handler, kHandlerBase);

    expect_halted(kernel.run_to_halt(), "the interrupted program");

    // The lower cause's handler ran and saw its own pending bit ALREADY CLEAR, because delivery
    // clears it before the vector fetch, while the higher cause's was still set and waiting. A
    // machine that cleared the pending bit after entering the handler, or that cleared both,
    // differs here and only here.
    const std::uint64_t observed = kernel.word(kMark0);
    V2_CHECK_EQ(observed & (std::uint64_t{1} << 40), 0);
    V2_CHECK_EQ(observed & (std::uint64_t{1} << 45), std::uint64_t{1} << 45);

    // The higher cause's handler ran afterward, on its own boundary, and saw its own bit clear by
    // then. Both handlers ran, so nothing was lost by the machine taking one of them first.
    const std::uint64_t higher_observed = kernel.word(kMark1);
    V2_CHECK_EQ(higher_observed & (std::uint64_t{1} << 45), 0);
    V2_CHECK_EQ(higher_observed & (std::uint64_t{1} << 40), 0);
}

V2_FIXTURE(interrupt_lands_between_instructions_never_inside_one) {
    // AC-6. A multi-byte-operand instruction, and the frame's captured program counter is the
    // address of the instruction AFTER it, never an address inside its own byte range, with the
    // instruction's full architectural effect already committed.
    //
    // WHY THE TIMER RATHER THAN A CONTINUOUSLY-ASSERTED CAUSE. The criterion's own wording
    // reaches for a cause that is always pending, and on a correct machine that state cannot
    // coexist with the instruction ever running: with the global gate open and a cause
    // permanently pending and enabled, EVERY boundary delivers, so the machine re-enters the
    // handler forever and the instruction under test never executes. The timer names the boundary
    // instead of hoping for one, which is strictly more precise about the property: armed with
    // two instruction-times immediately before the target, its expiry falls at the end of the
    // target and the delivery at the boundary after it. A machine that polled for interrupts
    // mid-decode or mid-execute captures an address inside the target's ten bytes, and the
    // range check below is what says so rather than merely that the address was unexpected.
    Kernel kernel;
    emit_port_out(kernel.program(), kTimerControl, 1);
    emit_csr_load(kernel.program(), csr::kInterruptEnable0, std::uint64_t{1} << 35);
    emit_csr_load(kernel.program(), csr::kStatus, kSupervisorInterruptsOn);
    emit_timer_arm(kernel.program(), kExpireAfterNextInstruction, 1);

    // The target: move.w r10, imm64, which is one opcode byte, one operand byte and eight
    // immediate bytes.
    const std::uint64_t target_pc = kernel.here();
    kernel.program().op_r_i8(op::kMoveW, reg(10), kSentinel);
    const std::uint64_t after_target = kernel.here();
    V2_CHECK_EQ(after_target - target_pc, 10);
    kernel.program().op_r_i8(op::kMoveW, reg(11), kSentinel);
    kernel.program().halt();

    Encoder handler(kHandlerBase);
    emit_handler_prologue(handler);
    // The handler reads r10 straight out of the register file, because the frame saved no general
    // register and r10 therefore still holds whatever the interrupted program left in it. If the
    // machine had taken the interrupt part-way through the move.w, r10 would still hold zero here.
    handler.op_r_i8(op::kMoveW, reg(kHandlerScratch4), kMark0);
    handler.op_r_r(op::kStore, reg(10), reg(kHandlerScratch4));
    emit_timer_acknowledge(handler);
    emit_handler_epilogue(handler);
    kernel.load_at(handler);
    kernel.install_handler(cause::kTimerInterrupt, kHandlerBase);
    kernel.start();

    const StepResult delivery = kernel.run();
    V2_CHECK_EQ(delivery.trap.cause, 35);
    V2_CHECK(delivery.disposition == TrapDisposition::Delivered);

    const std::uint64_t frame = kernel.trap_stack();
    expect_interrupt_frame(kernel, frame, after_target, kSupervisorInterruptsOn, 35,
                           "the interrupt taken at the boundary after the target");

    // The captured address is outside the target's own byte range, stated as its own check rather
    // than left to follow from the equality above, because "not inside the instruction" is the
    // property and the exact following address is only the way a correct machine expresses it.
    const std::uint64_t captured = frame_word(kernel, frame, trap_frame::kPcOffset);
    V2_CHECK(captured < target_pc || captured >= after_target);

    kernel.run_to_halt();
    V2_CHECK_EQ(kernel.word(kMark0), kSentinel);
    V2_CHECK_EQ(kernel.machine().get(10), kSentinel);
    V2_CHECK_EQ(kernel.machine().get(11), kSentinel);
}

V2_FIXTURE(interrupt_during_block_copy_restarts_and_copies_every_byte_once) {
    // AC-2. An interrupt raised during a long block_copy is delivered before the copy completes,
    // the captured program counter is the block_copy instruction's OWN address, and trap_return
    // finishes the copy with every byte transferred exactly once.
    //
    // THREE WRONG MACHINES ARE IN RANGE HERE. One takes the interrupt only at the instruction's
    // own boundary, so a long copy delays interrupts without bound and the frame below points
    // past the copy rather than at it. One captures the following instruction's address, so the
    // copy is never resumed and the destination keeps the bytes it had. One resumes from the
    // start, so the first bytes are copied twice, which is invisible in the destination's
    // contents and visible in the count register the handler observes.
    Kernel kernel;
    constexpr std::uint64_t kCount = 256;
    for (std::uint64_t i = 0; i < kCount; ++i) {
        // A pattern where every byte differs from its neighbours and from its own address's low
        // byte, so a copy that was off by one or that duplicated a run shows up.
        kernel.machine().memory().write_byte(kBlockSource + i,
                                             static_cast<std::uint8_t>((i * 7 + 13) & 0xFF));
    }
    kernel.machine().memory().write_byte(kBlockDestination + kCount, 0xA5);  // the guard byte

    emit_port_out(kernel.program(), kTimerControl, 1);
    emit_csr_load(kernel.program(), csr::kInterruptEnable0, std::uint64_t{1} << 35);
    emit_csr_load(kernel.program(), csr::kStatus, kSupervisorInterruptsOn);
    kernel.program().op_r_i8(op::kMoveW, reg(10), kBlockSource);
    kernel.program().op_r_i8(op::kMoveW, reg(11), kBlockDestination);
    kernel.program().op_r_i8(op::kMoveW, reg(12), kCount);
    emit_timer_arm(kernel.program(), kExpireAfterNextInstruction, 1);
    const std::uint64_t copy_pc = kernel.here();
    kernel.program().op_r_r_r(op::kBlockCopy, reg(10), reg(11), reg(12));
    const std::uint64_t after_copy = kernel.here();
    kernel.program().halt();

    Encoder handler(kHandlerBase);
    emit_handler_prologue(handler);
    // Record the three block registers exactly as the handler found them. This is the
    // remaining-work description the restartability contract fixes, and it is the only place a
    // machine that resumed from the wrong point can be caught before the copy finishes and hides
    // the evidence.
    handler.op_r_i8(op::kMoveW, reg(kHandlerScratch4), kMark0);
    handler.op_r_r(op::kStore, reg(10), reg(kHandlerScratch4));
    handler.op_r_i8(op::kMoveW, reg(kHandlerScratch4), kMark1);
    handler.op_r_r(op::kStore, reg(11), reg(kHandlerScratch4));
    handler.op_r_i8(op::kMoveW, reg(kHandlerScratch4), kMark2);
    handler.op_r_r(op::kStore, reg(12), reg(kHandlerScratch4));
    emit_timer_acknowledge(handler);
    emit_handler_epilogue(handler);
    kernel.load_at(handler);
    kernel.install_handler(cause::kTimerInterrupt, kHandlerBase);
    kernel.start();

    const StepResult delivery = kernel.run();
    V2_CHECK_EQ(delivery.trap.cause, 35);
    V2_CHECK(delivery.disposition == TrapDisposition::Delivered);

    // "The captured program counter is the block instruction's OWN address rather than the
    // following instruction's", which is what makes trap_return resume the transfer.
    const std::uint64_t frame = kernel.trap_stack();
    expect_interrupt_frame(kernel, frame, copy_pc, kSupervisorInterruptsOn, 35,
                           "the interrupt taken part-way through the copy");
    V2_CHECK(frame_word(kernel, frame, trap_frame::kPcOffset) != after_copy);

    kernel.run_to_halt();

    // The remaining-work description: the count is the bytes not yet transferred, and each
    // pointer is the lowest address in its region not yet transferred. Whatever granularity the
    // machine chose, the three have to agree with each other and with a partial copy.
    const std::uint64_t source_at_entry = kernel.word(kMark0);
    const std::uint64_t destination_at_entry = kernel.word(kMark1);
    const std::uint64_t remaining = kernel.word(kMark2);
    V2_CHECK(remaining > 0 && remaining < kCount);  // genuinely part-way through
    V2_CHECK_EQ(source_at_entry, kBlockSource + (kCount - remaining));
    V2_CHECK_EQ(destination_at_entry, kBlockDestination + (kCount - remaining));

    // Every byte transferred exactly once. A machine that restarted from the beginning would
    // still leave a correct destination here, which is why the register check above is the one
    // that catches it, and this check is what catches a machine that skipped the remainder.
    for (std::uint64_t i = 0; i < kCount; ++i) {
        if (kernel.machine().memory().read_byte(kBlockDestination + i) !=
            kernel.machine().memory().read_byte(kBlockSource + i)) {
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "destination byte %llu",
                          static_cast<unsigned long long>(i));
            record_failure(buffer);
            break;
        }
    }
    // And not one byte past the region, which is what a machine that resumed with a stale count
    // would write.
    V2_CHECK_EQ(kernel.machine().memory().read_byte(kBlockDestination + kCount), 0xA5);

    // On completion the registers hold the completion state, which is each pointer past its
    // region and a count of zero.
    V2_CHECK_EQ(kernel.machine().get(10), kBlockSource + kCount);
    V2_CHECK_EQ(kernel.machine().get(11), kBlockDestination + kCount);
    V2_CHECK_EQ(kernel.machine().get(12), 0);
}

V2_FIXTURE(one_expiry_is_delivered_exactly_once_and_acknowledged_before_return) {
    // AC-5. A one-shot timer expiry, delivered once, with the device acknowledged before
    // trap_return.
    //
    // TWO WRONG MACHINES, AND THEY NEED DIFFERENT ASSERTIONS. A machine that redelivered the same
    // expiry before the guest acknowledged the device runs the handler more than once, which the
    // counter catches. A machine that cleared the DEVICE's latch on delivery, as though the
    // machine's acknowledgement and the device's were the same act, runs the handler exactly once
    // and would pass the counter check; what catches it is the handler reading the timer's own
    // status port at entry and finding expiry-pending still set. trap-model.md draws that line
    // itself: "Clearing the pending bit is the machine's acknowledgement, and it is not the
    // device's."
    Kernel kernel;
    emit_port_out(kernel.program(), kTimerControl, 1);
    emit_csr_load(kernel.program(), csr::kInterruptEnable0, std::uint64_t{1} << 35);
    emit_csr_load(kernel.program(), csr::kStatus, kSupervisorInterruptsOn);
    emit_timer_arm(kernel.program(), 3 * kOneInstruction, 1);  // one-shot: counting, not periodic
    // A long enough tail that a machine which redelivered would have many boundaries to do it at.
    for (unsigned i = 0; i < 40; ++i) {
        kernel.program().op_r_i8(op::kMoveW, reg(20), kSentinel);
    }
    kernel.program().halt();

    Encoder handler(kHandlerBase);
    emit_handler_prologue(handler);
    // The entry counter, incremented in memory so it survives the handler's own register churn.
    handler.op_r_i8(op::kMoveW, reg(kHandlerScratch4), kSequence);
    handler.op_r_r(op::kLoad, reg(kHandlerScratch4), reg(kHandlerScratch3));
    handler.op_r_r_i4(op::kAddImm, reg(kHandlerScratch3), reg(kHandlerScratch3), 1);
    handler.op_r_r(op::kStore, reg(kHandlerScratch3), reg(kHandlerScratch4));
    // The device's own status, read BEFORE the acknowledge below. Expiry-pending must still be
    // set: the machine cleared its pending bit, and only the guest can clear the device's latch.
    emit_port_in(handler, kTimerStatus, kHandlerScratch3);
    handler.op_r_i8(op::kMoveW, reg(kHandlerScratch4), kMark0);
    handler.op_r_r(op::kStore, reg(kHandlerScratch3), reg(kHandlerScratch4));
    emit_timer_acknowledge(handler);
    // And the status again, after. The acknowledge is what clears it.
    emit_port_in(handler, kTimerStatus, kHandlerScratch3);
    handler.op_r_i8(op::kMoveW, reg(kHandlerScratch4), kMark1);
    handler.op_r_r(op::kStore, reg(kHandlerScratch3), reg(kHandlerScratch4));
    emit_handler_epilogue(handler);
    kernel.load_at(handler);
    kernel.install_handler(cause::kTimerInterrupt, kHandlerBase);
    kernel.start();

    expect_halted(kernel.run_to_halt(), "the interrupted program");

    // Exactly one, as a digit rather than as "more than zero".
    V2_CHECK_EQ(kernel.word(kSequence), 1);
    // Bit 0 of the timer's status is expiry-pending, still set at handler entry and clear after
    // the acknowledge.
    V2_CHECK_EQ(kernel.word(kMark0) & 1, 1);
    V2_CHECK_EQ(kernel.word(kMark1) & 1, 0);
    // One frame pushed and one popped, so the trap stack is exactly where it started. A machine
    // that delivered twice and returned twice would also land here, which is why the counter
    // above is the primary check and this is the corroborating one.
    V2_CHECK_EQ(kernel.trap_stack(), kTrapStackTop);
    // A one-shot that was acknowledged is disarmed, so nothing further was ever scheduled.
    V2_CHECK_EQ(kernel.devices().port_in(kTimerStatus) & 1, 0);
}

V2_FIXTURE(wait_for_interrupt_retires_before_it_delivers) {
    // AC-7. When a wait is woken with the global gate open, the frame's captured program counter
    // is the address of the instruction immediately AFTER the wait, never the wait's own, and
    // that instruction runs exactly once after trap_return.
    //
    // THE WRONG MACHINE folds delivery into the wait, treating the interrupt as though it had
    // arrived mid-instruction, and captures the wait's own address; on trap_return that machine
    // executes the wait a second time, which on a real kernel is an idle loop that never makes
    // progress. execution-model.md's atomicity rule is what forbids it, and D-2 on this card
    // records that wait_for_interrupt has no defined mid-operation state of its own: the
    // block-memory family is "the only place in the base where an interrupt is taken part-way
    // through an instruction".
    //
    // WHY THE CAUSE ARRIVES DURING THE WAIT RATHER THAN BEFORE IT, since the criterion's own
    // wording reaches for a cause already pending and enabled at the moment the wait executes.
    // That state is unreachable on a correct machine while the global gate is open, and the
    // reason is worth stating rather than working around: pending, enabled and the gate open is
    // exactly the DELIVERY condition, so at the boundary before the wait the machine takes the
    // interrupt and the wait never executes at all. Every attempt to arrange it, from guest
    // instructions or from the harness, arranges the interrupt one boundary too early instead.
    // The specification's own sentence about that state, "When some cause is already pending and
    // enabled at the moment the instruction executes, the instruction completes immediately, so
    // no interrupt is lost by racing to sleep", is therefore about the gate-closed case, and
    // masked_completion_of_a_wait_is_not_a_delivery below is where it is tested.
    //
    // None of which touches the discriminator. What separates the two machines is where the
    // frame points when a wait is woken, and a cause arriving during the wait puts that question
    // just as sharply: the correct machine retires the wait first and then delivers at the
    // boundary after it, and the machine that folds the two together captures the wait's own
    // address. The timer's period below is a thousand instruction-times against a program of
    // fewer than fifty instructions, so the wake can only have come from a genuine suspension.
    Kernel kernel;
    emit_port_out(kernel.program(), kTimerControl, 1);
    emit_csr_load(kernel.program(), csr::kInterruptEnable0, std::uint64_t{1} << 35);
    emit_csr_load(kernel.program(), csr::kStatus, kSupervisorInterruptsOn);
    emit_timer_arm(kernel.program(), 1000 * kOneInstruction, 1);
    const std::uint64_t wait_pc = kernel.here();
    kernel.program().op(op::kWaitForInterrupt);
    const std::uint64_t after_wait = kernel.here();
    // The instruction after the wait increments a memory cell, so "exactly once" is a number
    // rather than a claim about whether it ran.
    kernel.program().op_r_i8(op::kMoveW, reg(20), kSequence);
    kernel.program().op_r_r(op::kLoad, reg(20), reg(21));
    kernel.program().op_r_r_i4(op::kAddImm, reg(21), reg(21), 1);
    kernel.program().op_r_r(op::kStore, reg(21), reg(20));
    kernel.program().halt();

    Encoder handler(kHandlerBase);
    emit_handler_prologue(handler);
    emit_timer_acknowledge(handler);
    emit_handler_epilogue(handler);
    kernel.load_at(handler);
    kernel.install_handler(cause::kTimerInterrupt, kHandlerBase);
    kernel.start();

    // Step to the wait, so the fixture knows the machine really did stop there rather than
    // reaching the delivery by some other route.
    while (kernel.machine().interpreter().pc() != wait_pc) {
        const StepResult result = kernel.step();
        if (result.status != StepStatus::Advanced) {
            record_failure("the setup did not reach wait_for_interrupt");
            return;
        }
    }
    V2_CHECK_EQ(kernel.trap_stack(), kTrapStackTop);  // nothing delivered before the wait

    // THE WAIT ITSELF RETIRES. It advances the program counter to the following instruction and
    // pushes no frame, which is the whole of the criterion: a machine that delivered from inside
    // the wait would report a trap here.
    const StepResult wait = kernel.step();
    V2_CHECK(wait.status == StepStatus::Advanced);
    V2_CHECK_EQ(wait.opcode, 0xBE);
    V2_CHECK_EQ(kernel.machine().interpreter().pc(), after_wait);
    V2_CHECK_EQ(kernel.trap_stack(), kTrapStackTop);

    // And the delivery happens at the NEXT boundary, which is the boundary after the wait.
    const StepResult delivery = kernel.step();
    V2_CHECK_EQ(delivery.trap.cause, 35);
    V2_CHECK(delivery.disposition == TrapDisposition::Delivered);
    const std::uint64_t frame = kernel.trap_stack();
    expect_interrupt_frame(kernel, frame, after_wait, kSupervisorInterruptsOn, 35,
                           "the interrupt that woke the wait");
    V2_CHECK(frame_word(kernel, frame, trap_frame::kPcOffset) != wait_pc);

    expect_halted(kernel.run_to_halt(), "the program after the wait");
    V2_CHECK_EQ(kernel.word(kSequence), 1);
}

V2_FIXTURE(masked_completion_of_a_wait_is_not_a_delivery) {
    // AC-8. With the global gate CLOSED, a cause's CPU-level enable set, and that cause made
    // pending by its device, wait_for_interrupt completes and no trap is taken. "The instruction's
    // completion does not depend on the status register's interrupt-enable bit, and this matters.
    // A kernel that runs its idle loop with interrupts masked wakes on the pending bit, polls the
    // pending registers, and services the source itself."
    //
    // A HOLLOW FIXTURE CANNOT TELL THIS FROM A DELIVERY, because both leave the machine running
    // past the wait. What distinguishes them is that no frame was pushed, no vector-table entry
    // was read, the pending bit survived, and the global gate is still closed. All four are
    // checked below, and the vector entry for cause 33 is deliberately left zero so that a
    // machine which delivered here would halt with kind 1 rather than reaching the halt
    // instruction.
    Kernel kernel;
    emit_port_out(kernel.program(), kConsoleControl, 1);
    emit_csr_load(kernel.program(), csr::kInterruptEnable0, std::uint64_t{1} << 33);
    // The status register keeps its reset value: supervisor, interrupts off. Written out anyway
    // so the fixture states the precondition rather than relying on the reset.
    emit_csr_load(kernel.program(), csr::kStatus, kSupervisorInterruptsOff);
    const std::uint64_t wait_pc = kernel.here();
    kernel.program().op(op::kWaitForInterrupt);
    const std::uint64_t after_wait = kernel.here();
    // The kernel polls the pending register itself, which is the whole point of the masked idle
    // loop, and records what it found.
    kernel.program().op_r_i2(op::kCsrRead, reg(10), csr::kInterruptPending0);
    kernel.program().op_r_i2(op::kCsrRead, reg(11), csr::kStatus);
    emit_store_absolute(kernel.program(), 10, kMark0);
    kernel.program().halt();

    kernel.start();
    kernel.devices().console().host_push_input('Q');

    const StepResult result = kernel.run();
    expect_halted(result, "the masked idle loop");
    expect_halt_cause(kernel.machine(), halt_cause::kKindHaltInstruction, 0, 0,
                      "no handler was ever entered");

    // The wait completed: the program counter advanced past it and reached the halt.
    V2_CHECK(kernel.machine().interpreter().pc() > after_wait);
    V2_CHECK(after_wait > wait_pc);
    // No frame was pushed.
    V2_CHECK_EQ(kernel.trap_stack(), kTrapStackTop);
    // The pending bit survived the completion, so the kernel can go and service the source.
    V2_CHECK_EQ(kernel.word(kMark0) & (std::uint64_t{1} << 33), std::uint64_t{1} << 33);
    V2_CHECK_EQ(kernel.machine().get(10) & (std::uint64_t{1} << 33), std::uint64_t{1} << 33);
    // And the global gate is still closed, spelled as the literal status word rather than as a
    // mask test, because a machine that set some other bit while it was in there would pass a
    // mask test.
    V2_CHECK_EQ(kernel.machine().get(11), kSupervisorInterruptsOff);
}

V2_FIXTURE(a_pending_but_disabled_cause_never_wakes_the_machine) {
    // AC-9. The console's byte is delivered and its device-level enable is set, so its pending bit
    // is set before the wait even executes, but its CPU-level enable is clear for the whole test.
    // The timer is enabled at both levels. The wait must not complete on the console; it completes
    // only when the timer fires, and the timer's handler observes the console's pending bit still
    // set, proving the disabled cause was pending the entire time without waking anything and
    // without being lost.
    //
    // WHY THE BYTE IS PRE-ARMED RATHER THAN DELIVERED MID-WAIT, since this criterion was rewritten
    // once and the reasoning must not be lost. The console's CPU-level enable is off for the whole
    // test, so the machine can never deliver on it whenever the byte arrives; pre-arming proves
    // the identical property using ordinary console-input injection, and proves it over a longer
    // interval, since the bit is pending for the entire wait rather than from one boundary onward.
    //
    // AND WHY THAT REWRITE DID NOT COST THE TEST. The risk in dropping the timing dependency was
    // ending up with a fixture that also passes on a machine which never suspends at all, racing
    // past the wait on the strength of ordinary between-instruction delivery. It does not, and the
    // structure below is what prevents it: the handler and the code after the wait each increment
    // one shared counter and record what they got, so the fixture asserts the ORDER they ran in.
    // A machine that treats the wait as a no-op reaches the post-wait store first, with the timer
    // nowhere near its expiry, and records 1 there and 2 in the handler, or never runs the handler
    // at all. Either way it fails on an assertion that names the defect.
    Kernel kernel;
    emit_port_out(kernel.program(), kConsoleControl, 1);
    emit_port_out(kernel.program(), kTimerControl, 1);
    // The timer's bit and NOT the console's. This one line is the whole precondition.
    emit_csr_load(kernel.program(), csr::kInterruptEnable0, std::uint64_t{1} << 35);
    emit_csr_load(kernel.program(), csr::kStatus, kSupervisorInterruptsOn);
    // A period far longer than the instruction stream that follows, so a machine that failed to
    // suspend could not reach the expiry by executing instructions.
    emit_timer_arm(kernel.program(), 1000 * kOneInstruction, 1);
    kernel.program().op(op::kWaitForInterrupt);
    // After the wait: take the next sequence number and record it.
    kernel.program().op_r_i8(op::kMoveW, reg(20), kSequence);
    kernel.program().op_r_r(op::kLoad, reg(20), reg(21));
    kernel.program().op_r_r_i4(op::kAddImm, reg(21), reg(21), 1);
    kernel.program().op_r_r(op::kStore, reg(21), reg(20));
    emit_store_absolute(kernel.program(), 21, kMark2);
    kernel.program().halt();

    Encoder handler(kHandlerBase);
    emit_handler_prologue(handler);
    // The console's pending bit, as the handler finds it.
    handler.op_r_i2(op::kCsrRead, reg(kHandlerScratch3), csr::kInterruptPending0);
    handler.op_r_i8(op::kMoveW, reg(kHandlerScratch4), kMark0);
    handler.op_r_r(op::kStore, reg(kHandlerScratch3), reg(kHandlerScratch4));
    // The handler's own sequence number.
    handler.op_r_i8(op::kMoveW, reg(kHandlerScratch4), kSequence);
    handler.op_r_r(op::kLoad, reg(kHandlerScratch4), reg(kHandlerScratch3));
    handler.op_r_r_i4(op::kAddImm, reg(kHandlerScratch3), reg(kHandlerScratch3), 1);
    handler.op_r_r(op::kStore, reg(kHandlerScratch3), reg(kHandlerScratch4));
    handler.op_r_i8(op::kMoveW, reg(kHandlerScratch4), kMark1);
    handler.op_r_r(op::kStore, reg(kHandlerScratch3), reg(kHandlerScratch4));
    emit_timer_acknowledge(handler);
    emit_handler_epilogue(handler);
    kernel.load_at(handler);
    kernel.install_handler(cause::kTimerInterrupt, kHandlerBase);
    kernel.start();
    kernel.devices().console().host_push_input('P');

    expect_halted(kernel.run_to_halt(), "the program that waited");

    // THE ORDER. The handler ran first and the post-wait code second, which is only true on a
    // machine that genuinely suspended: the timer's period is a thousand instruction-times and
    // the program has fewer than fifty instructions in it, so nothing but a real suspension can
    // reach the expiry before the halt.
    V2_CHECK_EQ(kernel.word(kMark1), 1);  // the handler took sequence number 1
    V2_CHECK_EQ(kernel.word(kMark2), 2);  // the code after the wait took 2
    V2_CHECK_EQ(kernel.word(kSequence), 2);

    // And the disabled cause was pending the whole time, and still is: the handler saw it set,
    // nothing delivered it, and nothing lost it.
    V2_CHECK_EQ(kernel.word(kMark0) & (std::uint64_t{1} << 33), std::uint64_t{1} << 33);
    V2_CHECK(kernel.csr().pending(33));
    V2_CHECK(!kernel.csr().enabled(33));
    // The byte was never consumed, so the console is still asserting, which is what makes the
    // pending bit above a live condition rather than a stale latch.
    V2_CHECK_EQ(kernel.devices().console().host_pending_input(), 1);
}

V2_FIXTURE(wait_for_interrupt_at_user_level_faults_without_suspending) {
    // AC-10. wait_for_interrupt executed at user level raises cause 4, the privileged-operation
    // fault, with the offending OPCODE BYTE in the auxiliary word, and no suspension occurs.
    //
    // The auxiliary word is the discriminator between the two shapes cause 4 takes: an opcode
    // byte for a privileged instruction and a register number for a control-and-status-register
    // access. $BE as a digit-for-digit literal is what says which one this is.
    Kernel kernel;
    const std::uint64_t wait_pc = kernel.here();
    kernel.program().op(op::kWaitForInterrupt);
    kernel.program().halt();

    Encoder handler(kHandlerBase);
    handler.halt();
    kernel.load_at(handler);
    kernel.install_handler(cause::kPrivilegedOperation, kHandlerBase);
    kernel.start();
    kernel.machine().interpreter().host_set_privilege(Privilege::User);

    const StepResult result = kernel.step();
    // A fault captures the faulting instruction's own address, so a handler that raised the
    // privilege and returned would re-execute the wait rather than skip it.
    expect_trap(result, cause::kPrivilegedOperation, 0, 0xBE, wait_pc,
                "wait_for_interrupt at user level");
    V2_CHECK(result.status == StepStatus::Trapped);
    // NOT suspended: the guard runs before the body, so the instruction never reaches the wait at
    // all. A machine that suspended first and checked privilege afterward would report this
    // status instead, and would hang on a real machine.
    V2_CHECK(result.status != StepStatus::Suspended);
    V2_CHECK(result.disposition == TrapDisposition::Delivered);
    // And the machine did not advance past the wait.
    V2_CHECK_EQ(result.trap.pc, wait_pc);
}

V2_FIXTURE(an_interrupt_handler_preserves_every_general_register) {
    // AC-11. A handler that follows the swap-and-scratch discipline preserves every general
    // register bit for bit across an interrupt, compared against a reference run of the same
    // program with the interrupt never taken.
    //
    // WHY THIS IS AN INTERRUPT RATHER THAN A SYSCALL. A syscall handler may claim r2 outright,
    // because the program asked for the call and the calling convention says so. An interrupt
    // handler owes the interrupted program every register, because the program did not ask for
    // anything and chose neither the moment nor the fact. The frame saves nothing, so a handler
    // that writes r2 without banking it first corrupts its victim silently, and that corruption
    // is what maize-463's D8 addendum, csr_swap and the supervisor scratch register, exists to
    // prevent. Without it preemptive multitasking is unbuildable.
    //
    // THE TWO RUNS EXECUTE THE SAME BYTES. The only difference is one word of data the program
    // reads and writes to the timer's interrupt-control port, so the instruction stream, the
    // register writes and the memory writes are identical in both, and any difference in the
    // final register file is the interrupt's doing and nothing else.
    constexpr std::uint64_t kArmCell = kMark3;

    struct Run {
        std::uint64_t registers[32] = {};
        std::uint64_t handler_entries = 0;
    };

    auto perform = [](bool arm_the_interrupt, Run& out) {
        Kernel kernel;
        kernel.machine().memory().write_little_endian(kArmCell, 8, arm_the_interrupt ? 1 : 0);

        // Enable the timer's interrupt line, or not, from the data cell. Identical instructions
        // in both runs.
        kernel.program().op_r_i8(op::kMoveW, reg(kValueRegister), kArmCell);
        kernel.program().op_r_r(op::kLoad, reg(kValueRegister), reg(kValueRegister));
        kernel.program().op_r_i8(op::kMoveW, reg(kPortRegister), kTimerControl);
        kernel.program().op_r_r(op::kPortOut, reg(kValueRegister), reg(kPortRegister));
        emit_csr_load(kernel.program(), csr::kInterruptEnable0, std::uint64_t{1} << 35);
        emit_csr_load(kernel.program(), csr::kStatus, kSupervisorInterruptsOn);
        emit_timer_arm(kernel.program(), 4 * kOneInstruction, 1);

        // Every general register gets a distinct value, so a handler that clobbered one is caught
        // by which value moved rather than only by the fact that something did. r0 is skipped
        // because it discards writes and reads zero by construction.
        for (unsigned number = 1; number < 32; ++number) {
            kernel.program().op_r_i8(op::kMoveW, reg(number),
                                     kSentinel ^ (std::uint64_t{number} * 0x1111111111111111ull));
        }
        // The stretch during which the expiry falls. move.w r0 writes the register that discards
        // writes, so these instructions consume boundaries and change no architectural state
        // other than the program counter, which is exactly what a comparison of two runs needs.
        for (unsigned i = 0; i < 12; ++i) {
            kernel.program().op_r_i8(op::kMoveW, reg(0), kSentinel);
        }
        kernel.program().halt();

        Encoder handler(kHandlerBase);
        emit_handler_prologue(handler);
        // The handler does real work with the registers it banked, which is what makes the
        // restoration a claim worth testing rather than a formality.
        handler.op_r_i8(op::kMoveW, reg(kHandlerScratch4), kSequence);
        handler.op_r_r(op::kLoad, reg(kHandlerScratch4), reg(kHandlerScratch3));
        handler.op_r_r_i4(op::kAddImm, reg(kHandlerScratch3), reg(kHandlerScratch3), 1);
        handler.op_r_r(op::kStore, reg(kHandlerScratch3), reg(kHandlerScratch4));
        // And it writes r2 itself, through the pointer the swap gave it, which is precisely the
        // register a handler carried over from a syscall convention would clobber.
        handler.op_r_r_i2(op::kStoreDisp, reg(kHandlerBase2), reg(kHandlerBase2), 16);
        emit_timer_acknowledge(handler);
        emit_handler_epilogue(handler);
        kernel.load_at(handler);
        kernel.install_handler(cause::kTimerInterrupt, kHandlerBase);
        kernel.start();

        expect_halted(kernel.run_to_halt(), arm_the_interrupt ? "the interrupted run"
                                                              : "the reference run");
        for (unsigned number = 0; number < 32; ++number) {
            out.registers[number] = kernel.machine().get(number);
        }
        out.handler_entries = kernel.word(kSequence);
    };

    Run interrupted;
    Run reference;
    perform(true, interrupted);
    perform(false, reference);

    // The runs really did differ in the one way they were meant to: one took the interrupt and
    // the other did not. Without this the comparison below would pass vacuously on a machine that
    // delivered nothing at all.
    V2_CHECK_EQ(interrupted.handler_entries, 1);
    V2_CHECK_EQ(reference.handler_entries, 0);

    for (unsigned number = 0; number < 32; ++number) {
        if (interrupted.registers[number] != reference.registers[number]) {
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer),
                          "r%u after an interrupt is $%016llX, the reference run left $%016llX",
                          number, static_cast<unsigned long long>(interrupted.registers[number]),
                          static_cast<unsigned long long>(reference.registers[number]));
            record_failure(buffer);
        }
    }

    // And r2 specifically, named because it is the one a handler is most likely to take, and
    // because a sweep that happened to skip it would report a clean pass.
    V2_CHECK_EQ(interrupted.registers[2],
                kSentinel ^ (std::uint64_t{2} * 0x1111111111111111ull));
}

V2_FIXTURE(timer_contract_arms_expires_and_rearms_as_the_class_says) {
    // The timer's own contract (device-surface.md, "Timer"), which the criteria above lean on
    // throughout and which nothing else in the suite covers, since maize-466 is what brought the
    // class into the build.
    Machine machine;
    DeviceSurfaceV2& ports = machine.interpreter().device_surface();

    // Reset state: "The timer's counting-enable and periodic bits are clear and its period reads
    // zero", and no expiry is pending.
    V2_CHECK_EQ(ports.port_in(kTimerPeriod), 0);
    V2_CHECK_EQ(ports.port_in(kTimerMode), 0);
    V2_CHECK_EQ(ports.port_in(kTimerStatus), 0);
    V2_CHECK_EQ(ports.port_in(0x0030) & 0xFFFF, 3);  // the class code, as a digit

    // "A period of zero with counting enabled is an invalid request: the device sets the
    // invalid-request status bit, leaves counting disabled, and raises no trap." Bit 2 is the
    // skeleton's invalid-request bit, in every class.
    ports.port_out(kTimerMode, 1);
    V2_CHECK_EQ(ports.port_in(kTimerStatus) & 0x4, 0x4);
    V2_CHECK_EQ(ports.port_in(kTimerMode) & 0x1, 0);  // counting really was left disabled
    ports.port_out(kTimerStatus, 0x4);                // acknowledge it
    V2_CHECK_EQ(ports.port_in(kTimerStatus) & 0x4, 0);

    // A one-shot: it expires once and stays expired until acknowledged, and the acknowledge
    // leaves it disarmed.
    ports.port_out(kTimerPeriod, 5000);
    ports.port_out(kTimerMode, 1);
    ports.advance_time(4999);
    V2_CHECK_EQ(ports.port_in(kTimerStatus) & 1, 0);
    ports.advance_time(1);
    V2_CHECK_EQ(ports.port_in(kTimerStatus) & 1, 1);
    ports.advance_time(1'000'000);
    ports.port_out(kTimerStatus, 1);
    V2_CHECK_EQ(ports.port_in(kTimerStatus) & 1, 0);
    ports.advance_time(1'000'000);
    V2_CHECK_EQ(ports.port_in(kTimerStatus) & 1, 0);  // disarmed, so no second expiry

    // A periodic timer: the acknowledge re-arms it for the next expiry.
    ports.port_out(kTimerPeriod, 1000);
    ports.port_out(kTimerMode, 3);  // counting and periodic
    ports.advance_time(1000);
    V2_CHECK_EQ(ports.port_in(kTimerStatus) & 1, 1);
    ports.port_out(kTimerStatus, 1);
    V2_CHECK_EQ(ports.port_in(kTimerStatus) & 1, 0);
    ports.advance_time(1000);
    V2_CHECK_EQ(ports.port_in(kTimerStatus) & 1, 1);

    // "a monotonic count of nanoseconds since power-on, which never decreases and never wraps
    // within any run", and it agrees with what was advanced through it.
    const std::uint64_t count = ports.port_in(0x0035);
    V2_CHECK_EQ(count, 4999 + 1 + 1'000'000 + 1'000'000 + 1000 + 1000);

    // The line is asserted only when the device's own enable bit is set, which is the difference
    // between a condition being true and a device asking to be interrupted about it.
    V2_CHECK_EQ(ports.asserted_interrupt_lines(), 0);
    ports.port_out(kTimerControl, 1);
    V2_CHECK_EQ(ports.asserted_interrupt_lines(), std::uint64_t{1} << 3);
    ports.port_out(kTimerStatus, 1);
    V2_CHECK_EQ(ports.asserted_interrupt_lines(), 0);
}

V2_FIXTURE(timer_refuses_a_zero_period_written_while_counting_is_already_enabled) {
    // THE SAME RULE, BY THE OTHER ROUTE. "A period of zero with counting enabled is an invalid
    // request: the device sets the invalid-request status bit, leaves counting disabled, and
    // raises no trap" (device-surface.md, "Timer") is a condition on the device's state, and two
    // guest actions reach that state: enabling counting while the period is zero, which the
    // fixture above drives, and zeroing the period while counting is already enabled, which is
    // this one. A device that guards only the first route reports nothing wrong on the second,
    // reads back a mode register that still claims to be counting, and then stops after the
    // interval already in flight, which is a silently dead timer rather than a refused request.
    Machine machine;
    DeviceSurfaceV2& ports = machine.interpreter().device_surface();

    // A running periodic timer, mid-interval, with nothing wrong yet.
    ports.port_out(kTimerPeriod, 1000);
    ports.port_out(kTimerMode, 3);  // counting and periodic
    ports.advance_time(400);
    V2_CHECK_EQ(ports.port_in(kTimerStatus) & 0x4, 0);
    V2_CHECK_EQ(ports.port_in(kTimerMode) & 0x1, 0x1);

    ports.port_out(kTimerPeriod, 0);

    // The two things the sentence promises that a device surface can show. Its third, "raises no
    // trap", belongs to the instruction that carries the write rather than to the device, and the
    // port_out path already refuses to raise anything at all from a device write, so there is
    // nothing here for a device-level fixture to assert about it.
    V2_CHECK_EQ(ports.port_in(kTimerStatus) & 0x4, 0x4);  // invalid-request is set
    V2_CHECK_EQ(ports.port_in(kTimerMode) & 0x1, 0);      // counting is left disabled

    // The refusal is of the counting bit and not of the whole write: the period the guest asked
    // for reads back, and so does the periodic bit it did not touch.
    V2_CHECK_EQ(ports.port_in(kTimerPeriod), 0);
    V2_CHECK_EQ(ports.port_in(kTimerMode) & 0x2, 0x2);

    // Counting really stopped rather than merely being reported as stopped: the interval that was
    // in flight does not expire either, so there is no last tick from a refused configuration.
    ports.advance_time(1'000'000);
    V2_CHECK_EQ(ports.port_in(kTimerStatus) & 0x1, 0);
    ports.port_out(kTimerControl, 1);
    V2_CHECK_EQ(ports.asserted_interrupt_lines(), 0);
}

V2_FIXTURE(timer_period_written_mid_interval_takes_effect_at_the_next_expiry) {
    // "Writing the period while counting is enabled takes effect at the next expiry rather than
    // immediately, so a running periodic timer is reprogrammed without losing a tick"
    // (device-surface.md, "Timer"). The clause has two halves and a machine can fail either one:
    // applying the new period at once loses the tick in flight, and latching the period at arm
    // time and never re-reading it means the new period never arrives at all.
    Machine machine;
    DeviceSurfaceV2& ports = machine.interpreter().device_surface();

    ports.port_out(kTimerPeriod, 1000);
    ports.port_out(kTimerMode, 3);  // counting and periodic
    ports.advance_time(500);
    ports.port_out(kTimerPeriod, 4000);

    // The register reads back the new value straight away; what is deferred is the interval.
    V2_CHECK_EQ(ports.port_in(kTimerPeriod), 4000);

    // The tick in flight still lands where the old period put it, at 1000 rather than at 4500.
    ports.advance_time(499);
    V2_CHECK_EQ(ports.port_in(kTimerStatus) & 0x1, 0);
    ports.advance_time(1);
    V2_CHECK_EQ(ports.port_in(kTimerStatus) & 0x1, 0x1);

    // And the interval after it is the new period, so the write was not merely recorded.
    ports.port_out(kTimerStatus, 0x1);
    ports.advance_time(3999);
    V2_CHECK_EQ(ports.port_in(kTimerStatus) & 0x1, 0);
    ports.advance_time(1);
    V2_CHECK_EQ(ports.port_in(kTimerStatus) & 0x1, 0x1);
}

V2_FIXTURE(console_asserts_its_line_only_while_a_byte_is_waiting) {
    // The console's half of the same contract: "The interrupt condition is input-available.
    // Reading offset 3 consumes the byte and clears the condition when no further byte is
    // waiting." That level-sensitivity is what makes a device re-raise its cause after a delivery
    // until the guest actually reads the byte, which trap-model.md calls expected behaviour
    // rather than a double-delivery bug, and it is the distinction this fixture pins.
    Machine machine;
    DeviceSurfaceV2& ports = machine.interpreter().device_surface();

    ports.port_out(kConsoleControl, 1);
    V2_CHECK_EQ(ports.asserted_interrupt_lines(), 0);  // enabled, but nothing waiting

    ports.console().host_push_input('a');
    ports.console().host_push_input('b');
    V2_CHECK_EQ(ports.asserted_interrupt_lines(), std::uint64_t{1} << 1);
    V2_CHECK_EQ(ports.port_in(kConsoleStatus) & 1, 1);

    // The first read consumes one byte and the condition holds, because another is waiting.
    V2_CHECK_EQ(ports.port_in(kConsoleData), 'a');
    V2_CHECK_EQ(ports.asserted_interrupt_lines(), std::uint64_t{1} << 1);
    // The second empties the stream and drops the line.
    V2_CHECK_EQ(ports.port_in(kConsoleData), 'b');
    V2_CHECK_EQ(ports.asserted_interrupt_lines(), 0);
    V2_CHECK_EQ(ports.port_in(kConsoleStatus) & 1, 0);
    // And a read of an empty stream yields zero and consumes nothing.
    V2_CHECK_EQ(ports.port_in(kConsoleData), 0);

    // An acknowledge cannot clear input-available, because the device still considers it true.
    // This is the held-versus-acknowledgeable split, tested on the bit maize-466 added.
    ports.console().host_push_input('c');
    ports.port_out(kConsoleStatus, 0xFFFFFFFFFFFFFFFFull);
    V2_CHECK_EQ(ports.port_in(kConsoleStatus) & 1, 1);
    V2_CHECK_EQ(ports.asserted_interrupt_lines(), std::uint64_t{1} << 1);
}

V2_FIXTURE(a_wait_with_nothing_armed_suspends_rather_than_spinning) {
    // The machine has to be able to say that it suspended, or the two states a fixture most needs
    // to tell apart, a machine genuinely blocked in the wait and a machine spinning through it,
    // are indistinguishable from outside until a timeout fires. StepStatus::Suspended is that
    // report, and it is a host diagnostic rather than a trap: the machine is doing exactly what
    // "suspends the machine until some cause has both its pending bit and its enable bit set"
    // requires, and no cause can ever satisfy it because no device has anything scheduled.
    // The global gate stays CLOSED, so the wake below is the wait retiring on the raw pending
    // and enable condition rather than a delivery, and the two cannot be confused for each other.
    Kernel kernel;
    emit_csr_load(kernel.program(), csr::kStatus, kSupervisorInterruptsOff);
    const std::uint64_t wait_pc = kernel.here();
    kernel.program().op(op::kWaitForInterrupt);
    kernel.program().halt();
    kernel.start();

    const StepResult result = kernel.run();
    V2_CHECK(result.status == StepStatus::Suspended);
    V2_CHECK_EQ(result.opcode, 0xBE);
    V2_CHECK_EQ(result.pc, wait_pc);
    // The program counter still names the wait, so the machine resumes the wait rather than
    // skipping it once something is armed.
    V2_CHECK_EQ(kernel.machine().interpreter().pc(), wait_pc);
    V2_CHECK(!kernel.machine().interpreter().halted());
    V2_CHECK_EQ(kernel.trap_stack(), kTrapStackTop);

    // And it really does resume: make a cause pending from the host and step again. The cause's
    // ENABLE bit is still clear at this point, so the machine stays suspended, which is the
    // second half of "A cause whose pending bit is set while its enable bit is clear does not
    // wake the machine" tested at the one instruction that could hide it.
    kernel.csr().host_set_interrupt_pending(0, std::uint64_t{1} << 40);
    const StepResult still_waiting = kernel.step();
    V2_CHECK(still_waiting.status == StepStatus::Suspended);
    V2_CHECK_EQ(kernel.machine().interpreter().pc(), wait_pc);

    // Now the enable bit, through the machine's own write path so the register's validation runs.
    const CsrOutcome enabled = kernel.csr().access(csr::kInterruptEnable0, Privilege::Supervisor,
                                                   true, std::uint64_t{1} << 40);
    V2_CHECK(enabled.ok);
    const StepResult woken = kernel.step();
    V2_CHECK(woken.status == StepStatus::Advanced);
    V2_CHECK(kernel.machine().interpreter().pc() > wait_pc);
    // It woke without delivering anything, because the global gate was never opened.
    V2_CHECK_EQ(kernel.trap_stack(), kTrapStackTop);
    V2_CHECK(kernel.csr().pending(40));
}

}  // namespace maize::v2::test
