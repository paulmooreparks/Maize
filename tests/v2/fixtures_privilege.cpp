// fixtures_privilege.cpp (maize-463): privilege levels and the control-and-status-register
// space, against privileged-architecture.md "Privilege levels" through "The base registers".
//
// The chapter states four access rules, fixes the order the machine applies them in, and then
// says the order is observable. That makes the ordering itself a fixture rather than a comment,
// and csr_access_rules_apply_in_the_chapters_order below is it: every adjacent pair of rules is
// provoked by a number that satisfies both, and the expected trap is the earlier rule's.
//
// Rule 4 gets a fixture of its own because it is the rule a port loses to habit. Maize v1 read
// an undefined control-register index as zero and discarded a write to it, and conformance.md
// names that convention as the likely defect. A fixture that exercised only implemented numbers
// would pass on a machine carrying it.
//
// There is no trap delivery in this build (maize-464), so a fixture that needs to observe a
// register AFTER a rejected write resumes by hand at the following instruction with set_pc,
// which is what a handler returning will do once trap_return exists. The read that follows is
// the guest's own, so "the register keeps its value" is checked through the instruction set
// rather than through a host accessor.

#include <cstdio>

#include "fixture_support.h"

namespace maize::v2::test {
namespace {

constexpr std::uint64_t kBase = 0x100;
constexpr std::uint64_t kSentinel = 0x0123456789ABCDEFull;

// privileged-architecture.md "The base registers", all eighteen, in the chapter's order.
constexpr std::uint16_t kBaseNumbers[] = {
    csr::kFcsr,             csr::kFeatureBitmap,     csr::kStatus,
    csr::kTrapStack,        csr::kTrapVectorBase,    csr::kPagingRoot,
    csr::kInterruptEnable0, csr::kInterruptEnable1,  csr::kInterruptEnable2,
    csr::kInterruptEnable3, csr::kSyscallProvider,   csr::kScratch,
    csr::kInterruptPending0, csr::kInterruptPending1, csr::kInterruptPending2,
    csr::kInterruptPending3, csr::kHaltCause,        csr::kBootInfo,
};
constexpr unsigned kBaseNumberCount = sizeof(kBaseNumbers) / sizeof(kBaseNumbers[0]);

// Which of the three instructions an access uses. All three carry the same access rules, so
// most cases below run all three and expect one answer.
enum class Which { Read, Write, Swap };

const char* which_name(Which which) {
    switch (which) {
        case Which::Read: return "csr_read";
        case Which::Write: return "csr_write";
        case Which::Swap: return "csr_swap";
    }
    return "?";
}

constexpr unsigned kSourceRegister = 1;
constexpr unsigned kDestinationRegister = 2;

// One access on a fresh machine at a chosen privilege level. The destination register starts at
// a sentinel so "rd is unmodified on any trap" is a real check rather than a comparison against
// the zero it would hold anyway.
template <typename Inspect>
void run_access(Which which, std::uint16_t number, std::uint64_t source_value, Privilege level,
                Inspect&& inspect) {
    Machine machine;
    Encoder program(kBase);
    switch (which) {
        case Which::Read:
            program.op_r_i2(op::kCsrRead, reg(kDestinationRegister), number);
            break;
        case Which::Write:
            program.op_r_i2(op::kCsrWrite, reg(kSourceRegister), number);
            break;
        case Which::Swap:
            program.op_r_r_i2(op::kCsrSwap, reg(kSourceRegister), reg(kDestinationRegister),
                              number);
            break;
    }
    machine.load(program);
    machine.set(kSourceRegister, source_value);
    machine.set(kDestinationRegister, kSentinel);
    machine.interpreter().host_set_privilege(level);
    const StepResult result = machine.step();
    inspect(machine, result);
}

// The same access run on all three instructions, expecting one trap from each. `writes_only`
// covers a rule csr_read cannot trip, which is rule 3.
void expect_same_trap_on_each(std::uint16_t number, std::uint64_t value, Privilege level,
                              std::uint8_t cause_number, std::uint8_t subcode_number,
                              std::uint64_t aux, bool writes_only, const char* what) {
    const Which all[] = {Which::Read, Which::Write, Which::Swap};
    for (Which which : all) {
        if (writes_only && which == Which::Read) {
            continue;
        }
        run_access(which, number, value, level, [&](Machine& machine, const StepResult& result) {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "%s: %s of $%04X", what, which_name(which),
                          static_cast<unsigned>(number));
            expect_trap(result, cause_number, subcode_number, aux, kBase, buffer);
            // Every trapping case leaves rd alone, on all three instructions.
            V2_CHECK_EQ(machine.get(kDestinationRegister), kSentinel);
        });
    }
}

}  // namespace

V2_FIXTURE(csr_access_rules_apply_in_the_chapters_order) {
    // privileged-architecture.md, "The number layout". The four rules in the chapter's order:
    //
    //   1. A reserved privilege field (%10 or %11) raises illegal-operand subcode 7 at EVERY
    //      level, supervisor included.
    //   2. An access from below the level bits 15:14 name raises privileged-operation, cause 4,
    //      with the register number as the auxiliary word.
    //   3. A write to a number whose read-only bit is set raises illegal-operand subcode 4.
    //   4. A well-formed number this machine does not implement raises illegal-operand
    //      subcode 3.
    //
    // Rule 1, at both levels. $8000 has privilege field %10 and $C000 has %11, and neither
    // names a level the machine can check an access against, which is why the level does not
    // matter. Both numbers are otherwise index zero, so nothing else about them is wrong.
    for (std::uint16_t number : {std::uint16_t{0x8000}, std::uint16_t{0xC000}}) {
        expect_same_trap_on_each(number, 0x55, Privilege::Supervisor, cause::kIllegalOperand,
                                 subcode::kReservedCsrPrivilege, number, false,
                                 "rule 1 at supervisor level");
        expect_same_trap_on_each(number, 0x55, Privilege::User, cause::kIllegalOperand,
                                 subcode::kReservedCsrPrivilege, number, false,
                                 "rule 1 at user level");
    }

    // Rule 2. A supervisor-numbered register from user level. The auxiliary word of cause 4 is
    // the register number for a control-and-status-register access, not an opcode byte
    // (trap-model.md, the cause 4 row).
    for (std::uint16_t number : {csr::kStatus, csr::kScratch, csr::kTrapStack}) {
        expect_same_trap_on_each(number, 0x10, Privilege::User, cause::kPrivilegedOperation, 0,
                                 number, false, "rule 2");
    }

    // Rule 3. A write to a read-only number at supervisor level, where rule 2 does not
    // intercept it.
    for (std::uint16_t number : {csr::kHaltCause, csr::kBootInfo, csr::kInterruptPending0}) {
        expect_same_trap_on_each(number, 0x10, Privilege::Supervisor, cause::kIllegalOperand,
                                 subcode::kReadOnlyCsr, number, true, "rule 3");
    }

    // Rule 4 has a fixture of its own below. Here it is only the loser in the ordering pairs.

    // THE ORDERING ITSELF. Each pair below is a number that satisfies two rules at once, and
    // the expected trap is the EARLIER rule's. An implementation that gathers the four
    // conditions into one expression, or that checks them in the order they read most naturally,
    // fails here and passes everything above.

    // Rule 1 before rule 2: $C000 is reserved-privilege, and from user level it is also an
    // access from below. Subcode 7, not cause 4.
    expect_same_trap_on_each(0xC000, 0x10, Privilege::User, cause::kIllegalOperand,
                             subcode::kReservedCsrPrivilege, 0xC000, false,
                             "rule 1 before rule 2");

    // Rule 1 before rule 3: $E000 is reserved-privilege AND read-only. Subcode 7, not 4.
    expect_same_trap_on_each(0xE000, 0x10, Privilege::Supervisor, cause::kIllegalOperand,
                             subcode::kReservedCsrPrivilege, 0xE000, true,
                             "rule 1 before rule 3");

    // Rule 1 before rule 4: $8123 is reserved-privilege and names no implemented register.
    // Subcode 7, not 3.
    expect_same_trap_on_each(0x8123, 0x10, Privilege::Supervisor, cause::kIllegalOperand,
                             subcode::kReservedCsrPrivilege, 0x8123, false,
                             "rule 1 before rule 4");

    // Rule 2 before rule 3, which is the chapter's own worked example: "A number that is
    // read-only and names supervisor, accessed at user level, takes rule 2 and raises the
    // privileged-operation fault, and it does not reach rule 3."
    expect_same_trap_on_each(csr::kHaltCause, 0x10, Privilege::User, cause::kPrivilegedOperation,
                             0, csr::kHaltCause, true, "rule 2 before rule 3");

    // Rule 2 before rule 4: $4100 names supervisor and is unimplemented. Cause 4, not subcode 3.
    expect_same_trap_on_each(0x4100, 0x10, Privilege::User, cause::kPrivilegedOperation, 0,
                             0x4100, false, "rule 2 before rule 4");

    // Rule 3 before rule 4: $6006 is read-only, names supervisor, and is unimplemented. A write
    // takes subcode 4, not subcode 3. The READ of the same number takes subcode 3, since rule 3
    // is a write rule, and that asymmetry is what shows the two rules are distinct rather than
    // one condition tested twice.
    expect_same_trap_on_each(0x6006, 0x10, Privilege::Supervisor, cause::kIllegalOperand,
                             subcode::kReadOnlyCsr, 0x6006, true, "rule 3 before rule 4");
    run_access(Which::Read, 0x6006, 0, Privilege::Supervisor,
               [](Machine& machine, const StepResult& result) {
                   expect_trap(result, cause::kIllegalOperand, subcode::kUnimplementedCsr, 0x6006,
                               kBase, "a read of unimplemented read-only $6006");
                   V2_CHECK_EQ(machine.get(kDestinationRegister), kSentinel);
               });

    // The read-only rule leaves the register's value alone, and the check is the guest's own
    // read rather than a host accessor. halt_cause is preloaded with a value no reset produces,
    // so a machine that stored the write and a machine that zeroed the register both fail.
    {
        constexpr std::uint64_t kPreloaded = 0xFEEDFACECAFEBEEFull;
        Machine machine;
        Encoder program(kBase);
        program.op_r_i2(op::kCsrWrite, reg(kSourceRegister), csr::kHaltCause);
        program.op_r_i2(op::kCsrRead, reg(5), csr::kHaltCause);
        program.halt();
        machine.load(program);
        machine.set(kSourceRegister, 0x1111111111111111ull);
        machine.interpreter().csr().host_set_halt_cause(kPreloaded);

        const StepResult trapped = machine.step();
        expect_trap(trapped, cause::kIllegalOperand, subcode::kReadOnlyCsr, csr::kHaltCause,
                    kBase, "a write to halt_cause");
        // Resume at the instruction after the faulting one, which is what trap_return will do
        // once maize-464 exists.
        machine.interpreter().set_pc(kBase + 4);
        expect_halted(machine.run(), "the read that follows a rejected write");
        V2_CHECK_EQ(machine.get(5), kPreloaded);
    }
}

V2_FIXTURE(csr_unimplemented_numbers_trap_rather_than_reading_zero) {
    // Rule 4, and conformance.md's named defect. Maize v1 read an undefined control-register
    // index as zero and discarded a write to it. Every number below is well formed, names
    // supervisor or user so that rule 2 does not intercept it at supervisor level, has its
    // read-only bit clear so that rule 3 does not, and names no register in the base table.
    const std::uint16_t unimplemented[] = {
        0x0001,  // the base's user range, immediately above fcsr
        0x0FFF,  // the last base index
        0x1000,  // the first extension index; no extension is allocated in this build
        0x1234,  // an arbitrary extension index
        0x1FFF,  // the last extension index
        // Every number here has its read-only bit CLEAR, so a write reaches rule 4 rather than
        // being stopped at rule 3. $2001 would not: bit 13 is the read-only bit, so the whole
        // $2xxx range is read-only and a write to an unimplemented number in it takes subcode 4.
        // The ordering fixture covers that case deliberately, with $6006.
        0x0100,  // user, an index in no allocated range
        0x400A,  // supervisor, immediately above scratch
        0x4100,  // supervisor, a long way above the allocated block
        0x5000,  // supervisor, an index in no allocated range
    };

    for (std::uint16_t number : unimplemented) {
        expect_same_trap_on_each(number, 0xA5A5A5A5A5A5A5A5ull, Privilege::Supervisor,
                                 cause::kIllegalOperand, subcode::kUnimplementedCsr, number,
                                 false, "an unimplemented number");
    }

    // The read case deserves its own words: rd must still hold the sentinel afterward. A
    // machine that read the undefined index as zero would leave rd holding zero, which is
    // exactly the v1 behaviour and exactly what a test asserting only "some trap happened"
    // would miss. expect_same_trap_on_each already checks the sentinel; this loop states the
    // v1 value explicitly so the failure message names the defect.
    for (std::uint16_t number : unimplemented) {
        run_access(Which::Read, number, 0, Privilege::Supervisor,
                   [&](Machine& machine, const StepResult&) {
                       if (machine.get(kDestinationRegister) == 0u) {
                           char buffer[192];
                           std::snprintf(buffer, sizeof(buffer),
                                         "csr_read of unimplemented $%04X delivered zero into "
                                         "rd, which is the v1 convention conformance.md rejects",
                                         static_cast<unsigned>(number));
                           record_failure(buffer);
                       }
                   });
    }

    // The positive control. Every number the base table DOES define is reachable at supervisor
    // level, so the fixture is testing which numbers are implemented rather than a machine that
    // traps on all of them.
    for (std::uint16_t number : kBaseNumbers) {
        run_access(Which::Read, number, 0, Privilege::Supervisor,
                   [&](Machine&, const StepResult& result) {
                       if (result.status != StepStatus::Advanced) {
                           char buffer[192];
                           std::snprintf(buffer, sizeof(buffer),
                                         "csr_read of implemented $%04X did not advance",
                                         static_cast<unsigned>(number));
                           record_failure(buffer);
                       }
                   });
    }
}

V2_FIXTURE(csr_read_has_no_side_effect) {
    // "A read of an implemented register has no side effect on any other register." Every
    // implemented number is read in turn on a machine whose whole architectural state has been
    // stirred away from its reset values first, and afterward every control and status register
    // and every general register except the destination has to be exactly what it was.
    for (std::uint16_t number : kBaseNumbers) {
        Machine machine;
        Encoder program(kBase);
        program.op_r_i2(op::kCsrRead, reg(5), number);
        program.halt();
        machine.load(program);

        // Stir the state: distinct values everywhere, so an accidental copy between two
        // registers shows up rather than matching by luck.
        CsrFileV2& csr_file = machine.interpreter().csr();
        csr_file.host_set_feature_bitmap(0x0000000000000003ull);
        csr_file.host_set_boot_info(0x0000000000002000ull);
        csr_file.host_set_halt_cause(0x0000000000000042ull);
        for (unsigned array = 0; array < 4; ++array) {
            csr_file.host_set_interrupt_pending(array, 0x1000u + array);
        }
        for (unsigned n = 1; n < kRegisterCount; ++n) {
            machine.set(n, kSentinel + n);
        }

        std::uint64_t before[kBaseNumberCount] = {};
        for (unsigned i = 0; i < kBaseNumberCount; ++i) {
            before[i] = csr_file.host_read(kBaseNumbers[i]);
        }

        // One step, so what is compared below is the csr_read's effect and not the halt's.
        V2_CHECK(machine.step().status == StepStatus::Advanced);

        for (unsigned i = 0; i < kBaseNumberCount; ++i) {
            if (csr_file.host_read(kBaseNumbers[i]) != before[i]) {
                char buffer[192];
                std::snprintf(buffer, sizeof(buffer),
                              "a csr_read of $%04X changed register $%04X",
                              static_cast<unsigned>(number),
                              static_cast<unsigned>(kBaseNumbers[i]));
                record_failure(buffer);
            }
        }
        for (unsigned n = 1; n < kRegisterCount; ++n) {
            if (n == 5) {
                continue;  // the destination, which is the one thing that may change
            }
            if (machine.get(n) != kSentinel + n) {
                char buffer[192];
                std::snprintf(buffer, sizeof(buffer), "a csr_read of $%04X changed r%u",
                              static_cast<unsigned>(number), n);
                record_failure(buffer);
            }
        }
    }
}

V2_FIXTURE(csr_swap_exchanges_atomically) {
    // instruction-reference-control.md, "csr_swap", and the D8 addendum. The canonical use is a
    // handler's first instruction: csr_swap r2 scratch r2 exchanges the interrupted program's
    // r2 with a kernel pointer the kernel parked in the scratch register.
    constexpr std::uint64_t kKernelPointer = 0x0000000000001234ull;
    constexpr std::uint64_t kInterruptedValue = 0xDEADBEEF0BADF00Dull;

    {
        Machine machine;
        Encoder program(kBase);
        // The kernel preloads scratch, then the handler's exchange runs.
        program.op_r_i2(op::kCsrWrite, reg(3), csr::kScratch);
        program.op_r_r_i2(op::kCsrSwap, reg(2), reg(2), csr::kScratch);
        program.op_r_i2(op::kCsrRead, reg(4), csr::kScratch);
        program.halt();
        machine.load(program);
        machine.set(3, kKernelPointer);
        machine.set(2, kInterruptedValue);

        expect_halted(machine.run(), "the trap-entry bootstrap");
        V2_CHECK_EQ(machine.get(2), kKernelPointer);      // the handler gained its pointer
        V2_CHECK_EQ(machine.get(4), kInterruptedValue);   // scratch banked the interrupted r2
    }

    // rd = r0 behaves as a plain csr_write: the old value is discarded, r0 stays zero, and the
    // named register still takes the new value.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_i2(op::kCsrWrite, reg(3), csr::kScratch);
        program.op_r_r_i2(op::kCsrSwap, reg(2), reg(0), csr::kScratch);
        program.op_r_i2(op::kCsrRead, reg(4), csr::kScratch);
        program.halt();
        machine.load(program);
        machine.set(3, kKernelPointer);
        machine.set(2, kInterruptedValue);

        expect_halted(machine.run(), "csr_swap with rd = r0");
        V2_CHECK_EQ(machine.get(0), 0u);
        V2_CHECK_EQ(machine.get(4), kInterruptedValue);
    }

    // rs = r0 writes zero and still delivers the old value.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_i2(op::kCsrWrite, reg(3), csr::kScratch);
        program.op_r_r_i2(op::kCsrSwap, reg(0), reg(5), csr::kScratch);
        program.op_r_i2(op::kCsrRead, reg(4), csr::kScratch);
        program.halt();
        machine.load(program);
        machine.set(3, kKernelPointer);
        machine.set(5, kSentinel);

        expect_halted(machine.run(), "csr_swap with rs = r0");
        V2_CHECK_EQ(machine.get(5), kKernelPointer);  // the prior value still arrives
        V2_CHECK_EQ(machine.get(4), 0u);              // and zero went in
    }

    // Distinct rs and rd on a register that is not scratch, so the exchange is not a property of
    // one register or of naming the same register twice.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_i2(op::kCsrWrite, reg(3), csr::kTrapVectorBase);
        program.op_r_r_i2(op::kCsrSwap, reg(6), reg(7), csr::kTrapVectorBase);
        program.op_r_i2(op::kCsrRead, reg(8), csr::kTrapVectorBase);
        program.halt();
        machine.load(program);
        machine.set(3, 0x0000000000010800ull);  // 2 KiB aligned, as the register requires
        machine.set(6, 0x0000000000020000ull);
        machine.set(7, kSentinel);

        expect_halted(machine.run(), "csr_swap with distinct rs and rd");
        V2_CHECK_EQ(machine.get(7), 0x0000000000010800ull);
        V2_CHECK_EQ(machine.get(8), 0x0000000000020000ull);
    }
}

V2_FIXTURE(csr_swap_traps_exactly_as_csr_write) {
    // "Exactly the traps of a csr_write to the same number with the same value." Every trapping
    // condition the two share is run through both instructions and the whole trap record is
    // compared, so a csr_swap that grew its own copy of the rules and drifted fails here.
    struct Case {
        const char* what;
        std::uint16_t number;
        std::uint64_t value;
        Privilege level;
        std::uint8_t cause_number;
        std::uint8_t subcode_number;
        std::uint64_t aux;
    };

    const Case cases[] = {
        {"a reserved privilege field", 0x8000, 0x10, Privilege::Supervisor,
         cause::kIllegalOperand, subcode::kReservedCsrPrivilege, 0x8000},
        {"a supervisor number at user level", csr::kScratch, 0x10, Privilege::User,
         cause::kPrivilegedOperation, 0, csr::kScratch},
        {"a read-only number", csr::kBootInfo, 0x10, Privilege::Supervisor,
         cause::kIllegalOperand, subcode::kReadOnlyCsr, csr::kBootInfo},
        {"an unimplemented number", 0x4100, 0x10, Privilege::Supervisor, cause::kIllegalOperand,
         subcode::kUnimplementedCsr, 0x4100},
        {"a misaligned trap stack", csr::kTrapStack, 0x1008, Privilege::Supervisor,
         cause::kIllegalOperand, subcode::kInvalidCsrValue, 0x1008},
        {"a reserved status bit", csr::kStatus, 0x9, Privilege::Supervisor,
         cause::kIllegalOperand, subcode::kInvalidCsrValue, 0x9},
        {"a reserved privilege encoding in status", csr::kStatus, 0x2, Privilege::Supervisor,
         cause::kIllegalOperand, subcode::kInvalidCsrValue, 0x2},
    };

    for (const Case& one : cases) {
        for (Which which : {Which::Write, Which::Swap}) {
            run_access(which, one.number, one.value, one.level,
                       [&](Machine& machine, const StepResult& result) {
                           char buffer[256];
                           std::snprintf(buffer, sizeof(buffer), "%s through %s", one.what,
                                         which_name(which));
                           expect_trap(result, one.cause_number, one.subcode_number, one.aux,
                                       kBase, buffer);
                           // Neither rd nor the named register changes.
                           V2_CHECK_EQ(machine.get(kDestinationRegister), kSentinel);
                           if (CsrFileV2::is_implemented(one.number)) {
                               V2_CHECK(machine.interpreter().csr().host_read(one.number) !=
                                        one.value);
                           }
                       });
        }
    }
}

V2_FIXTURE(csr_value_validation_is_per_register) {
    // Six registers carry rules the base table had no room for, and every one of them rejects
    // with subcode 6, whose auxiliary word is the offending VALUE rather than the register
    // number (trap-model.md, cause 1 row: "Under subcode 6 the offending value is the value the
    // write supplied, not the register number").
    //
    // Each case writes an ACCEPTED value first, then the rejected one, then reads the register
    // back through the guest's own csr_read, so "the register keeps its prior value" is
    // observed rather than assumed.
    struct Case {
        const char* what;
        std::uint16_t number;
        std::uint64_t accepted;
        std::uint64_t rejected;
    };

    const Case cases[] = {
        // trap-model.md "The status word": %10 and %11 are reserved privilege encodings, and
        // bits 63:3 are reserved.
        {"status naming reserved privilege %10", csr::kStatus, 0x5, 0x2},
        {"status naming reserved privilege %11", csr::kStatus, 0x1, 0x3},
        {"status setting a reserved bit", csr::kStatus, 0x5, 0x8},
        // "trap_stack requires 16-byte alignment."
        {"a misaligned trap stack", csr::kTrapStack, 0x0000000000000FF0ull, 0x0000000000000FF8ull},
        // "trap_vector_base requires 2 KiB alignment."
        {"a trap vector base with low bits set", csr::kTrapVectorBase, 0x0000000000018800ull,
         0x0000000000018400ull},
        // "bits 0 through 31 correspond to synchronous causes, which are never maskable, so
        // those bits read as zero and a write that sets any of them raises the illegal-operand
        // trap."
        {"an enable for a synchronous cause", csr::kInterruptEnable0,
         0xFFFFFFFF00000000ull, 0x0000000100000000ull | 0x1u},
        // "Bit 0 selects the syscall provider; every other bit is reserved."
        {"a reserved syscall-provider bit", csr::kSyscallProvider, 0x1, 0x2},
        // floating-point.md: bits 63 through 8 are reserved and a write of anything other than
        // zero into them raises the trap. A reserved frm encoding is NOT rejected here; that
        // chapter is explicit that the write succeeds and the next rounding operation traps.
        {"an fcsr reserved bit", csr::kFcsr, 0x00000000000000FFull, 0x0000000000000100ull},
        // "The paging-root register": mode 2 through 15 are reserved, and bits 11:4 are written
        // as zero.
        {"a reserved paging mode", csr::kPagingRoot, 0x0000000000002001ull,
         0x0000000000002002ull},
        {"a paging root with reserved bits set", csr::kPagingRoot, 0x0000000000002001ull,
         0x0000000000002011ull},
    };

    for (const Case& one : cases) {
        Machine machine;
        Encoder program(kBase);
        program.op_r_i2(op::kCsrWrite, reg(1), one.number);   // accepted
        program.op_r_i2(op::kCsrWrite, reg(2), one.number);   // rejected
        program.op_r_i2(op::kCsrRead, reg(5), one.number);
        program.halt();
        machine.load(program);
        machine.set(1, one.accepted);
        machine.set(2, one.rejected);

        const StepResult first = machine.step();
        if (first.status != StepStatus::Advanced) {
            record_failure(std::string(one.what) +
                           ": the accepted value was rejected, so the case proves nothing");
            continue;
        }
        const StepResult trapped = machine.step();
        expect_trap(trapped, cause::kIllegalOperand, subcode::kInvalidCsrValue, one.rejected,
                    kBase + 4, one.what);

        machine.interpreter().set_pc(kBase + 8);
        expect_halted(machine.run(), one.what);
        V2_CHECK_EQ(machine.get(5), one.accepted);
    }

    // interrupt_enable0's read rule is a second, separate statement: bits 0 through 31 read as
    // zero. The accepted write above already set only high bits, so this reads back a value
    // whose low half has to be zero on any machine and whose high half has to survive.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_i2(op::kCsrWrite, reg(1), csr::kInterruptEnable0);
        program.op_r_i2(op::kCsrRead, reg(5), csr::kInterruptEnable0);
        program.halt();
        machine.load(program);
        machine.set(1, 0xFFFFFFFF00000000ull);
        expect_halted(machine.run(), "interrupt_enable0 read-back");
        V2_CHECK_EQ(machine.get(5), 0xFFFFFFFF00000000ull);
    }

    // A write to the status register IS a privilege change, because the privilege field of that
    // register is where the live level lives. Dropping to user level and then executing halt
    // proves it: the same instruction that halts the machine one line earlier now faults.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_i2(op::kCsrWrite, reg(1), csr::kStatus);
        program.halt();
        machine.load(program);
        machine.set(1, 0x0);  // user level, interrupts disabled

        V2_CHECK(machine.step().status == StepStatus::Advanced);
        V2_CHECK(machine.interpreter().privilege() == Privilege::User);
        expect_trap(machine.step(), cause::kPrivilegedOperation, 0, op::kHalt, kBase + 4,
                    "halt after dropping to user level");
        V2_CHECK(!machine.interpreter().halted());
    }

    // The paging-root seam. This card owns the register and the fact that a write asks for
    // cached translations to be flushed; maize-465 owns the cache that will answer. The chapter
    // says every write flushes "whether or not the write changes the value", so the second
    // write of the same value has to count too.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_i2(op::kCsrWrite, reg(1), csr::kPagingRoot);
        program.op_r_i2(op::kCsrWrite, reg(1), csr::kPagingRoot);
        program.op_r_i2(op::kCsrRead, reg(5), csr::kPagingRoot);
        program.halt();
        machine.load(program);
        machine.set(1, 0x0000000000003001ull);  // Sv48, root at physical $3000

        V2_CHECK_EQ(machine.interpreter().csr().translation_flushes(), 0u);
        expect_halted(machine.run(), "the paging-root flush seam");
        V2_CHECK_EQ(machine.interpreter().csr().translation_flushes(), 2u);
        V2_CHECK_EQ(machine.get(5), 0x0000000000003001ull);
    }
}

V2_FIXTURE(csr_reset_state) {
    // "At reset every writable register in the table holds zero except status, which holds $1,
    // the supervisor level with interrupts disabled. The read-only registers hold what the
    // machine has to report: feature_bitmap and boot_info are populated before the first
    // instruction executes, the pending registers are clear, and halt_cause is zero."
    //
    // In this build the two populated registers report zero, because no extension is
    // implemented and the boot-information block is maize-421. What the fixture pins is that
    // the machine reports whatever the host populated, and that everything else is zero.
    {
        Machine machine;
        const CsrFileV2& csr_file = machine.interpreter().csr();
        V2_CHECK_EQ(csr_file.host_read(csr::kStatus), 0x1u);
        V2_CHECK(machine.interpreter().privilege() == Privilege::Supervisor);
        V2_CHECK(!csr_file.interrupts_enabled());
        V2_CHECK_EQ(csr_file.translation_flushes(), 0u);

        for (std::uint16_t number : kBaseNumbers) {
            if (number == csr::kStatus) {
                continue;
            }
            if (csr_file.host_read(number) != 0u) {
                char buffer[160];
                std::snprintf(buffer, sizeof(buffer), "$%04X is not zero at reset",
                              static_cast<unsigned>(number));
                record_failure(buffer);
            }
        }
    }

    // The guest's own view of the same thing, read through csr_read rather than through the
    // host accessor, and with the two populated read-only registers actually populated so the
    // fixture is not agreeing with zero by accident.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_i2(op::kCsrRead, reg(5), csr::kStatus);
        program.op_r_i2(op::kCsrRead, reg(6), csr::kFeatureBitmap);
        program.op_r_i2(op::kCsrRead, reg(7), csr::kBootInfo);
        program.op_r_i2(op::kCsrRead, reg(8), csr::kHaltCause);
        program.op_r_i2(op::kCsrRead, reg(9), csr::kInterruptPending0);
        program.halt();
        machine.load(program);
        machine.interpreter().csr().host_set_feature_bitmap(0x0000000000000005ull);
        machine.interpreter().csr().host_set_boot_info(0x0000000000004000ull);

        expect_halted(machine.run(), "the reset-state read-back");
        V2_CHECK_EQ(machine.get(5), 0x1u);
        V2_CHECK_EQ(machine.get(6), 0x0000000000000005ull);
        V2_CHECK_EQ(machine.get(7), 0x0000000000004000ull);
        V2_CHECK_EQ(machine.get(8), 0u);
        V2_CHECK_EQ(machine.get(9), 0u);
    }
}

V2_FIXTURE(scratch_register_contract) {
    // "$4009 scratch: a scratch word the machine itself never reads or writes, held for the
    // trap-entry register bootstrap." Supervisor-only, read-write, no side effect, reset zero,
    // and it rejects no value.
    const std::uint64_t patterns[] = {
        0x0000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0x8000000000000000ull,
        0x0000000000000001ull, 0x0123456789ABCDEFull, 0xFFFFFFFF00000000ull,
    };

    for (std::uint64_t pattern : patterns) {
        Machine machine;
        Encoder program(kBase);
        program.op_r_i2(op::kCsrWrite, reg(1), csr::kScratch);
        program.op_r_i2(op::kCsrRead, reg(5), csr::kScratch);
        program.halt();
        machine.load(program);
        machine.set(1, pattern);

        // Everything else, before and after, so "no side effect observable in any other
        // register or CSR" is a comparison rather than a claim.
        CsrFileV2& csr_file = machine.interpreter().csr();
        std::uint64_t before[kBaseNumberCount] = {};
        for (unsigned i = 0; i < kBaseNumberCount; ++i) {
            before[i] = csr_file.host_read(kBaseNumbers[i]);
        }

        expect_halted(machine.run(), "a scratch round trip");
        V2_CHECK_EQ(machine.get(5), pattern);

        for (unsigned i = 0; i < kBaseNumberCount; ++i) {
            if (kBaseNumbers[i] == csr::kScratch) {
                continue;
            }
            if (csr_file.host_read(kBaseNumbers[i]) != before[i]) {
                char buffer[192];
                std::snprintf(buffer, sizeof(buffer),
                              "a scratch write changed $%04X, which it must not",
                              static_cast<unsigned>(kBaseNumbers[i]));
                record_failure(buffer);
            }
        }
        V2_CHECK_EQ(csr_file.translation_flushes(), 0u);
    }

    // Reset value, and the standard privilege check rather than any special-cased path.
    V2_CHECK_EQ(Machine().interpreter().csr().host_read(csr::kScratch), 0u);
    expect_same_trap_on_each(csr::kScratch, 0x10, Privilege::User, cause::kPrivilegedOperation, 0,
                             csr::kScratch, false, "scratch from user level");
}

V2_FIXTURE(privileged_instructions_are_privileged_at_user_level) {
    // "Seven instructions are privileged, and executing any of them at user level raises the
    // privileged-operation fault. They are trap_return, halt, wait_for_interrupt,
    // tlb_invalidate_all, tlb_invalidate_address, port_in, and port_out." The list is closed,
    // and cause 4's auxiliary word here is the offending opcode byte, since none of the seven
    // names a control and status register.
    struct Case {
        const char* what;
        std::uint8_t opcode;
        std::vector<std::uint8_t> bytes;
    };

    const Case privileged[] = {
        {"trap_return", op::kTrapReturn, {op::kTrapReturn}},
        {"halt", op::kHalt, {op::kHalt}},
        {"wait_for_interrupt", op::kWaitForInterrupt, {op::kWaitForInterrupt}},
        {"tlb_invalidate_all", op::kTlbInvalidateAll, {op::kTlbInvalidateAll}},
        {"tlb_invalidate_address", op::kTlbInvalidateAddress, {op::kTlbInvalidateAddress, 0x04}},
        {"port_in", op::kPortIn, {op::kPortIn, 0x04, 0x05}},
        {"port_out", op::kPortOut, {op::kPortOut, 0x04, 0x05}},
    };

    for (const Case& one : privileged) {
        Machine machine;
        Encoder program(kBase);
        for (std::uint8_t byte : one.bytes) {
            program.raw_byte(byte);
        }
        machine.load(program);
        machine.set(4, 0x1000u);
        machine.set(5, kSentinel);
        machine.interpreter().host_set_privilege(Privilege::User);

        const StepResult result = machine.step();
        expect_trap(result, cause::kPrivilegedOperation, 0, one.opcode, kBase, one.what);
        // No other effect: the machine did not stop, no destination register moved, and no
        // byte reached a device.
        V2_CHECK(!machine.interpreter().halted());
        V2_CHECK_EQ(machine.get(5), kSentinel);
        V2_CHECK(machine.interpreter().device_surface().console_output().empty());
    }

    // The list is CLOSED at seven. sys is the intended way for user code to enter the kernel
    // and breakpoint has to be plantable in user code, so neither is privileged. Both are still
    // unimplemented in this build (maize-464), and the point here is that what comes back is
    // the host's scaffold diagnostic rather than cause 4.
    const Case not_privileged[] = {
        {"sys #imm", op::kSysImm, {op::kSysImm, 0x2A}},
        {"sys rs", op::kSysReg, {op::kSysReg, 0x04}},
        {"breakpoint", op::kBreakpoint, {op::kBreakpoint}},
    };

    for (const Case& one : not_privileged) {
        Machine machine;
        Encoder program(kBase);
        for (std::uint8_t byte : one.bytes) {
            program.raw_byte(byte);
        }
        machine.load(program);
        machine.interpreter().host_set_privilege(Privilege::User);

        const StepResult result = machine.step();
        expect_unimplemented(result, one.opcode, one.what);
        V2_CHECK(result.status != StepStatus::Trapped);
    }

    // The same seven at supervisor level do not raise cause 4, so the fixture tests the
    // privilege check rather than seven instructions that fault unconditionally. halt halts,
    // the two port instructions run, and the other four reach the host diagnostic because their
    // bodies belong to maize-464 through maize-466.
    for (const Case& one : privileged) {
        Machine machine;
        Encoder program(kBase);
        for (std::uint8_t byte : one.bytes) {
            program.raw_byte(byte);
        }
        machine.load(program);
        machine.set(4, 0x1000u);
        const StepResult result = machine.step();
        if (result.status == StepStatus::Trapped) {
            char buffer[192];
            std::snprintf(buffer, sizeof(buffer),
                          "%s at supervisor level raised cause %u, and it must not",
                          one.what, result.trap.cause);
            record_failure(buffer);
        }
    }
}

}  // namespace maize::v2::test
