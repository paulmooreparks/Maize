// fixtures_decode.cpp (maize-418): the decode layer.
//
// Length, the reserved and escape bytes, the operand-byte form field, and the boundary between
// an opcode this build executes and one it only decodes.

#include <cstdio>

#include "fixture_support.h"

namespace maize::v2::test {
namespace {

constexpr std::uint64_t kBase = 0x100;

// Appendix A transcribed a SECOND time, independently of the production table in
// opcode_v2.h and in a different shape: contiguous byte ranges with the total instruction
// length, or -1 for a byte that raises the illegal-instruction trap instead of having one.
// Two transcriptions from the same appendix that agree byte for byte is the check; a test that
// re-derived the expected value from the table under test would check nothing.
struct LengthRange {
    std::uint8_t first;
    std::uint8_t last;
    int length;
};

constexpr LengthRange kAppendixA[] = {
    {0x00, 0x00, -1},  // A.2 the zero-byte guard
    {0x01, 0x01, 3},   // move, op r r
    {0x02, 0x02, 10},  // move.w, op r i8, the longest instruction in the base
    {0x03, 0x04, 3},   // move.zb, move.sb, op r i1
    {0x05, 0x06, 4},   // move.zq, move.sq, op r i2
    {0x07, 0x09, 6},   // move.zh, move.sh, pc_add, op r i4
    {0x0A, 0x0F, -1},
    {0x10, 0x28, 4},   // three-operand register arithmetic and logic, op r r r
    {0x29, 0x2E, 3},   // the unary forms, op r r
    {0x2F, 0x30, 5},   // add_carry, subtract_borrow, op r r r r
    {0x31, 0x37, 7},   // the 32-bit-immediate forms, op r r i4
    {0x38, 0x3D, 4},   // the shift-count immediate forms, op r r i1
    {0x3E, 0x3F, -1},
    {0x40, 0x49, 4},   // the ten register compares, op r r r
    {0x4A, 0x53, 7},   // the same ten as immediate compares, op r r i4
    {0x54, 0x5F, -1},
    {0x60, 0x69, 7},   // the ten branches, op r r i4
    {0x6A, 0x6F, -1},
    {0x70, 0x70, 5},   // jump target, op i4
    {0x71, 0x71, 2},   // jump rs, op r
    {0x72, 0x72, 5},   // call target, op i4
    {0x73, 0x73, 2},   // call rs, op r
    {0x74, 0x74, 1},   // return, op
    {0x75, 0x76, 4},   // select_nz, select_z, op r r r
    {0x77, 0x7F, -1},
    {0x80, 0x86, 3},   // the seven bare loads, op r r
    {0x87, 0x8D, 5},   // the same seven displaced, op r r i2
    {0x8E, 0x91, 3},   // the four bare stores, op r r
    {0x92, 0x95, 5},   // the same four displaced, op r r i2
    {0x96, 0x9F, -1},
    {0xA0, 0xA8, 3},   // the six extracts and three inserts, op r r
    {0xA9, 0xAB, 5},   // the three bitfield instructions, op r r i1 i1
    {0xAC, 0xAF, -1},
    {0xB0, 0xB2, 4},   // block_copy, block_copy_forward, block_set, op r r r
    {0xB3, 0xB7, -1},
    {0xB8, 0xB9, 4},   // csr_read, csr_write, op r i2
    {0xBA, 0xBB, 2},   // sys #imm (op i1), sys rs (op r)
    {0xBC, 0xC0, 1},   // trap_return, halt, wait_for_interrupt, nop, tlb_invalidate_all
    {0xC1, 0xC1, 2},   // tlb_invalidate_address, op r
    {0xC2, 0xC3, 3},   // port_in, port_out, op r r
    {0xC4, 0xC4, 5},   // csr_swap, op r r i2
    {0xC5, 0xC7, -1},
    {0xC8, 0xCF, 4},   // float add, subtract, multiply, divide, both formats, op r r r
    {0xD0, 0xD5, 3},   // float square_root, negate, absolute, op r r
    {0xD6, 0xD9, 5},   // float multiply_add, multiply_subtract, op r r r r
    {0xDA, 0xE9, 4},   // float minimum, maximum, the six compares, op r r r
    {0xEA, 0xF3, 3},   // float narrow, widen, the eight conversions, op r r
    {0xF4, 0xF7, -1},
    {0xF8, 0xFE, -1},  // A.13 the seven escape bytes, no page allocated
    {0xFF, 0xFF, 1},   // A.14 breakpoint, assigned rather than reserved
};

int expected_length(std::uint8_t byte) {
    for (const LengthRange& range : kAppendixA) {
        if (byte >= range.first && byte <= range.last) {
            return range.length;
        }
    }
    return -2;  // a byte no range covers, which is itself a transcription defect
}

}  // namespace

V2_FIXTURE(decode_length_table_matches_appendix_a) {
    for (int i = 0; i < 256; ++i) {
        const std::uint8_t byte = static_cast<std::uint8_t>(i);
        const int expected = expected_length(byte);
        const int actual = instruction_length(byte);
        if (expected != actual) {
            char buffer[192];
            std::snprintf(buffer, sizeof(buffer),
                          "opcode $%02X has length %d in the table, appendix A says %d", byte,
                          actual, expected);
            record_failure(buffer);
        }
    }

    // Appendix A.1 states the totals outright, so count them rather than trusting the ranges.
    int assigned = 0;
    int escapes = 0;
    int reserved = 0;
    for (int i = 0; i < 256; ++i) {
        switch (kOpcodeTable[static_cast<std::size_t>(i)].kind) {
            case OpcodeKind::Assigned: ++assigned; break;
            case OpcodeKind::Escape: ++escapes; break;
            case OpcodeKind::Reserved: ++reserved; break;
        }
    }
    V2_CHECK_EQ(static_cast<std::uint64_t>(assigned), 187u);
    V2_CHECK_EQ(static_cast<std::uint64_t>(escapes), 7u);
    V2_CHECK_EQ(static_cast<std::uint64_t>(reserved), 62u);
}

V2_FIXTURE(decode_reserved_and_escape_bytes_trap) {
    // Every byte with no length, reserved or escape alike, raises the illegal-instruction trap
    // reporting itself as the offending byte and its OWN address as the faulting address.
    for (int i = 0; i < 256; ++i) {
        const std::uint8_t byte = static_cast<std::uint8_t>(i);
        if (expected_length(byte) != -1) {
            continue;
        }
        Machine machine;
        Encoder program(kBase);
        // A byte after the offending one, so a decoder that wrongly consumed a following byte
        // would still find something there and the fixture would still catch the length claim.
        program.raw_byte(byte).raw_byte(0x42);
        machine.load(program);
        const StepResult result = machine.step();
        char label[64];
        std::snprintf(label, sizeof(label), "reserved or escape byte $%02X", byte);
        expect_trap(result, cause::kIllegalInstruction, 0, byte, kBase, label);
        // The program counter does not move on a fault: the instruction is restartable.
        V2_CHECK_EQ(machine.interpreter().pc(), kBase);
    }
}

V2_FIXTURE(decode_plain_slot_rejects_nonzero_form) {
    // A plain slot demands a form field of %000. Each case puts %001 on the FIRST plain operand
    // byte of an instruction of a different length class, and expects the illegal-operand trap
    // carrying that whole operand byte.
    const std::uint8_t bad_operand = static_cast<std::uint8_t>((1u << 5) | 3u);  // form %001, r3

    struct Case {
        const char* what;
        std::vector<std::uint8_t> bytes;
    };

    const std::vector<Case> cases = {
        {"op r (jump rs)", {op::kJumpReg, bad_operand}},
        {"op r r (move)", {op::kMove, bad_operand, 0x04}},
        {"op r r r (add)", {op::kAdd, bad_operand, 0x04, 0x05}},
        {"op r r r r (add_carry)", {op::kAddCarry, bad_operand, 0x04, 0x05, 0x06}},
        {"op r i1 (move.zb)", {op::kMoveZb, bad_operand, 0x11}},
        {"op r i2 (move.zq)", {op::kMoveZq, bad_operand, 0x11, 0x22}},
        {"op r i4 (move.zh)", {op::kMoveZh, bad_operand, 0x11, 0x22, 0x33, 0x44}},
        {"op r i8 (move.w)",
         {op::kMoveW, bad_operand, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}},
        {"op r r i1 (shift_left immediate)", {op::kShiftLeftImm, bad_operand, 0x04, 0x03}},
        {"op r r i2 (displaced load)", {op::kLoadDisp, bad_operand, 0x04, 0x00, 0x00}},
        {"op r r i4 (add immediate)",
         {op::kAddImm, bad_operand, 0x04, 0x00, 0x10, 0x00, 0x00}},
        {"op r r i1 i1 (bitfield_extract)",
         {op::kBitfieldExtract, bad_operand, 0x04, 0x0C, 0x05}},
    };

    for (const Case& one : cases) {
        Machine machine;
        Encoder program(kBase);
        for (std::uint8_t byte : one.bytes) {
            program.raw_byte(byte);
        }
        machine.load(program);
        const StepResult result = machine.step();
        expect_trap(result, cause::kIllegalOperand, subcode::kOperandForm, bad_operand, kBase,
                    one.what);
    }
}

V2_FIXTURE(decode_sliced_slot_form_range) {
    // A byte-sliced slot admits all eight forms; a quarter-sliced slot rejects %100 and above;
    // a half-sliced slot rejects %010 and above. The rejection is the same illegal-operand trap
    // a malformed plain slot raises.
    struct Case {
        const char* what;
        std::uint8_t opcode;
        std::uint8_t form;
        bool legal;
    };

    const Case cases[] = {
        {"extract.zb form %111", op::kExtractZb, 7, true},
        {"extract.zq form %011", op::kExtractZq, 3, true},
        {"extract.zq form %100", op::kExtractZq, 4, false},
        {"extract.sq form %111", op::kExtractSq, 7, false},
        {"extract.zh form %001", op::kExtractZh, 1, true},
        {"extract.zh form %010", op::kExtractZh, 2, false},
        {"extract.sh form %011", op::kExtractSh, 3, false},
    };

    for (const Case& one : cases) {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r(one.opcode, slice(3, one.form), reg(4)).halt();
        machine.load(program);
        const StepResult result = machine.step();
        if (one.legal) {
            V2_CHECK(result.status == StepStatus::Advanced);
        } else {
            const std::uint8_t operand_byte = static_cast<std::uint8_t>((one.form << 5) | 3u);
            expect_trap(result, cause::kIllegalOperand, subcode::kOperandForm, operand_byte,
                        kBase, one.what);
        }
    }

    // The sliced slot of an insert is the SECOND operand byte, so the same rule has to be
    // checked from that position too.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r(op::kInsertQ, reg(3), slice(4, 4)).halt();
        machine.load(program);
        const StepResult result = machine.step();
        expect_trap(result, cause::kIllegalOperand, subcode::kOperandForm,
                    static_cast<std::uint8_t>((4u << 5) | 4u), kBase,
                    "insert.q form %100 in the second slot");
    }
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r(op::kInsertH, reg(3), slice(4, 2)).halt();
        machine.load(program);
        const StepResult result = machine.step();
        expect_trap(result, cause::kIllegalOperand, subcode::kOperandForm,
                    static_cast<std::uint8_t>((2u << 5) | 4u), kBase,
                    "insert.h form %010 in the second slot");
    }
}

V2_FIXTURE(out_of_scope_opcodes_are_a_host_diagnostic) {
    // These are real assigned opcodes whose families this build does not implement. Reaching
    // one is a scaffold gap the host reports, never a guest-visible trap record, because
    // inventing a trap cause for "not implemented yet" would misrepresent the gap as
    // conformant illegal-instruction behaviour.
    struct Case {
        const char* what;
        std::uint8_t opcode;
        std::vector<std::uint8_t> bytes;
    };

    const Case cases[] = {
        {"nop", op::kNop, {op::kNop}},
        // port_in and port_out USED to sit here and no longer do: maize-451 implements both, and
        // the case they vacated is filled by another member of the same band rather than
        // dropped, so the band's coverage does not thin out as it is implemented. csr_read and
        // csr_swap left the same way on maize-463, and sys, trap_return and breakpoint left the
        // same way on maize-464, where all three became real behaviour that fixtures_traps.cpp
        // now pins. Each departure is replaced from the floating-point band, which is the one
        // band with members to spare until maize-419.
        //
        // The two survivors below are privileged instructions whose bodies are still another
        // card's: they reach this diagnostic at SUPERVISOR level, which is where the fixture
        // runs them, and raise the privileged-operation fault at user level instead, which is
        // where privileged_instructions_are_privileged_at_user_level runs them.
        {"wait_for_interrupt", op::kWaitForInterrupt, {op::kWaitForInterrupt}},
        {"tlb_invalidate_address", op::kTlbInvalidateAddress, {op::kTlbInvalidateAddress, 0x04}},
        {"tlb_invalidate_all", op::kTlbInvalidateAll, {op::kTlbInvalidateAll}},
        {"float_add", 0xC8, {0xC8, 0x01, 0x02, 0x03}},
        {"float_add.h", 0xC9, {0xC9, 0x01, 0x02, 0x03}},
        {"float_subtract", 0xCA, {0xCA, 0x01, 0x02, 0x03}},
        {"float_square_root", 0xD0, {0xD0, 0x01, 0x02}},
        {"unsigned_to_float.h", 0xF3, {0xF3, 0x01, 0x02}},
    };

    for (const Case& one : cases) {
        Machine machine;
        Encoder program(kBase);
        for (std::uint8_t byte : one.bytes) {
            program.raw_byte(byte);
        }
        machine.load(program);
        const StepResult result = machine.step();
        expect_unimplemented(result, one.opcode, one.what);
        V2_CHECK(result.status != StepStatus::Trapped);
    }

    // The two guard bytes of A.14 reach "does not execute" by two different routes, and the
    // routes are not interchangeable: $00 is reserved and stops at decode with a trap, $FF is
    // assigned and reaches the execute stage where this build has nothing for it.
    {
        Machine machine;
        Encoder program(kBase);
        program.raw_byte(0x00);
        machine.load(program);
        const StepResult result = machine.step();
        expect_trap(result, cause::kIllegalInstruction, 0, 0x00, kBase, "the zero-byte guard");
    }
}

V2_FIXTURE(encoding_worked_examples) {
    // Every worked example in instruction-encoding.md, byte for byte. These are the encodings a
    // reader of the specification will have checked by hand, so they are the right anchor for a
    // fixture suite that has no assembler behind it yet.
    struct Example {
        const char* assembly;
        std::vector<std::uint8_t> expected;
        void (*emit)(Encoder&);
    };

    const Example examples[] = {
        {"add r1 r2 r3",
         {0x10, 0x01, 0x02, 0x03},
         [](Encoder& e) { e.op_r_r_r(op::kAdd, reg(1), reg(2), reg(3)); }},
        {"add r4 $1000 r4",
         {0x31, 0x04, 0x04, 0x00, 0x10, 0x00, 0x00},
         [](Encoder& e) { e.op_r_r_i4(op::kAddImm, reg(4), reg(4), 0x1000); }},
        {"load @r30+$20 r5",
         {0x87, 0x1E, 0x05, 0x20, 0x00},
         [](Encoder& e) { e.op_r_r_i2(op::kLoadDisp, reg(30), reg(5), 0x20); }},
        {"load.zb @r9 r4",
         {0x81, 0x09, 0x04},
         [](Encoder& e) { e.op_r_r(op::kLoadZb, reg(9), reg(4)); }},
        {"extract.zb r3.b5 r7",
         {0xA0, 0xA3, 0x07},
         [](Encoder& e) { e.op_r_r(op::kExtractZb, slice(3, 5), reg(7)); }},
        {"bitfield_extract r5 #12 #5 r6",
         {0xA9, 0x05, 0x06, 0x0C, 0x05},
         [](Encoder& e) { e.op_r_r_i1_i1(op::kBitfieldExtract, reg(5), reg(6), 12, 5); }},
        {"branch_lt_signed r4 r5 (displacement -24)",
         {0x62, 0x04, 0x05, 0xE8, 0xFF, 0xFF, 0xFF},
         [](Encoder& e) {
             e.op_r_r_i4(static_cast<std::uint8_t>(op::kBranchBase + 2), reg(4), reg(5),
                         0xFFFFFFE8ull);
         }},
        {"move.w $1122334455667788 r10",
         {0x02, 0x0A, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11},
         [](Encoder& e) { e.op_r_i8(op::kMoveW, reg(10), 0x1122334455667788ull); }},
        {"return", {0x74}, [](Encoder& e) { e.op(op::kReturn); }},
    };

    for (const Example& one : examples) {
        Encoder program(kBase);
        one.emit(program);
        if (program.bytes() != one.expected) {
            record_failure(std::string("encoding of ") + one.assembly + " does not match the "
                           "worked example in instruction-encoding.md");
        }
        // Length is a decode-layer fact, so the table has to agree with the bytes the example
        // spells out.
        V2_CHECK_EQ(static_cast<std::uint64_t>(program.bytes().size()),
                    static_cast<std::uint64_t>(instruction_length(one.expected[0])));
    }
}

V2_FIXTURE(decode_next_instruction_address_precedes_operands) {
    // The address of the following instruction is fixed from the length table before any
    // operand byte is read, so an instruction whose operand bytes run off the end of populated
    // memory faults on the FETCH of the missing byte and still reports the opcode byte's
    // address as the faulting instruction address.
    Machine machine(0x104);
    Encoder program(0x100);
    program.raw_byte(op::kMoveW).raw_byte(0x04).raw_byte(0x11).raw_byte(0x22);
    machine.load(program);
    const StepResult result = machine.step();
    // move.w is ten bytes; memory ends at $104, so the fifth immediate byte is the first one
    // that is not there.
    expect_trap(result, cause::kPhysicalMemoryFault, 0, 0x104, 0x100,
                "a move.w whose immediate runs past populated memory");
    V2_CHECK_EQ(machine.get(4), 0u);
}

}  // namespace maize::v2::test
