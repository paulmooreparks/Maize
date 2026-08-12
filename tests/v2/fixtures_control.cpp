// fixtures_control.cpp (maize-418): branches, jump, call, return, and the select pair.

#include <cstdio>

#include "fixture_support.h"
#include "predicate_oracle.h"

namespace maize::v2::test {
namespace {

constexpr std::uint64_t kBase = 0x100;
constexpr std::uint64_t kSentinel = 0x0123456789ABCDEFull;

}  // namespace

V2_FIXTURE(branches_agree_with_compares) {
    // The ten branch predicates are exactly the ten compare predicates, evaluated on the same
    // two registers in the same order, so a branch and the compare of the same relation can
    // never disagree. The expectations here are pinned to appendix A.5 through
    // predicate_oracle.h and never computed by the interpreter's own predicate function, so a
    // permuted mapping fails this fixture instead of agreeing with itself.
    //
    // The branch is seven bytes. A displacement of three reaches past the three-byte move.zb
    // that follows it, so r4 ends up 1 when the branch was NOT taken and 0 when it was, and the
    // fixture reads the branch's own behaviour rather than a side effect of the target. A fresh
    // machine per pair, because halt is terminal and a halted machine takes no further
    // instruction.
    auto run_branch = [](unsigned predicate, std::uint64_t left, std::uint64_t right) {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_i4(static_cast<std::uint8_t>(op::kBranchBase + predicate), reg(1), reg(2),
                          3);
        program.op_r_i1(op::kMoveZb, reg(4), 1);
        program.halt();
        machine.load(program);
        machine.set(1, left);
        machine.set(2, right);
        expect_halted(machine.run(), "a branch fixture");
        // 0 means the branch was taken and skipped the marker, 1 means it fell through.
        return machine.get(4) == 0u;
    };

    // The pin table first: nine hand-written pairs with hand-written answers, four of them
    // where the signed and unsigned answers diverge. Each predicate has a distinct signature
    // across the nine, so ANY permutation of the ten is caught.
    for (unsigned predicate = 0; predicate < kPredicateCount; ++predicate) {
        for (unsigned index = 0; index < kPredicatePinCount; ++index) {
            const PredicatePin& pin = kPredicatePins[index];
            const bool taken = run_branch(predicate, pin.left, pin.right);
            if (taken != pinned_answer(pin, predicate)) {
                char buffer[192];
                std::snprintf(buffer, sizeof(buffer), "branch_%s on pin %u %s",
                              kPredicateNames[predicate], index,
                              taken ? "was taken and should not have been"
                                    : "was not taken and should have been");
                record_failure(buffer);
            }
        }
    }

    // Then the sweep, against the second transcription rather than the production function.
    const std::uint64_t values[] = {
        0, 1, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull,
    };

    for (unsigned predicate = 0; predicate < kPredicateCount; ++predicate) {
        for (std::uint64_t left : values) {
            for (std::uint64_t right : values) {
                const bool taken = run_branch(predicate, left, right);
                if (taken != oracle_predicate(predicate, left, right)) {
                    char buffer[192];
                    std::snprintf(buffer, sizeof(buffer),
                                  "branch_%s on $%016llX and $%016llX",
                                  kPredicateNames[predicate],
                                  static_cast<unsigned long long>(left),
                                  static_cast<unsigned long long>(right));
                    record_failure(buffer);
                }
            }
        }
    }

    // A displacement of zero falls through to the following instruction, since the
    // displacement is measured from the address of the instruction after the branch.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_i4(op::kBranchBase, reg(0), reg(0), 0);  // branch_eq r0 r0, displacement 0
        program.op_r_i1(op::kMoveZb, reg(4), 1);
        program.halt();
        machine.load(program);
        expect_halted(machine.run(), "a taken branch with a displacement of zero");
        V2_CHECK_EQ(machine.get(4), 1u);
    }

    // A negative displacement, which is the loop shape, and the encoding the chapter works
    // through: a displacement of -24 is $FFFFFFE8 little-endian.
    {
        Encoder program(kBase);
        program.op_r_r_i4(op::kBranchBase + 2, reg(4), reg(5), 0xFFFFFFE8ull);
        const std::vector<std::uint8_t> expected = {0x62, 0x04, 0x05, 0xE8, 0xFF, 0xFF, 0xFF};
        V2_CHECK(program.bytes() == expected);
    }

    // A branch writes no register at all, taken or not.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_i4(op::kBranchBase, reg(1), reg(2), 1).raw_byte(0x00).halt();
        machine.load(program);
        machine.set(1, 5);
        machine.set(2, 5);
        machine.set(31, kSentinel);
        expect_halted(machine.run(), "a taken branch over a guard byte");
        V2_CHECK_EQ(machine.get(1), 5u);
        V2_CHECK_EQ(machine.get(2), 5u);
        V2_CHECK_EQ(machine.get(31), kSentinel);
    }
}

V2_FIXTURE(jump_call_and_return) {
    // jump with a displacement of zero falls through to the following instruction, and neither
    // form of jump disturbs any register, r31 included.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_i4(op::kJumpDisp, 0);
        program.op_r_i1(op::kMoveZb, reg(4), 1);
        program.halt();
        machine.load(program);
        machine.set(31, kSentinel);
        expect_halted(machine.run(), "jump with a displacement of zero");
        V2_CHECK_EQ(machine.get(4), 1u);
        V2_CHECK_EQ(machine.get(31), kSentinel);
    }

    // The register form of jump takes an absolute address from the whole register.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r(op::kJumpReg, reg(5));               // $100, two bytes
        program.op_r_i1(op::kMoveZb, reg(4), 1);          // $102, skipped
        program.op_r_i1(op::kMoveZb, reg(6), 2);          // $105, the target
        program.halt();
        machine.load(program);
        machine.set(5, kBase + 5);
        machine.set(31, kSentinel);
        expect_halted(machine.run(), "jump r5");
        V2_CHECK_EQ(machine.get(4), 0u);
        V2_CHECK_EQ(machine.get(6), 2u);
        V2_CHECK_EQ(machine.get(31), kSentinel);
    }

    // call writes the address of the instruction after the call into r31 and disturbs nothing
    // else; return transfers to whatever r31 holds.
    {
        // $100 call, displacement 4, five bytes, so the link is $105 and the target is $109.
        // $105 move.zb $01 r4, three bytes, reached only after the return.
        // $108 halt.
        // $109 move.zb $02 r6, the callee.
        // $10C return.
        Machine machine;
        Encoder program(kBase);
        program.op_i4(op::kCallDisp, 4);
        const std::uint64_t after_call = program.current_address();
        program.op_r_i1(op::kMoveZb, reg(4), 1);
        program.halt();
        const std::uint64_t callee = program.current_address();
        program.op_r_i1(op::kMoveZb, reg(6), 2);
        program.op(op::kReturn);
        machine.load(program);
        machine.set(7, kSentinel);

        V2_CHECK_EQ(after_call, kBase + 5);
        V2_CHECK_EQ(callee, kBase + 9);

        const StepResult result = machine.run();
        expect_halted(result, "call target then return");
        V2_CHECK_EQ(machine.get(31), after_call);
        V2_CHECK_EQ(machine.get(6), 2u);
        V2_CHECK_EQ(machine.get(4), 1u);  // reached after the return
        V2_CHECK_EQ(machine.get(7), kSentinel);
    }

    // call r31 is well defined: the register form reads its target BEFORE it writes the link,
    // so the transfer goes to the address r31 held and r31 is left holding the address after
    // the call.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r(op::kCallReg, reg(31));       // $100, two bytes
        const std::uint64_t after_call = kBase + 2;
        program.op_r_i1(op::kMoveZb, reg(4), 1);   // $102, skipped
        program.op_r_i1(op::kMoveZb, reg(6), 2);   // $105, the target
        program.halt();
        machine.load(program);
        machine.set(31, kBase + 5);
        expect_halted(machine.run(), "call r31");
        V2_CHECK_EQ(machine.get(6), 2u);
        V2_CHECK_EQ(machine.get(4), 0u);
        V2_CHECK_EQ(machine.get(31), after_call);
    }

    // A call disturbs no register other than r31.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_i4(op::kCallDisp, 0).halt();
        machine.load(program);
        for (unsigned n = 1; n < 31; ++n) {
            machine.set(n, kSentinel + n);
        }
        expect_halted(machine.run(), "call with a displacement of zero");
        for (unsigned n = 1; n < 31; ++n) {
            V2_CHECK_EQ(machine.get(n), kSentinel + n);
        }
        V2_CHECK_EQ(machine.get(31), kBase + 5);
    }
}

V2_FIXTURE(select_leaves_the_destination_alone) {
    // When the condition does not hold the destination is not written AT ALL, which is the
    // difference between select and a masked arithmetic sequence. The condition is the whole
    // 64-bit value tested against zero, so no single bit is privileged.
    struct Case {
        const char* what;
        std::uint8_t opcode;
        std::uint64_t condition;
        bool writes;
    };

    const Case cases[] = {
        {"select_nz with a nonzero condition", op::kSelectNz, 1, true},
        {"select_nz with a high-bit-only condition", op::kSelectNz, 0x8000000000000000ull, true},
        {"select_nz with a zero condition", op::kSelectNz, 0, false},
        {"select_z with a zero condition", op::kSelectZ, 0, true},
        {"select_z with a nonzero condition", op::kSelectZ, 0x0000000100000000ull, false},
    };

    for (const Case& one : cases) {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(one.opcode, reg(7), reg(6), reg(4)).halt();
        machine.load(program);
        machine.set(7, 0xAAAAAAAAAAAAAAAAull);
        machine.set(6, one.condition);
        machine.set(4, kSentinel);
        expect_halted(machine.run(), one.what);
        check_equal_u64(machine.get(4), one.writes ? 0xAAAAAAAAAAAAAAAAull : kSentinel, one.what,
                        __FILE__, __LINE__);
        // Neither instruction modifies the condition register or the source register.
        V2_CHECK_EQ(machine.get(7), 0xAAAAAAAAAAAAAAAAull);
        V2_CHECK_EQ(machine.get(6), one.condition);
    }

    // Bit-for-bit unchanged INCLUDING when the destination is the same register as the source,
    // which is the case an implementation that writes unconditionally and then repairs would
    // get wrong.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(op::kSelectNz, reg(4), reg(6), reg(4)).halt();
        machine.load(program);
        machine.set(4, kSentinel);
        machine.set(6, 0);
        expect_halted(machine.run(), "select_nz with rs and rd naming one register");
        V2_CHECK_EQ(machine.get(4), kSentinel);
    }

    // The branchless maximum the chapter builds out of the pair.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(op::kCompareLtSigned, reg(4), reg(5), reg(6))
            .op_r_r_r(op::kSelectNz, reg(5), reg(6), reg(4))
            .halt();
        machine.load(program);
        machine.set(4, static_cast<std::uint64_t>(-5));
        machine.set(5, 9);
        expect_halted(machine.run(), "the branchless signed maximum");
        V2_CHECK_EQ(machine.get(4), 9u);
    }
}

V2_FIXTURE(halt_stops_the_machine) {
    // halt stops the machine: it fetches no further instruction and the register and memory
    // state at that point is final, which is what lets a fixture end with halt and have its
    // results inspected.
    Machine machine;
    Encoder program(kBase);
    program.op_r_i1(op::kMoveZb, reg(4), 7).halt().op_r_i1(op::kMoveZb, reg(4), 9);
    machine.load(program);
    const StepResult result = machine.run();
    expect_halted(result, "a program that ends in halt");
    V2_CHECK_EQ(machine.get(4), 7u);
    V2_CHECK(machine.interpreter().halted());

    // Stepping a halted machine takes no further instruction.
    const StepResult again = machine.step();
    expect_halted(again, "stepping an already halted machine");
    V2_CHECK_EQ(machine.get(4), 7u);
}

}  // namespace maize::v2::test
