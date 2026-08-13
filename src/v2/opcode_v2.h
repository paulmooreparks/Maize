// opcode_v2.h (maize-418): the Maize v2 primary opcode page as data.
//
// Appendix A of docs/spec-v2 assigns every one of the 256 bytes of the primary opcode page
// to an instruction, to an extension escape, or to the reserved set. This header carries
// that assignment verbatim: one entry per byte, giving the length class, the total length in
// bytes, and the operand slot class of every operand byte.
//
// The table is built in FULL, including the bytes this build does not execute (the floating
// point band, the system/CSR/TLB/port band, breakpoint). Length is a decode-layer fact that
// instruction-encoding.md invariant 2 makes a pure function of the leading byte, and invariant
// 3 forbids it from depending on machine state. A table that only described the bytes this
// build can execute would report a state-dependent length the moment maize-419 and maize-420
// close the gap, so the table describes the architecture and the interpreter decides
// separately what it can run.

#ifndef MAIZE_V2_OPCODE_V2_H
#define MAIZE_V2_OPCODE_V2_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace maize::v2 {

// The fifteen length classes of instruction-encoding.md. `op` is the opcode byte, `r` one
// operand byte, `iN` an immediate of N bytes, and the components always appear in that order.
enum class Shape : std::uint8_t {
    None = 0,  // reserved byte or escape byte: no shape, no length
    Op,
    OpR,
    OpRR,
    OpRRR,
    OpRRRR,
    OpI1,
    OpI4,
    OpRI1,
    OpRI2,
    OpRI4,
    OpRI8,
    OpRRI1,
    OpRRI2,
    OpRRI4,
    OpRRI1I1,
};

// The operand slot classes of instruction-encoding.md. A plain slot demands a form field of
// %000; a sliced slot carries an element index there, and the width of the element comes from
// the opcode rather than from the operand byte.
enum class Slot : std::uint8_t {
    None = 0,
    Plain,
    ByteSliced,     // form %000..%111 select rN.b0..rN.b7
    QuarterSliced,  // form %000..%011 select rN.q0..rN.q3; %100..%111 trap
    HalfSliced,     // form %000..%001 select rN.h0..rN.h1; %010..%111 trap
};

enum class OpcodeKind : std::uint8_t {
    Reserved = 0,  // illegal-instruction trap on fetch
    Escape,        // extension page opener; no page is allocated, so it also traps
    Assigned,      // a real instruction of the base
};

struct ShapeInfo {
    std::uint8_t operands = 0;
    std::uint8_t immediates = 0;
    std::array<std::uint8_t, 2> immediate_bytes{0, 0};
    std::uint8_t length = 0;
};

constexpr ShapeInfo shape_info(Shape s) {
    switch (s) {
        case Shape::None: return {0, 0, {0, 0}, 0};
        case Shape::Op: return {0, 0, {0, 0}, 1};
        case Shape::OpR: return {1, 0, {0, 0}, 2};
        case Shape::OpRR: return {2, 0, {0, 0}, 3};
        case Shape::OpRRR: return {3, 0, {0, 0}, 4};
        case Shape::OpRRRR: return {4, 0, {0, 0}, 5};
        case Shape::OpI1: return {0, 1, {1, 0}, 2};
        case Shape::OpI4: return {0, 1, {4, 0}, 5};
        case Shape::OpRI1: return {1, 1, {1, 0}, 3};
        case Shape::OpRI2: return {1, 1, {2, 0}, 4};
        case Shape::OpRI4: return {1, 1, {4, 0}, 6};
        case Shape::OpRI8: return {1, 1, {8, 0}, 10};
        case Shape::OpRRI1: return {2, 1, {1, 0}, 4};
        case Shape::OpRRI2: return {2, 1, {2, 0}, 5};
        case Shape::OpRRI4: return {2, 1, {4, 0}, 7};
        case Shape::OpRRI1I1: return {2, 2, {1, 1}, 5};
    }
    return {0, 0, {0, 0}, 0};
}

struct OpcodeInfo {
    OpcodeKind kind = OpcodeKind::Reserved;
    Shape shape = Shape::None;
    std::uint8_t length = 0;  // total instruction length in bytes; 0 when nothing is assigned
    std::array<Slot, 4> slots{Slot::None, Slot::None, Slot::None, Slot::None};
};

// Opcode byte constants, named exactly as appendix-a-opcode-map.md names the instructions.
namespace op {

// A.3 Constants and moves, $01..$0F
inline constexpr std::uint8_t kMove = 0x01;
inline constexpr std::uint8_t kMoveW = 0x02;
inline constexpr std::uint8_t kMoveZb = 0x03;
inline constexpr std::uint8_t kMoveSb = 0x04;
inline constexpr std::uint8_t kMoveZq = 0x05;
inline constexpr std::uint8_t kMoveSq = 0x06;
inline constexpr std::uint8_t kMoveZh = 0x07;
inline constexpr std::uint8_t kMoveSh = 0x08;
inline constexpr std::uint8_t kPcAdd = 0x09;

// A.4 Integer arithmetic and logic, $10..$3F
inline constexpr std::uint8_t kAdd = 0x10;
inline constexpr std::uint8_t kAddH = 0x11;
inline constexpr std::uint8_t kSubtract = 0x12;
inline constexpr std::uint8_t kSubtractH = 0x13;
inline constexpr std::uint8_t kMultiply = 0x14;
inline constexpr std::uint8_t kMultiplyH = 0x15;
inline constexpr std::uint8_t kMultiplyHighSigned = 0x16;
inline constexpr std::uint8_t kMultiplyHighUnsigned = 0x17;
inline constexpr std::uint8_t kDivideSigned = 0x18;
inline constexpr std::uint8_t kDivideSignedH = 0x19;
inline constexpr std::uint8_t kDivideUnsigned = 0x1A;
inline constexpr std::uint8_t kDivideUnsignedH = 0x1B;
inline constexpr std::uint8_t kRemainderSigned = 0x1C;
inline constexpr std::uint8_t kRemainderSignedH = 0x1D;
inline constexpr std::uint8_t kRemainderUnsigned = 0x1E;
inline constexpr std::uint8_t kRemainderUnsignedH = 0x1F;
inline constexpr std::uint8_t kAnd = 0x20;
inline constexpr std::uint8_t kOr = 0x21;
inline constexpr std::uint8_t kXor = 0x22;
inline constexpr std::uint8_t kShiftLeft = 0x23;
inline constexpr std::uint8_t kShiftLeftH = 0x24;
inline constexpr std::uint8_t kShiftRightLogical = 0x25;
inline constexpr std::uint8_t kShiftRightLogicalH = 0x26;
inline constexpr std::uint8_t kShiftRightArithmetic = 0x27;
inline constexpr std::uint8_t kShiftRightArithmeticH = 0x28;
inline constexpr std::uint8_t kNot = 0x29;
inline constexpr std::uint8_t kNotH = 0x2A;
inline constexpr std::uint8_t kNegate = 0x2B;
inline constexpr std::uint8_t kNegateH = 0x2C;
inline constexpr std::uint8_t kByteReverse = 0x2D;
inline constexpr std::uint8_t kByteReverseH = 0x2E;
inline constexpr std::uint8_t kAddCarry = 0x2F;
inline constexpr std::uint8_t kSubtractBorrow = 0x30;
inline constexpr std::uint8_t kAddImm = 0x31;
inline constexpr std::uint8_t kAddHImm = 0x32;
inline constexpr std::uint8_t kSubtractImm = 0x33;
inline constexpr std::uint8_t kSubtractHImm = 0x34;
inline constexpr std::uint8_t kAndImm = 0x35;
inline constexpr std::uint8_t kOrImm = 0x36;
inline constexpr std::uint8_t kXorImm = 0x37;
inline constexpr std::uint8_t kShiftLeftImm = 0x38;
inline constexpr std::uint8_t kShiftLeftHImm = 0x39;
inline constexpr std::uint8_t kShiftRightLogicalImm = 0x3A;
inline constexpr std::uint8_t kShiftRightLogicalHImm = 0x3B;
inline constexpr std::uint8_t kShiftRightArithmeticImm = 0x3C;
inline constexpr std::uint8_t kShiftRightArithmeticHImm = 0x3D;

// A.5 Compares, $40..$5F. The immediate opcode is the register opcode plus ten.
inline constexpr std::uint8_t kCompareEq = 0x40;
inline constexpr std::uint8_t kCompareNe = 0x41;
inline constexpr std::uint8_t kCompareLtSigned = 0x42;
inline constexpr std::uint8_t kCompareLeSigned = 0x43;
inline constexpr std::uint8_t kCompareGtSigned = 0x44;
inline constexpr std::uint8_t kCompareGeSigned = 0x45;
inline constexpr std::uint8_t kCompareLtUnsigned = 0x46;
inline constexpr std::uint8_t kCompareLeUnsigned = 0x47;
inline constexpr std::uint8_t kCompareGtUnsigned = 0x48;
inline constexpr std::uint8_t kCompareGeUnsigned = 0x49;
inline constexpr std::uint8_t kCompareImmBase = 0x4A;  // $4A..$53, same predicate order

// A.6 Branches, $60..$6F, in the compare predicate order.
inline constexpr std::uint8_t kBranchBase = 0x60;  // $60..$69

// A.7 Control transfer and select, $70..$7F
inline constexpr std::uint8_t kJumpDisp = 0x70;
inline constexpr std::uint8_t kJumpReg = 0x71;
inline constexpr std::uint8_t kCallDisp = 0x72;
inline constexpr std::uint8_t kCallReg = 0x73;
inline constexpr std::uint8_t kReturn = 0x74;
inline constexpr std::uint8_t kSelectNz = 0x75;
inline constexpr std::uint8_t kSelectZ = 0x76;

// A.8 Loads and stores, $80..$9F
inline constexpr std::uint8_t kLoad = 0x80;
inline constexpr std::uint8_t kLoadZb = 0x81;
inline constexpr std::uint8_t kLoadSb = 0x82;
inline constexpr std::uint8_t kLoadZq = 0x83;
inline constexpr std::uint8_t kLoadSq = 0x84;
inline constexpr std::uint8_t kLoadZh = 0x85;
inline constexpr std::uint8_t kLoadSh = 0x86;
inline constexpr std::uint8_t kLoadDisp = 0x87;  // $87..$8D, bare opcode plus seven
inline constexpr std::uint8_t kStore = 0x8E;
inline constexpr std::uint8_t kStoreB = 0x8F;
inline constexpr std::uint8_t kStoreQ = 0x90;
inline constexpr std::uint8_t kStoreH = 0x91;
inline constexpr std::uint8_t kStoreDisp = 0x92;  // $92..$95, bare opcode plus four

// A.9 Extract and insert, $A0..$AF
inline constexpr std::uint8_t kExtractZb = 0xA0;
inline constexpr std::uint8_t kExtractSb = 0xA1;
inline constexpr std::uint8_t kExtractZq = 0xA2;
inline constexpr std::uint8_t kExtractSq = 0xA3;
inline constexpr std::uint8_t kExtractZh = 0xA4;
inline constexpr std::uint8_t kExtractSh = 0xA5;
inline constexpr std::uint8_t kInsertB = 0xA6;
inline constexpr std::uint8_t kInsertQ = 0xA7;
inline constexpr std::uint8_t kInsertH = 0xA8;
inline constexpr std::uint8_t kBitfieldExtract = 0xA9;
inline constexpr std::uint8_t kBitfieldExtractSigned = 0xAA;
inline constexpr std::uint8_t kBitfieldInsert = 0xAB;

// A.10 Block memory, $B0..$B7
inline constexpr std::uint8_t kBlockCopy = 0xB0;
inline constexpr std::uint8_t kBlockCopyForward = 0xB1;
inline constexpr std::uint8_t kBlockSet = 0xB2;

// A.11 System, control registers, TLB, and ports, $B8..$C7.
inline constexpr std::uint8_t kCsrRead = 0xB8;
inline constexpr std::uint8_t kCsrWrite = 0xB9;
inline constexpr std::uint8_t kSysImm = 0xBA;
inline constexpr std::uint8_t kSysReg = 0xBB;
inline constexpr std::uint8_t kTrapReturn = 0xBC;
inline constexpr std::uint8_t kHalt = 0xBD;
inline constexpr std::uint8_t kWaitForInterrupt = 0xBE;
inline constexpr std::uint8_t kNop = 0xBF;
inline constexpr std::uint8_t kTlbInvalidateAll = 0xC0;
inline constexpr std::uint8_t kTlbInvalidateAddress = 0xC1;
inline constexpr std::uint8_t kPortIn = 0xC2;
inline constexpr std::uint8_t kPortOut = 0xC3;
inline constexpr std::uint8_t kCsrSwap = 0xC4;

// A.14 Breakpoint. Assigned, not reserved, so it decodes rather than trapping at decode.
inline constexpr std::uint8_t kBreakpoint = 0xFF;

}  // namespace op

namespace detail {

constexpr void assign(std::array<OpcodeInfo, 256>& t, std::uint8_t byte, Shape shape,
                      Slot s0 = Slot::None, Slot s1 = Slot::None, Slot s2 = Slot::None,
                      Slot s3 = Slot::None) {
    t[byte].kind = OpcodeKind::Assigned;
    t[byte].shape = shape;
    t[byte].length = shape_info(shape).length;
    t[byte].slots = {s0, s1, s2, s3};
}

constexpr std::array<OpcodeInfo, 256> build_opcode_table() {
    std::array<OpcodeInfo, 256> t{};

    // A.3 Constants and moves. Every slot is plain.
    assign(t, op::kMove, Shape::OpRR, Slot::Plain, Slot::Plain);
    assign(t, op::kMoveW, Shape::OpRI8, Slot::Plain);
    assign(t, op::kMoveZb, Shape::OpRI1, Slot::Plain);
    assign(t, op::kMoveSb, Shape::OpRI1, Slot::Plain);
    assign(t, op::kMoveZq, Shape::OpRI2, Slot::Plain);
    assign(t, op::kMoveSq, Shape::OpRI2, Slot::Plain);
    assign(t, op::kMoveZh, Shape::OpRI4, Slot::Plain);
    assign(t, op::kMoveSh, Shape::OpRI4, Slot::Plain);
    assign(t, op::kPcAdd, Shape::OpRI4, Slot::Plain);

    // A.4 Integer arithmetic and logic. $10..$28 are three-operand register forms.
    for (std::uint8_t b = op::kAdd; b <= op::kShiftRightArithmeticH; ++b) {
        assign(t, b, Shape::OpRRR, Slot::Plain, Slot::Plain, Slot::Plain);
    }
    // $29..$2E are the unary forms.
    for (std::uint8_t b = op::kNot; b <= op::kByteReverseH; ++b) {
        assign(t, b, Shape::OpRR, Slot::Plain, Slot::Plain);
    }
    // $2F..$30 are the carry pair, four operand bytes each.
    assign(t, op::kAddCarry, Shape::OpRRRR, Slot::Plain, Slot::Plain, Slot::Plain, Slot::Plain);
    assign(t, op::kSubtractBorrow, Shape::OpRRRR, Slot::Plain, Slot::Plain, Slot::Plain,
           Slot::Plain);
    // $31..$37 are the 32-bit-immediate forms.
    for (std::uint8_t b = op::kAddImm; b <= op::kXorImm; ++b) {
        assign(t, b, Shape::OpRRI4, Slot::Plain, Slot::Plain);
    }
    // $38..$3D are the 8-bit shift-count immediate forms.
    for (std::uint8_t b = op::kShiftLeftImm; b <= op::kShiftRightArithmeticHImm; ++b) {
        assign(t, b, Shape::OpRRI1, Slot::Plain, Slot::Plain);
    }

    // A.5 Compares. Ten register forms then the same ten immediate forms.
    for (std::uint8_t b = op::kCompareEq; b <= op::kCompareGeUnsigned; ++b) {
        assign(t, b, Shape::OpRRR, Slot::Plain, Slot::Plain, Slot::Plain);
    }
    for (std::uint8_t b = op::kCompareImmBase; b <= op::kCompareImmBase + 9; ++b) {
        assign(t, b, Shape::OpRRI4, Slot::Plain, Slot::Plain);
    }

    // A.6 Branches, ten fused compare-and-branch forms.
    for (std::uint8_t b = op::kBranchBase; b <= op::kBranchBase + 9; ++b) {
        assign(t, b, Shape::OpRRI4, Slot::Plain, Slot::Plain);
    }

    // A.7 Control transfer and select.
    assign(t, op::kJumpDisp, Shape::OpI4);
    assign(t, op::kJumpReg, Shape::OpR, Slot::Plain);
    assign(t, op::kCallDisp, Shape::OpI4);
    assign(t, op::kCallReg, Shape::OpR, Slot::Plain);
    assign(t, op::kReturn, Shape::Op);
    assign(t, op::kSelectNz, Shape::OpRRR, Slot::Plain, Slot::Plain, Slot::Plain);
    assign(t, op::kSelectZ, Shape::OpRRR, Slot::Plain, Slot::Plain, Slot::Plain);

    // A.8 Loads and stores. Seven bare loads, seven displaced, four bare stores, four
    // displaced. Every slot is plain: width and the extension rule ride the opcode.
    for (std::uint8_t b = op::kLoad; b <= op::kLoadSh; ++b) {
        assign(t, b, Shape::OpRR, Slot::Plain, Slot::Plain);
    }
    for (std::uint8_t b = op::kLoadDisp; b <= op::kLoadDisp + 6; ++b) {
        assign(t, b, Shape::OpRRI2, Slot::Plain, Slot::Plain);
    }
    for (std::uint8_t b = op::kStore; b <= op::kStoreH; ++b) {
        assign(t, b, Shape::OpRR, Slot::Plain, Slot::Plain);
    }
    for (std::uint8_t b = op::kStoreDisp; b <= op::kStoreDisp + 3; ++b) {
        assign(t, b, Shape::OpRRI2, Slot::Plain, Slot::Plain);
    }

    // A.9 Extract and insert. This is the whole of the base's sliced-slot surface.
    assign(t, op::kExtractZb, Shape::OpRR, Slot::ByteSliced, Slot::Plain);
    assign(t, op::kExtractSb, Shape::OpRR, Slot::ByteSliced, Slot::Plain);
    assign(t, op::kExtractZq, Shape::OpRR, Slot::QuarterSliced, Slot::Plain);
    assign(t, op::kExtractSq, Shape::OpRR, Slot::QuarterSliced, Slot::Plain);
    assign(t, op::kExtractZh, Shape::OpRR, Slot::HalfSliced, Slot::Plain);
    assign(t, op::kExtractSh, Shape::OpRR, Slot::HalfSliced, Slot::Plain);
    assign(t, op::kInsertB, Shape::OpRR, Slot::Plain, Slot::ByteSliced);
    assign(t, op::kInsertQ, Shape::OpRR, Slot::Plain, Slot::QuarterSliced);
    assign(t, op::kInsertH, Shape::OpRR, Slot::Plain, Slot::HalfSliced);
    assign(t, op::kBitfieldExtract, Shape::OpRRI1I1, Slot::Plain, Slot::Plain);
    assign(t, op::kBitfieldExtractSigned, Shape::OpRRI1I1, Slot::Plain, Slot::Plain);
    assign(t, op::kBitfieldInsert, Shape::OpRRI1I1, Slot::Plain, Slot::Plain);

    // A.10 Block memory.
    assign(t, op::kBlockCopy, Shape::OpRRR, Slot::Plain, Slot::Plain, Slot::Plain);
    assign(t, op::kBlockCopyForward, Shape::OpRRR, Slot::Plain, Slot::Plain, Slot::Plain);
    assign(t, op::kBlockSet, Shape::OpRRR, Slot::Plain, Slot::Plain, Slot::Plain);

    // A.11 System, control registers, TLB, and ports. Assigned here so the length table is
    // complete; only halt executes in this build (D-2).
    assign(t, op::kCsrRead, Shape::OpRI2, Slot::Plain);
    assign(t, op::kCsrWrite, Shape::OpRI2, Slot::Plain);
    assign(t, op::kSysImm, Shape::OpI1);
    assign(t, op::kSysReg, Shape::OpR, Slot::Plain);
    assign(t, op::kTrapReturn, Shape::Op);
    assign(t, op::kHalt, Shape::Op);
    assign(t, op::kWaitForInterrupt, Shape::Op);
    assign(t, op::kNop, Shape::Op);
    assign(t, op::kTlbInvalidateAll, Shape::Op);
    assign(t, op::kTlbInvalidateAddress, Shape::OpR, Slot::Plain);
    assign(t, op::kPortIn, Shape::OpRR, Slot::Plain, Slot::Plain);
    assign(t, op::kPortOut, Shape::OpRR, Slot::Plain, Slot::Plain);
    assign(t, op::kCsrSwap, Shape::OpRRI2, Slot::Plain, Slot::Plain);

    // A.12 Floating point, $C8..$F7, of which $C8..$F3 are assigned and $F4..$F7 are reserved,
    // so the loops below stop at $F3 and the reserved tail is left unassigned. Within the band
    // the binary64 form sits at an even offset from $C8 and its binary32 .h form is the next
    // byte up.
    for (std::uint8_t b = 0xC8; b <= 0xCF; ++b) {  // add/subtract/multiply/divide, both formats
        assign(t, b, Shape::OpRRR, Slot::Plain, Slot::Plain, Slot::Plain);
    }
    for (std::uint8_t b = 0xD0; b <= 0xD5; ++b) {  // square_root, negate, absolute
        assign(t, b, Shape::OpRR, Slot::Plain, Slot::Plain);
    }
    for (std::uint8_t b = 0xD6; b <= 0xD9; ++b) {  // multiply_add, multiply_subtract
        assign(t, b, Shape::OpRRRR, Slot::Plain, Slot::Plain, Slot::Plain, Slot::Plain);
    }
    for (std::uint8_t b = 0xDA; b <= 0xE9; ++b) {  // minimum, maximum, the six compares
        assign(t, b, Shape::OpRRR, Slot::Plain, Slot::Plain, Slot::Plain);
    }
    for (std::uint8_t b = 0xEA; b <= 0xF3; ++b) {  // narrow, widen, the eight conversions
        assign(t, b, Shape::OpRR, Slot::Plain, Slot::Plain);
    }

    // A.13 Extension escape bytes. No page is allocated, so a base-only machine raises the
    // illegal-instruction trap on the escape byte and never fetches the byte after it.
    for (std::uint8_t b = 0xF8; b <= 0xFE; ++b) {
        t[b].kind = OpcodeKind::Escape;
    }

    // A.14 Breakpoint. One byte, assigned rather than reserved.
    assign(t, op::kBreakpoint, Shape::Op);

    return t;
}

}  // namespace detail

// The opcode table, one entry per byte of the primary page.
inline constexpr std::array<OpcodeInfo, 256> kOpcodeTable = detail::build_opcode_table();

// The 256-entry length table instruction-encoding.md invariant 2 names, expressed as the
// decoder actually consumes it: the total instruction length in bytes, or -1 for a byte that
// has no length because it raises the illegal-instruction trap instead (reserved or escape).
inline constexpr std::array<std::int8_t, 256> build_length_table() {
    std::array<std::int8_t, 256> lengths{};
    for (int i = 0; i < 256; ++i) {
        lengths[static_cast<std::size_t>(i)] =
            kOpcodeTable[static_cast<std::size_t>(i)].kind == OpcodeKind::Assigned
                ? static_cast<std::int8_t>(kOpcodeTable[static_cast<std::size_t>(i)].length)
                : static_cast<std::int8_t>(-1);
    }
    return lengths;
}

inline constexpr std::array<std::int8_t, 256> kLengthTable = build_length_table();

constexpr std::int8_t instruction_length(std::uint8_t opcode_byte) {
    return kLengthTable[opcode_byte];
}

}  // namespace maize::v2

#endif  // MAIZE_V2_OPCODE_V2_H
