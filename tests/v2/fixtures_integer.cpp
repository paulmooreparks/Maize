// fixtures_integer.cpp (maize-418): constants and moves, integer arithmetic and logic, compares.

#include <cstdio>

#include "fixture_support.h"
#include "predicate_oracle.h"

namespace maize::v2::test {
namespace {

constexpr std::uint64_t kBase = 0x100;
constexpr std::uint64_t kSentinel = 0x0123456789ABCDEFull;

}  // namespace

V2_FIXTURE(constants_and_moves) {
    struct Case {
        const char* what;
        std::uint8_t opcode;
        std::uint64_t immediate;
        std::uint64_t expected;
    };

    // Every value here is the one the reference chapter's own example produces.
    const Case cases[] = {
        {"move.w $FFFFFFFFFFFFFFFF r7", op::kMoveW, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull},
        {"move.w $1122334455667788 r7", op::kMoveW, 0x1122334455667788ull, 0x1122334455667788ull},
        {"move.zb $FF r7", op::kMoveZb, 0xFF, 0x00000000000000FFull},
        {"move.sb $FF r7", op::kMoveSb, 0xFF, 0xFFFFFFFFFFFFFFFFull},
        {"move.zq #1000 r7", op::kMoveZq, 1000, 1000},
        {"move.sq $8000 r7", op::kMoveSq, 0x8000, 0xFFFFFFFFFFFF8000ull},
        {"move.zh $DEADBEEF r7", op::kMoveZh, 0xDEADBEEFull, 0x00000000DEADBEEFull},
        {"move.sh $FFFFFFF8 r7", op::kMoveSh, 0xFFFFFFF8ull, 0xFFFFFFFFFFFFFFF8ull},
    };

    for (const Case& one : cases) {
        Machine machine;
        Encoder program(kBase);
        switch (kOpcodeTable[one.opcode].shape) {
            case Shape::OpRI1: program.op_r_i1(one.opcode, reg(7), one.immediate); break;
            case Shape::OpRI2: program.op_r_i2(one.opcode, reg(7), one.immediate); break;
            case Shape::OpRI4: program.op_r_i4(one.opcode, reg(7), one.immediate); break;
            default: program.op_r_i8(one.opcode, reg(7), one.immediate); break;
        }
        program.halt();
        machine.load(program);
        machine.set(7, kSentinel);
        expect_halted(machine.run(), one.what);
        check_equal_u64(machine.get(7), one.expected, one.what, __FILE__, __LINE__);
    }

    // move rs rd writes all 64 bits, and naming the same register on both sides still performs
    // the write and leaves the value alone.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r(op::kMove, reg(9), reg(4)).op_r_r(op::kMove, reg(4), reg(4)).halt();
        machine.load(program);
        machine.set(9, kSentinel);
        expect_halted(machine.run(), "move r9 r4 then move r4 r4");
        V2_CHECK_EQ(machine.get(4), kSentinel);
        V2_CHECK_EQ(machine.get(9), kSentinel);
    }

    // move r0 rd is the clear-to-zero the family relies on r0 for, and a write to r0 is
    // discarded while every other effect still happens.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r(op::kMove, reg(0), reg(4)).op_r_i1(op::kMoveZb, reg(0), 0xFF).halt();
        machine.load(program);
        machine.set(4, kSentinel);
        expect_halted(machine.run(), "move r0 r4 and move.zb $FF r0");
        V2_CHECK_EQ(machine.get(4), 0u);
        V2_CHECK_EQ(machine.get(0), 0u);
    }
}

V2_FIXTURE(pc_add_reads_the_following_instruction_address) {
    // pc_add adds the sign-extended 32-bit literal to the address of the instruction FOLLOWING
    // this one, and the instruction is six bytes, so pc_add #0 rd writes its own address plus
    // six. This is the only way software reads the program counter.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_i4(op::kPcAdd, reg(6), 0).halt();
        machine.load(program);
        expect_halted(machine.run(), "pc_add #0 r6");
        V2_CHECK_EQ(machine.get(6), kBase + 6);
    }
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_i4(op::kPcAdd, reg(6), 0x20).halt();
        machine.load(program);
        expect_halted(machine.run(), "pc_add $20 r6");
        V2_CHECK_EQ(machine.get(6), kBase + 6 + 0x20);
    }
    {
        // A negative literal, to prove the 32-bit field is sign-extended before the addition.
        Machine machine;
        Encoder program(kBase);
        program.op_r_i4(op::kPcAdd, reg(6), 0xFFFFFFF0ull).halt();
        machine.load(program);
        expect_halted(machine.run(), "pc_add $-16 r6");
        V2_CHECK_EQ(machine.get(6), kBase + 6 - 16);
    }
}

V2_FIXTURE(half_word_operations_zero_extend) {
    // Every .h instruction leaves bits 63 through 32 of its destination zero, for every input,
    // INCLUDING inputs whose 32-bit result has bit 31 set. Each source below carries a nonzero
    // upper half as well, so an implementation that failed to ignore it would also be caught.
    struct Binary {
        const char* what;
        std::uint8_t opcode;
        std::uint64_t a;
        std::uint64_t b;
        std::uint32_t expected;
    };

    const Binary binaries[] = {
        {"add.h", op::kAddH, 0xFFFFFFFF80000000ull, 0x1111111100000001ull, 0x80000001u},
        {"subtract.h", op::kSubtractH, 0xFFFFFFFF00000000ull, 0x1111111100000001ull, 0xFFFFFFFFu},
        {"multiply.h", op::kMultiplyH, 0xFFFFFFFF40000000ull, 0x1111111100000002ull, 0x80000000u},
        {"divide_signed.h", op::kDivideSignedH, 0xFFFFFFFF80000000ull, 0x1111111100000002ull,
         0xC0000000u},
        {"divide_unsigned.h", op::kDivideUnsignedH, 0xFFFFFFFFFFFFFFFFull, 0x1111111100000001ull,
         0xFFFFFFFFu},
        {"remainder_signed.h", op::kRemainderSignedH, 0xFFFFFFFFFFFFFFF9ull,
         0x1111111100000002ull, 0xFFFFFFFFu},
        {"remainder_unsigned.h", op::kRemainderUnsignedH, 0xFFFFFFFF80000000ull,
         0x11111111C0000000ull, 0x80000000u},
        {"shift_left.h", op::kShiftLeftH, 0xFFFFFFFF00000001ull, 0x111111110000001Full,
         0x80000000u},
        {"shift_right_logical.h", op::kShiftRightLogicalH, 0xFFFFFFFFFFFFFFFFull,
         0x1111111100000000ull, 0xFFFFFFFFu},
        {"shift_right_arithmetic.h", op::kShiftRightArithmeticH, 0xFFFFFFFF80000000ull,
         0x1111111100000001ull, 0xC0000000u},
    };

    for (const Binary& one : binaries) {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(one.opcode, reg(1), reg(2), reg(3)).halt();
        machine.load(program);
        machine.set(1, one.a);
        machine.set(2, one.b);
        machine.set(3, kSentinel);
        expect_halted(machine.run(), one.what);
        check_equal_u64(machine.get(3), static_cast<std::uint64_t>(one.expected), one.what,
                        __FILE__, __LINE__);
    }

    struct Unary {
        const char* what;
        std::uint8_t opcode;
        std::uint64_t a;
        std::uint32_t expected;
    };

    const Unary unaries[] = {
        {"not.h", op::kNotH, 0xFFFFFFFF00000000ull, 0xFFFFFFFFu},
        {"negate.h", op::kNegateH, 0xFFFFFFFF80000000ull, 0x80000000u},
        {"byte_reverse.h", op::kByteReverseH, 0xFFFFFFFF000000FFull, 0xFF000000u},
    };

    for (const Unary& one : unaries) {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r(one.opcode, reg(1), reg(3)).halt();
        machine.load(program);
        machine.set(1, one.a);
        machine.set(3, kSentinel);
        expect_halted(machine.run(), one.what);
        check_equal_u64(machine.get(3), static_cast<std::uint64_t>(one.expected), one.what,
                        __FILE__, __LINE__);
    }

    // The immediate .h forms take the 32-bit literal as the half-word operand directly, with no
    // extension applied, since the literal and the operation are the same width.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_i4(op::kAddHImm, reg(1), reg(3), 0x80000000ull).halt();
        machine.load(program);
        machine.set(1, 0xFFFFFFFF00000001ull);
        machine.set(3, kSentinel);
        expect_halted(machine.run(), "add.h r1 $80000000 r3");
        V2_CHECK_EQ(machine.get(3), 0x80000001ull);
    }
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_i1(op::kShiftLeftHImm, reg(1), reg(3), 31).halt();
        machine.load(program);
        machine.set(1, 0xFFFFFFFF00000001ull);
        machine.set(3, kSentinel);
        expect_halted(machine.run(), "shift_left.h r1 #31 r3");
        V2_CHECK_EQ(machine.get(3), 0x80000000ull);
    }
}

V2_FIXTURE(shift_counts_are_masked) {
    // A shift takes its count modulo the operation width, so a word shift uses the low 6 bits
    // and a .h shift the low 5, and every count value from 0 through 255 is therefore defined.
    const std::uint64_t word_value = 0x123456789ABCDEF0ull;
    const std::uint64_t half_value = 0xFFFFFFFF9ABCDEF0ull;

    struct Word {
        const char* what;
        std::uint8_t opcode;
    };
    const Word word_shifts[] = {
        {"shift_left", op::kShiftLeft},
        {"shift_right_logical", op::kShiftRightLogical},
        {"shift_right_arithmetic", op::kShiftRightArithmetic},
    };

    for (const Word& one : word_shifts) {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(one.opcode, reg(1), reg(2), reg(3));
        machine.load(program);
        for (unsigned count = 0; count < 256; ++count) {
            machine.set(1, word_value);
            machine.set(2, count);
            machine.set(3, kSentinel);
            machine.interpreter().set_pc(kBase);
            const StepResult result = machine.step();
            V2_CHECK(result.status == StepStatus::Advanced);
            const unsigned masked = count & 63u;
            std::uint64_t expected = 0;
            if (one.opcode == op::kShiftLeft) {
                expected = word_value << masked;
            } else if (one.opcode == op::kShiftRightLogical) {
                expected = word_value >> masked;
            } else {
                expected = static_cast<std::uint64_t>(static_cast<std::int64_t>(word_value) >>
                                                      masked);
            }
            if (machine.get(3) != expected) {
                char buffer[192];
                std::snprintf(buffer, sizeof(buffer), "%s by a count of %u", one.what, count);
                check_equal_u64(machine.get(3), expected, buffer, __FILE__, __LINE__);
            }
        }
    }

    const Word half_shifts[] = {
        {"shift_left.h", op::kShiftLeftH},
        {"shift_right_logical.h", op::kShiftRightLogicalH},
        {"shift_right_arithmetic.h", op::kShiftRightArithmeticH},
    };

    for (const Word& one : half_shifts) {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(one.opcode, reg(1), reg(2), reg(3));
        machine.load(program);
        for (unsigned count = 0; count < 256; ++count) {
            machine.set(1, half_value);
            machine.set(2, count);
            machine.set(3, kSentinel);
            machine.interpreter().set_pc(kBase);
            const StepResult result = machine.step();
            V2_CHECK(result.status == StepStatus::Advanced);
            const unsigned masked = count & 31u;
            const std::uint32_t low = static_cast<std::uint32_t>(half_value);
            std::uint32_t expected = 0;
            if (one.opcode == op::kShiftLeftH) {
                expected = low << masked;
            } else if (one.opcode == op::kShiftRightLogicalH) {
                expected = low >> masked;
            } else {
                expected = static_cast<std::uint32_t>(static_cast<std::int32_t>(low) >> masked);
            }
            if (machine.get(3) != expected) {
                char buffer[192];
                std::snprintf(buffer, sizeof(buffer), "%s by a count of %u", one.what, count);
                check_equal_u64(machine.get(3), expected, buffer, __FILE__, __LINE__);
            }
        }
    }

    // The immediate forms mask the same way, and the count literal is 8 bits, so its
    // signedness never arises.
    struct Immediate {
        const char* what;
        std::uint8_t opcode;
        unsigned count;
        std::uint64_t expected;
    };
    const Immediate immediates[] = {
        {"shift_left #64", op::kShiftLeftImm, 64, word_value},
        {"shift_left #65", op::kShiftLeftImm, 65, word_value << 1},
        {"shift_left #255", op::kShiftLeftImm, 255, word_value << 63},
        {"shift_right_logical #128", op::kShiftRightLogicalImm, 128, word_value},
        {"shift_left.h #32", op::kShiftLeftHImm, 32, static_cast<std::uint32_t>(word_value)},
        {"shift_left.h #33", op::kShiftLeftHImm, 33,
         static_cast<std::uint32_t>(static_cast<std::uint32_t>(word_value) << 1)},
        {"shift_left.h #255", op::kShiftLeftHImm, 255,
         static_cast<std::uint32_t>(static_cast<std::uint32_t>(word_value) << 31)},
    };
    for (const Immediate& one : immediates) {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_i1(one.opcode, reg(1), reg(3), one.count).halt();
        machine.load(program);
        machine.set(1, word_value);
        machine.set(3, kSentinel);
        expect_halted(machine.run(), one.what);
        check_equal_u64(machine.get(3), one.expected, one.what, __FILE__, __LINE__);
    }
}

V2_FIXTURE(divide_and_remainder_trap_without_writing) {
    // A zero divisor raises the divide-error trap with the divide-by-zero subcode, and the
    // destination keeps its prior value. This is the trap-writes-nothing contract at exactly
    // the inputs that catch an implementation which computes before it checks.
    const std::uint8_t dividers[] = {
        op::kDivideSigned,      op::kDivideSignedH,      op::kDivideUnsigned,
        op::kDivideUnsignedH,   op::kRemainderSigned,    op::kRemainderSignedH,
        op::kRemainderUnsigned, op::kRemainderUnsignedH,
    };

    for (std::uint8_t opcode : dividers) {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(opcode, reg(1), reg(2), reg(3)).halt();
        machine.load(program);
        machine.set(1, 100);
        machine.set(2, 0);
        machine.set(3, kSentinel);
        const StepResult result = machine.step();
        char label[96];
        std::snprintf(label, sizeof(label), "opcode $%02X divided by zero", opcode);
        expect_trap(result, cause::kDivideError, subcode::kDivideByZero, 0, kBase, label);
        check_equal_u64(machine.get(3), kSentinel, label, __FILE__, __LINE__);
        // The faulting instruction is restartable, so the program counter has not moved.
        V2_CHECK_EQ(machine.interpreter().pc(), kBase);
    }

    // divide_signed of the most negative word by -1 raises the quotient-overflow subcode,
    // because the true quotient is not representable.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(op::kDivideSigned, reg(1), reg(2), reg(3)).halt();
        machine.load(program);
        machine.set(1, 0x8000000000000000ull);
        machine.set(2, 0xFFFFFFFFFFFFFFFFull);
        machine.set(3, kSentinel);
        const StepResult result = machine.step();
        expect_trap(result, cause::kDivideError, subcode::kQuotientOverflow, 0, kBase,
                    "divide_signed of the most negative word by -1");
        V2_CHECK_EQ(machine.get(3), kSentinel);
    }
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(op::kDivideSignedH, reg(1), reg(2), reg(3)).halt();
        machine.load(program);
        machine.set(1, 0x80000000ull);
        machine.set(2, 0xFFFFFFFFull);
        machine.set(3, kSentinel);
        const StepResult result = machine.step();
        expect_trap(result, cause::kDivideError, subcode::kQuotientOverflow, 0, kBase,
                    "divide_signed.h of the most negative half-word by -1");
        V2_CHECK_EQ(machine.get(3), kSentinel);
    }

    // remainder_signed on the same operands writes zero and raises nothing, because that
    // remainder is exactly zero and is representable even though the quotient is not.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(op::kRemainderSigned, reg(1), reg(2), reg(3)).halt();
        machine.load(program);
        machine.set(1, 0x8000000000000000ull);
        machine.set(2, 0xFFFFFFFFFFFFFFFFull);
        machine.set(3, kSentinel);
        expect_halted(machine.run(), "remainder_signed of the most negative word by -1");
        V2_CHECK_EQ(machine.get(3), 0u);
    }
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(op::kRemainderSignedH, reg(1), reg(2), reg(3)).halt();
        machine.load(program);
        machine.set(1, 0x80000000ull);
        machine.set(2, 0xFFFFFFFFull);
        machine.set(3, kSentinel);
        expect_halted(machine.run(), "remainder_signed.h of the most negative half-word by -1");
        V2_CHECK_EQ(machine.get(3), 0u);
    }

    // Truncation toward zero, which is what C requires: -7 divided by 2 is -3, and the
    // remainder takes the sign of the dividend.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(op::kDivideSigned, reg(1), reg(2), reg(3))
            .op_r_r_r(op::kRemainderSigned, reg(1), reg(2), reg(4))
            .halt();
        machine.load(program);
        machine.set(1, static_cast<std::uint64_t>(-7));
        machine.set(2, 2);
        expect_halted(machine.run(), "-7 divided by 2");
        V2_CHECK_EQ(machine.get(3), static_cast<std::uint64_t>(-3));
        V2_CHECK_EQ(machine.get(4), static_cast<std::uint64_t>(-1));
    }

    // Naming r0 as the destination discards the result but does NOT suppress the trap: every
    // other effect of an instruction naming r0 still happens.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(op::kDivideSigned, reg(1), reg(2), reg(0)).halt();
        machine.load(program);
        machine.set(1, 100);
        machine.set(2, 0);
        const StepResult result = machine.step();
        expect_trap(result, cause::kDivideError, subcode::kDivideByZero, 0, kBase,
                    "divide_signed r1 r2 r0 with a zero divisor");
    }
}

V2_FIXTURE(carry_chain_matches_wide_addition) {
    // A three-limb chain over a single carry register produces the same limbs as the
    // arbitrary-precision sum, and the final carry register holds 1 exactly when the wide sum
    // overflowed.
    struct Case {
        std::uint64_t a[3];
        std::uint64_t b[3];
    };
    const Case cases[] = {
        {{0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 1}, {1, 0, 0}},
        {{0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull}, {1, 0, 0}},
        {{0x0123456789ABCDEFull, 0xFEDCBA9876543210ull, 0}, {0x1111111111111111ull, 1, 0}},
        {{0, 0, 0}, {0, 0, 0}},
    };

    for (const Case& one : cases) {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r(op::kMove, reg(0), reg(11))  // clear the carry register to open the chain
            .op_r_r_r_r(op::kAddCarry, reg(2), reg(5), reg(11), reg(8))
            .op_r_r_r_r(op::kAddCarry, reg(3), reg(6), reg(11), reg(9))
            .op_r_r_r_r(op::kAddCarry, reg(4), reg(7), reg(11), reg(10))
            .halt();
        machine.load(program);
        for (unsigned limb = 0; limb < 3; ++limb) {
            machine.set(2 + limb, one.a[limb]);
            machine.set(5 + limb, one.b[limb]);
        }
        machine.set(11, kSentinel);
        expect_halted(machine.run(), "a three-limb add_carry chain");

        // The reference: schoolbook addition over the three limbs, written independently of the
        // interpreter's own carry computation.
        std::uint64_t carry = 0;
        std::uint64_t expected[3];
        for (unsigned limb = 0; limb < 3; ++limb) {
            const std::uint64_t partial = one.a[limb] + one.b[limb];
            const std::uint64_t total = partial + carry;
            carry = (partial < one.a[limb] || total < partial) ? 1u : 0u;
            expected[limb] = total;
        }
        V2_CHECK_EQ(machine.get(8), expected[0]);
        V2_CHECK_EQ(machine.get(9), expected[1]);
        V2_CHECK_EQ(machine.get(10), expected[2]);
        V2_CHECK_EQ(machine.get(11), carry);
    }

    // Naming r0 as the carry register reads a carry-in of zero and discards the carry-out,
    // which degenerates add_carry into a plain add and subtract_borrow into a plain subtract.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r_r(op::kAddCarry, reg(2), reg(5), reg(0), reg(8))
            .op_r_r_r(op::kAdd, reg(2), reg(5), reg(9))
            .op_r_r_r_r(op::kSubtractBorrow, reg(2), reg(5), reg(0), reg(10))
            .op_r_r_r(op::kSubtract, reg(2), reg(5), reg(12))
            .halt();
        machine.load(program);
        machine.set(2, 0xFFFFFFFFFFFFFFFFull);
        machine.set(5, 0x0000000000000005ull);
        expect_halted(machine.run(), "add_carry and subtract_borrow over r0");
        V2_CHECK_EQ(machine.get(8), machine.get(9));
        V2_CHECK_EQ(machine.get(10), machine.get(12));
        V2_CHECK_EQ(machine.get(0), 0u);
    }

    // The machine writes rd first and rc second, so when rd and rc name the same register the
    // carry-out is the value that survives and the sum is lost.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r_r(op::kAddCarry, reg(2), reg(5), reg(11), reg(11)).halt();
        machine.load(program);
        machine.set(2, 0xFFFFFFFFFFFFFFFFull);
        machine.set(5, 0x0000000000000001ull);
        machine.set(11, 0);
        expect_halted(machine.run(), "add_carry with rd and rc naming one register");
        V2_CHECK_EQ(machine.get(11), 1u);
    }

    // The borrow-out is 1 exactly when the second source plus the incoming borrow exceeds the
    // first, and the carry register is canonical (0 or 1 across all 64 bits) afterward.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r_r(op::kSubtractBorrow, reg(2), reg(5), reg(11), reg(8)).halt();
        machine.load(program);
        machine.set(2, 0);
        machine.set(5, 0);
        machine.set(11, 0xFFFFFFFFFFFFFFFFull);  // only bit 0 is read on the way in
        expect_halted(machine.run(), "subtract_borrow with a borrow in");
        V2_CHECK_EQ(machine.get(8), 0xFFFFFFFFFFFFFFFFull);
        V2_CHECK_EQ(machine.get(11), 1u);
    }
}

V2_FIXTURE(arithmetic_and_logic_word_forms) {
    // The word forms, including the two high-half multiplies, which is the one place the
    // interpreter forms an exact 128-bit product.
    struct Case {
        const char* what;
        std::uint8_t opcode;
        std::uint64_t a;
        std::uint64_t b;
        std::uint64_t expected;
    };

    const Case cases[] = {
        {"add wraps", op::kAdd, 0xFFFFFFFFFFFFFFFFull, 2, 1},
        {"subtract", op::kSubtract, 5, 7, static_cast<std::uint64_t>(-2)},
        {"multiply keeps the low word", op::kMultiply, 0x100000000ull, 0x100000000ull, 0},
        {"multiply_high_unsigned", op::kMultiplyHighUnsigned, 0xFFFFFFFFFFFFFFFFull,
         0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFEull},
        {"multiply_high_signed of -1 by -1", op::kMultiplyHighSigned, 0xFFFFFFFFFFFFFFFFull,
         0xFFFFFFFFFFFFFFFFull, 0},
        {"multiply_high_signed of -1 by 1", op::kMultiplyHighSigned, 0xFFFFFFFFFFFFFFFFull, 1,
         0xFFFFFFFFFFFFFFFFull},
        {"multiply_high_signed of the most negative word by 2", op::kMultiplyHighSigned,
         0x8000000000000000ull, 2, 0xFFFFFFFFFFFFFFFFull},
        {"and", op::kAnd, 0xF0F0F0F0F0F0F0F0ull, 0xFFFF0000FFFF0000ull, 0xF0F00000F0F00000ull},
        {"or", op::kOr, 0xF0F0F0F0F0F0F0F0ull, 0x0F0F0F0F0F0F0F0Full, 0xFFFFFFFFFFFFFFFFull},
        {"xor self-clear", op::kXor, kSentinel, kSentinel, 0},
    };

    for (const Case& one : cases) {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(one.opcode, reg(1), reg(2), reg(3)).halt();
        machine.load(program);
        machine.set(1, one.a);
        machine.set(2, one.b);
        expect_halted(machine.run(), one.what);
        check_equal_u64(machine.get(3), one.expected, one.what, __FILE__, __LINE__);
    }

    // The unary forms, and negate of the most negative word, which yields itself back and
    // raises nothing.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r(op::kNot, reg(1), reg(3))
            .op_r_r(op::kNegate, reg(2), reg(4))
            .op_r_r(op::kByteReverse, reg(1), reg(5))
            .halt();
        machine.load(program);
        machine.set(1, 0x0011223344556677ull);
        machine.set(2, 0x8000000000000000ull);
        expect_halted(machine.run(), "not, negate and byte_reverse");
        V2_CHECK_EQ(machine.get(3), 0xFFEEDDCCBBAA9988ull);
        V2_CHECK_EQ(machine.get(4), 0x8000000000000000ull);
        V2_CHECK_EQ(machine.get(5), 0x7766554433221100ull);
    }

    // An immediate is a 32-bit literal sign-extended to 64 bits before the operation, and that
    // includes the bitwise operations, so a mask whose high bits are all ones reaches the whole
    // word.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_i4(op::kAndImm, reg(1), reg(3), 0xFFFFFFF0ull)
            .op_r_r_i4(op::kAddImm, reg(1), reg(4), 0x00001000ull)
            .op_r_r_i4(op::kSubtractImm, reg(1), reg(5), 0xFFFFFFFFull)
            .halt();
        machine.load(program);
        machine.set(1, 0xFFFFFFFFFFFFFFFFull);
        expect_halted(machine.run(), "immediate arithmetic and logic");
        V2_CHECK_EQ(machine.get(3), 0xFFFFFFFFFFFFFFF0ull);
        V2_CHECK_EQ(machine.get(4), 0x0000000000000FFFull);
        V2_CHECK_EQ(machine.get(5), 0u);
    }
}

V2_FIXTURE(compares_write_one_or_zero) {
    // The pin table first. Nine hand-written operand pairs with ten hand-written answers each,
    // pinned to appendix A.5 rather than computed by anything the interpreter uses, so a
    // permuted predicate mapping fails here instead of agreeing with itself. Every predicate
    // has a distinct signature across the nine pairs, which is what makes ANY permutation
    // visible rather than only the convenient ones.
    for (unsigned predicate = 0; predicate < kPredicateCount; ++predicate) {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(static_cast<std::uint8_t>(op::kCompareEq + predicate), reg(1), reg(2),
                         reg(3));
        machine.load(program);
        for (unsigned index = 0; index < kPredicatePinCount; ++index) {
            const PredicatePin& pin = kPredicatePins[index];
            machine.set(1, pin.left);
            machine.set(2, pin.right);
            machine.set(3, kSentinel);
            machine.interpreter().set_pc(kBase);
            const StepResult result = machine.step();
            V2_CHECK(result.status == StepStatus::Advanced);
            char buffer[192];
            std::snprintf(buffer, sizeof(buffer), "compare_%s on pin %u",
                          kPredicateNames[predicate], index);
            check_equal_u64(machine.get(3), pinned_answer(pin, predicate) ? 1u : 0u, buffer,
                            __FILE__, __LINE__);
        }
    }

    // The same pins through the IMMEDIATE compare form, which is a separate opcode band and a
    // separate dispatch site, on the pairs whose right operand a sign-extended 32-bit literal
    // can present. The pairs turning on the most negative word are unreachable that way and are
    // marked so in the table rather than quietly skipped.
    for (unsigned predicate = 0; predicate < kPredicateCount; ++predicate) {
        for (unsigned index = 0; index < kPredicatePinCount; ++index) {
            const PredicatePin& pin = kPredicatePins[index];
            if (!pin.reachable_by_immediate) {
                continue;
            }
            Machine machine;
            Encoder program(kBase);
            program.op_r_r_i4(static_cast<std::uint8_t>(op::kCompareImmBase + predicate), reg(1),
                              reg(3), pin.immediate);
            machine.load(program);
            machine.set(1, pin.left);
            machine.set(3, kSentinel);
            const StepResult result = machine.step();
            V2_CHECK(result.status == StepStatus::Advanced);
            char buffer[192];
            std::snprintf(buffer, sizeof(buffer), "compare_%s immediate form on pin %u",
                          kPredicateNames[predicate], index);
            check_equal_u64(machine.get(3), pinned_answer(pin, predicate) ? 1u : 0u, buffer,
                            __FILE__, __LINE__);
        }
    }

    // Then the exhaustive sweep, for the separate claim that every compare writes exactly 1 or
    // exactly 0 across all 64 bits for every pair. Its expectations come from the second
    // transcription in predicate_oracle.h, never from the interpreter's own function.
    const std::uint64_t values[] = {
        0, 1, 2, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull,
        0x00000000FFFFFFFFull,
    };

    for (unsigned predicate = 0; predicate < kPredicateCount; ++predicate) {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(static_cast<std::uint8_t>(op::kCompareEq + predicate), reg(1), reg(2),
                         reg(3));
        machine.load(program);
        for (std::uint64_t left : values) {
            for (std::uint64_t right : values) {
                machine.set(1, left);
                machine.set(2, right);
                machine.set(3, kSentinel);
                machine.interpreter().set_pc(kBase);
                const StepResult result = machine.step();
                V2_CHECK(result.status == StepStatus::Advanced);
                const std::uint64_t expected =
                    oracle_predicate(predicate, left, right) ? 1u : 0u;
                const std::uint64_t actual = machine.get(3);
                if (actual != expected) {
                    char buffer[192];
                    std::snprintf(buffer, sizeof(buffer), "compare_%s",
                                  kPredicateNames[predicate]);
                    check_equal_u64(actual, expected, buffer, __FILE__, __LINE__);
                }
                V2_CHECK(actual == 0u || actual == 1u);
            }
        }
    }

    // An unsigned comparison against a literal sign-extends the literal first and then reads it
    // as unsigned, which is what gives a 32-bit literal access to the top of the unsigned
    // range: compare_lt_unsigned rs $-1 rd sets rd for every value except that one.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_i4(static_cast<std::uint8_t>(op::kCompareImmBase + 6), reg(1), reg(3),
                          0xFFFFFFFFull);
        machine.load(program);
        for (std::uint64_t value : values) {
            machine.set(1, value);
            machine.set(3, kSentinel);
            machine.interpreter().set_pc(kBase);
            const StepResult result = machine.step();
            V2_CHECK(result.status == StepStatus::Advanced);
            V2_CHECK_EQ(machine.get(3), value == 0xFFFFFFFFFFFFFFFFull ? 0u : 1u);
        }
    }

    // compare_eq rs r0 rd is the idiomatic test against zero, with no opcode spent on it.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r_r(op::kCompareEq, reg(4), reg(0), reg(5)).halt();
        machine.load(program);
        machine.set(4, 0);
        expect_halted(machine.run(), "compare_eq r4 r0 r5 with r4 zero");
        V2_CHECK_EQ(machine.get(5), 1u);
    }
}

}  // namespace maize::v2::test
