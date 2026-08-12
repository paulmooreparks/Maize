// mnemonic_v2.h (maize-422): the mnemonic-to-opcode association for the v2 assembler.
//
// Decision D-4 on maize-422 fixes what this table may carry and what it may not. One row per
// assigned opcode byte, and each row holds exactly three things: the opcode byte constant from
// opcode_v2.h's op:: namespace, the canonical mnemonic text spelled exactly as
// appendix-a-opcode-map.md spells it, and an operand-selection tag distinguishing siblings that
// share a mnemonic. Shape, slot classes and instruction length are NEVER re-declared here;
// every consumer reads those back from opcode_v2.h's kOpcodeTable, indexing on the byte the row
// already carries. Two tables that each declare a length are two tables that can disagree.
//
// The table is hand-written rather than generated (D-12). The correctness argument does not
// rest on that choice: the conformance test for AC-14 parses appendix-a-opcode-map.md itself,
// which is the frozen normative assignment and is independent of everything this project
// transcribed, and checks every row's byte, text and select against it. A generator would only
// have caught a hand-edit diverging from its own prior output, which is a narrower problem, and
// it would have cost a second copy of the appendix-parsing and operand-classification logic
// that the checker needs anyway.

#ifndef MAIZE_V2_MNEMONIC_V2_H
#define MAIZE_V2_MNEMONIC_V2_H

#include <array>
#include <cstdint>

#include "opcode_v2.h"

namespace maize::v2 {

// Which of a mnemonic's several encodings a source statement selected. The assembler's parser
// determines the bucket from what it parsed (a register operand or a literal one for the ALU
// and compare pairs, a bare `@rN` or a displaced `@rN+disp` for the loads and stores, a
// register or a target for jump, call and sys), and looks up the one row matching both the
// mnemonic text and the bucket. This is the whole of the sanctioned mnemonic selection
// assembler.md's pseudo-instruction policy allows.
enum class Select : std::uint8_t {
    None,        // the mnemonic has exactly one opcode, so nothing is selected
    RegForm,     // register-operand sibling of an immediate pair, e.g. add rs1 rs2 rd
    ImmForm,     // immediate-operand sibling, e.g. add rs $imm rd
    RegTarget,   // jump, call and sys register form
    DispTarget,  // jump, call and sys displacement or immediate form
    Bare,        // load and store bare-address form, @rb
    Displaced,   // load and store displaced-address form, @rb+$disp
};

struct MnemonicEntry {
    std::uint8_t opcode;  // op::kXxx from opcode_v2.h
    const char* text;     // exactly as appendix-a-opcode-map.md spells it, e.g. "add.h"
    Select select;
};

// One row per Assigned byte of the primary page: 186 tabled in A.3 through A.12, plus
// breakpoint, which A.14 states in prose rather than in a table row.
inline constexpr std::array<MnemonicEntry, 187> kMnemonics = {{
    // A.3 Constants and moves, $01..$0F. Every immediate move names its width in the mnemonic,
    // so each is its own row and none of them is selected against another.
    {op::kMove, "move", Select::None},
    {op::kMoveW, "move.w", Select::None},
    {op::kMoveZb, "move.zb", Select::None},
    {op::kMoveSb, "move.sb", Select::None},
    {op::kMoveZq, "move.zq", Select::None},
    {op::kMoveSq, "move.sq", Select::None},
    {op::kMoveZh, "move.zh", Select::None},
    {op::kMoveSh, "move.sh", Select::None},
    {op::kPcAdd, "pc_add", Select::None},

    // A.4 Integer arithmetic and logic, $10..$3F. The thirteen mnemonics with both a register
    // and an immediate form appear twice, once per form; the rest appear once.
    {op::kAdd, "add", Select::RegForm},
    {op::kAddH, "add.h", Select::RegForm},
    {op::kSubtract, "subtract", Select::RegForm},
    {op::kSubtractH, "subtract.h", Select::RegForm},
    {op::kMultiply, "multiply", Select::None},
    {op::kMultiplyH, "multiply.h", Select::None},
    {op::kMultiplyHighSigned, "multiply_high_signed", Select::None},
    {op::kMultiplyHighUnsigned, "multiply_high_unsigned", Select::None},
    {op::kDivideSigned, "divide_signed", Select::None},
    {op::kDivideSignedH, "divide_signed.h", Select::None},
    {op::kDivideUnsigned, "divide_unsigned", Select::None},
    {op::kDivideUnsignedH, "divide_unsigned.h", Select::None},
    {op::kRemainderSigned, "remainder_signed", Select::None},
    {op::kRemainderSignedH, "remainder_signed.h", Select::None},
    {op::kRemainderUnsigned, "remainder_unsigned", Select::None},
    {op::kRemainderUnsignedH, "remainder_unsigned.h", Select::None},
    {op::kAnd, "and", Select::RegForm},
    {op::kOr, "or", Select::RegForm},
    {op::kXor, "xor", Select::RegForm},
    {op::kShiftLeft, "shift_left", Select::RegForm},
    {op::kShiftLeftH, "shift_left.h", Select::RegForm},
    {op::kShiftRightLogical, "shift_right_logical", Select::RegForm},
    {op::kShiftRightLogicalH, "shift_right_logical.h", Select::RegForm},
    {op::kShiftRightArithmetic, "shift_right_arithmetic", Select::RegForm},
    {op::kShiftRightArithmeticH, "shift_right_arithmetic.h", Select::RegForm},
    {op::kNot, "not", Select::None},
    {op::kNotH, "not.h", Select::None},
    {op::kNegate, "negate", Select::None},
    {op::kNegateH, "negate.h", Select::None},
    {op::kByteReverse, "byte_reverse", Select::None},
    {op::kByteReverseH, "byte_reverse.h", Select::None},
    {op::kAddCarry, "add_carry", Select::None},
    {op::kSubtractBorrow, "subtract_borrow", Select::None},
    {op::kAddImm, "add", Select::ImmForm},
    {op::kAddHImm, "add.h", Select::ImmForm},
    {op::kSubtractImm, "subtract", Select::ImmForm},
    {op::kSubtractHImm, "subtract.h", Select::ImmForm},
    {op::kAndImm, "and", Select::ImmForm},
    {op::kOrImm, "or", Select::ImmForm},
    {op::kXorImm, "xor", Select::ImmForm},
    {op::kShiftLeftImm, "shift_left", Select::ImmForm},
    {op::kShiftLeftHImm, "shift_left.h", Select::ImmForm},
    {op::kShiftRightLogicalImm, "shift_right_logical", Select::ImmForm},
    {op::kShiftRightLogicalHImm, "shift_right_logical.h", Select::ImmForm},
    {op::kShiftRightArithmeticImm, "shift_right_arithmetic", Select::ImmForm},
    {op::kShiftRightArithmeticHImm, "shift_right_arithmetic.h", Select::ImmForm},

    // A.5 Compares, $40..$5F. Ten predicates in the register form, then the same ten in the
    // immediate form and in the identical order.
    {op::kCompareEq, "compare_eq", Select::RegForm},
    {op::kCompareNe, "compare_ne", Select::RegForm},
    {op::kCompareLtSigned, "compare_lt_signed", Select::RegForm},
    {op::kCompareLeSigned, "compare_le_signed", Select::RegForm},
    {op::kCompareGtSigned, "compare_gt_signed", Select::RegForm},
    {op::kCompareGeSigned, "compare_ge_signed", Select::RegForm},
    {op::kCompareLtUnsigned, "compare_lt_unsigned", Select::RegForm},
    {op::kCompareLeUnsigned, "compare_le_unsigned", Select::RegForm},
    {op::kCompareGtUnsigned, "compare_gt_unsigned", Select::RegForm},
    {op::kCompareGeUnsigned, "compare_ge_unsigned", Select::RegForm},
    {static_cast<std::uint8_t>(op::kCompareImmBase + 0), "compare_eq", Select::ImmForm},
    {static_cast<std::uint8_t>(op::kCompareImmBase + 1), "compare_ne", Select::ImmForm},
    {static_cast<std::uint8_t>(op::kCompareImmBase + 2), "compare_lt_signed", Select::ImmForm},
    {static_cast<std::uint8_t>(op::kCompareImmBase + 3), "compare_le_signed", Select::ImmForm},
    {static_cast<std::uint8_t>(op::kCompareImmBase + 4), "compare_gt_signed", Select::ImmForm},
    {static_cast<std::uint8_t>(op::kCompareImmBase + 5), "compare_ge_signed", Select::ImmForm},
    {static_cast<std::uint8_t>(op::kCompareImmBase + 6), "compare_lt_unsigned", Select::ImmForm},
    {static_cast<std::uint8_t>(op::kCompareImmBase + 7), "compare_le_unsigned", Select::ImmForm},
    {static_cast<std::uint8_t>(op::kCompareImmBase + 8), "compare_gt_unsigned", Select::ImmForm},
    {static_cast<std::uint8_t>(op::kCompareImmBase + 9), "compare_ge_unsigned", Select::ImmForm},

    // A.6 Branches, $60..$6F, in the compare predicate order. Each has one form only.
    {static_cast<std::uint8_t>(op::kBranchBase + 0), "branch_eq", Select::None},
    {static_cast<std::uint8_t>(op::kBranchBase + 1), "branch_ne", Select::None},
    {static_cast<std::uint8_t>(op::kBranchBase + 2), "branch_lt_signed", Select::None},
    {static_cast<std::uint8_t>(op::kBranchBase + 3), "branch_le_signed", Select::None},
    {static_cast<std::uint8_t>(op::kBranchBase + 4), "branch_gt_signed", Select::None},
    {static_cast<std::uint8_t>(op::kBranchBase + 5), "branch_ge_signed", Select::None},
    {static_cast<std::uint8_t>(op::kBranchBase + 6), "branch_lt_unsigned", Select::None},
    {static_cast<std::uint8_t>(op::kBranchBase + 7), "branch_le_unsigned", Select::None},
    {static_cast<std::uint8_t>(op::kBranchBase + 8), "branch_gt_unsigned", Select::None},
    {static_cast<std::uint8_t>(op::kBranchBase + 9), "branch_ge_unsigned", Select::None},

    // A.7 Control transfer and select, $70..$7F.
    {op::kJumpDisp, "jump", Select::DispTarget},
    {op::kJumpReg, "jump", Select::RegTarget},
    {op::kCallDisp, "call", Select::DispTarget},
    {op::kCallReg, "call", Select::RegTarget},
    {op::kReturn, "return", Select::None},
    {op::kSelectNz, "select_nz", Select::None},
    {op::kSelectZ, "select_z", Select::None},

    // A.8 Loads and stores, $80..$9F. Seven bare loads, the same seven displaced, four bare
    // stores, the same four displaced.
    {op::kLoad, "load", Select::Bare},
    {op::kLoadZb, "load.zb", Select::Bare},
    {op::kLoadSb, "load.sb", Select::Bare},
    {op::kLoadZq, "load.zq", Select::Bare},
    {op::kLoadSq, "load.sq", Select::Bare},
    {op::kLoadZh, "load.zh", Select::Bare},
    {op::kLoadSh, "load.sh", Select::Bare},
    {static_cast<std::uint8_t>(op::kLoadDisp + 0), "load", Select::Displaced},
    {static_cast<std::uint8_t>(op::kLoadDisp + 1), "load.zb", Select::Displaced},
    {static_cast<std::uint8_t>(op::kLoadDisp + 2), "load.sb", Select::Displaced},
    {static_cast<std::uint8_t>(op::kLoadDisp + 3), "load.zq", Select::Displaced},
    {static_cast<std::uint8_t>(op::kLoadDisp + 4), "load.sq", Select::Displaced},
    {static_cast<std::uint8_t>(op::kLoadDisp + 5), "load.zh", Select::Displaced},
    {static_cast<std::uint8_t>(op::kLoadDisp + 6), "load.sh", Select::Displaced},
    {op::kStore, "store", Select::Bare},
    {op::kStoreB, "store.b", Select::Bare},
    {op::kStoreQ, "store.q", Select::Bare},
    {op::kStoreH, "store.h", Select::Bare},
    {static_cast<std::uint8_t>(op::kStoreDisp + 0), "store", Select::Displaced},
    {static_cast<std::uint8_t>(op::kStoreDisp + 1), "store.b", Select::Displaced},
    {static_cast<std::uint8_t>(op::kStoreDisp + 2), "store.q", Select::Displaced},
    {static_cast<std::uint8_t>(op::kStoreDisp + 3), "store.h", Select::Displaced},

    // A.9 Extract and insert, $A0..$AF. The whole of the base's sliced-slot surface.
    {op::kExtractZb, "extract.zb", Select::None},
    {op::kExtractSb, "extract.sb", Select::None},
    {op::kExtractZq, "extract.zq", Select::None},
    {op::kExtractSq, "extract.sq", Select::None},
    {op::kExtractZh, "extract.zh", Select::None},
    {op::kExtractSh, "extract.sh", Select::None},
    {op::kInsertB, "insert.b", Select::None},
    {op::kInsertQ, "insert.q", Select::None},
    {op::kInsertH, "insert.h", Select::None},
    {op::kBitfieldExtract, "bitfield_extract", Select::None},
    {op::kBitfieldExtractSigned, "bitfield_extract_signed", Select::None},
    {op::kBitfieldInsert, "bitfield_insert", Select::None},

    // A.10 Block memory, $B0..$B7.
    {op::kBlockCopy, "block_copy", Select::None},
    {op::kBlockCopyForward, "block_copy_forward", Select::None},
    {op::kBlockSet, "block_set", Select::None},

    // A.11 System, control registers, TLB, and ports, $B8..$C7. sys divides the way jump and
    // call do, an immediate form and a register form under one name.
    {op::kCsrRead, "csr_read", Select::None},
    {op::kCsrWrite, "csr_write", Select::None},
    {op::kSysImm, "sys", Select::DispTarget},
    {op::kSysReg, "sys", Select::RegTarget},
    {op::kTrapReturn, "trap_return", Select::None},
    {op::kHalt, "halt", Select::None},
    {op::kWaitForInterrupt, "wait_for_interrupt", Select::None},
    {op::kNop, "nop", Select::None},
    {op::kTlbInvalidateAll, "tlb_invalidate_all", Select::None},
    {op::kTlbInvalidateAddress, "tlb_invalidate_address", Select::None},
    {op::kPortIn, "port_in", Select::None},
    {op::kPortOut, "port_out", Select::None},
    {op::kCsrSwap, "csr_swap", Select::None},

    // A.12 Floating point, $C8..$F7. Every member has exactly one form.
    {0xC8, "float_add", Select::None},
    {0xC9, "float_add.h", Select::None},
    {0xCA, "float_subtract", Select::None},
    {0xCB, "float_subtract.h", Select::None},
    {0xCC, "float_multiply", Select::None},
    {0xCD, "float_multiply.h", Select::None},
    {0xCE, "float_divide", Select::None},
    {0xCF, "float_divide.h", Select::None},
    {0xD0, "float_square_root", Select::None},
    {0xD1, "float_square_root.h", Select::None},
    {0xD2, "float_negate", Select::None},
    {0xD3, "float_negate.h", Select::None},
    {0xD4, "float_absolute", Select::None},
    {0xD5, "float_absolute.h", Select::None},
    {0xD6, "float_multiply_add", Select::None},
    {0xD7, "float_multiply_add.h", Select::None},
    {0xD8, "float_multiply_subtract", Select::None},
    {0xD9, "float_multiply_subtract.h", Select::None},
    {0xDA, "float_minimum", Select::None},
    {0xDB, "float_minimum.h", Select::None},
    {0xDC, "float_maximum", Select::None},
    {0xDD, "float_maximum.h", Select::None},
    {0xDE, "float_compare_eq", Select::None},
    {0xDF, "float_compare_eq.h", Select::None},
    {0xE0, "float_compare_ne", Select::None},
    {0xE1, "float_compare_ne.h", Select::None},
    {0xE2, "float_compare_lt", Select::None},
    {0xE3, "float_compare_lt.h", Select::None},
    {0xE4, "float_compare_le", Select::None},
    {0xE5, "float_compare_le.h", Select::None},
    {0xE6, "float_compare_ordered", Select::None},
    {0xE7, "float_compare_ordered.h", Select::None},
    {0xE8, "float_compare_unordered", Select::None},
    {0xE9, "float_compare_unordered.h", Select::None},
    {0xEA, "float_narrow", Select::None},
    {0xEB, "float_widen", Select::None},
    {0xEC, "float_to_signed", Select::None},
    {0xED, "float_to_signed.h", Select::None},
    {0xEE, "float_to_unsigned", Select::None},
    {0xEF, "float_to_unsigned.h", Select::None},
    {0xF0, "signed_to_float", Select::None},
    {0xF1, "signed_to_float.h", Select::None},
    {0xF2, "unsigned_to_float", Select::None},
    {0xF3, "unsigned_to_float.h", Select::None},

    // A.14 Breakpoint, $FF. Assigned rather than reserved, and stated in prose rather than in
    // a table row, because it is pinned to the value that fills erased storage.
    {op::kBreakpoint, "breakpoint", Select::None},
}};

}  // namespace maize::v2

#endif  // MAIZE_V2_MNEMONIC_V2_H
