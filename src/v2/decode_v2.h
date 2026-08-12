// decode_v2.h (maize-418): the byte-granular v2 decoder.
//
// instruction-encoding.md fixes a decode sequence with no back-tracking, and this is it:
//
//   1. Read the byte at the program counter. A reserved byte, or an escape byte for an
//      extension this machine does not implement (all seven of them, since no page is
//      allocated), raises the illegal-instruction trap with that byte as the offending byte
//      and that byte's own address as the faulting address. The zero-byte guard $00 falls out
//      of this rule exactly like every other reserved byte, so a run of zeroed memory reached
//      as code stops at its first byte.
//   2. Look up the length class. Add the length to the address of the opcode byte to get the
//      address of the following instruction, BEFORE any operand is read. That ordering is what
//      makes a faulting instruction re-decode identically on restart.
//   3. Read the operand bytes in order, checking each one's form field against the slot class
//      the opcode declares for that position. A plain slot demands %000; a quarter-sliced slot
//      rejects %100 through %111; a half-sliced slot rejects %010 through %111.
//   4. Read the immediates in order, little-endian and whole. Nothing in the instruction
//      selects an immediate's size: the size lives in the opcode.
//
// Sign-extension is deliberately NOT applied here. Whether a narrow immediate is sign-extended
// or zero-extended is a property of the opcode rather than of the encoding, so the decoder
// hands back the raw field and the execute stage applies the rule its entry states.

#ifndef MAIZE_V2_DECODE_V2_H
#define MAIZE_V2_DECODE_V2_H

#include <array>
#include <cstdint>

#include "memory_v2.h"
#include "opcode_v2.h"
#include "trap_v2.h"

namespace maize::v2 {

struct DecodedV2 {
    std::uint8_t opcode = 0;
    std::uint64_t pc = 0;       // address of the opcode byte
    std::uint64_t next_pc = 0;  // pc + length, fixed before any operand is read
    std::uint8_t length = 0;
    std::uint8_t operand_count = 0;
    std::array<std::uint8_t, 4> reg{};   // register numbers, 0 through 31
    std::array<std::uint8_t, 4> form{};  // form fields, already validated against the slot class
    std::uint8_t immediate_count = 0;
    std::array<std::uint64_t, 2> immediate{};       // raw fields, zero-extended from their width
    std::array<std::uint8_t, 2> immediate_bytes{};  // the encoded width of each, in bytes
};

enum class DecodeStatus : std::uint8_t { Ok, Trap };

struct DecodeResult {
    DecodeStatus status = DecodeStatus::Ok;
    DecodedV2 instruction{};
    TrapV2 trap{};
};

// Decode the instruction at `pc`. Never executes and never writes machine state, so a
// disassembler, a tracer or a JIT front end can walk a byte stream with it.
DecodeResult decode_v2(const MemoryV2& memory, std::uint64_t pc);

// Is this form field legal for this slot class? Exposed because the same question is worth
// asking directly in a test.
constexpr bool form_is_legal(Slot slot, std::uint8_t form) {
    switch (slot) {
        case Slot::Plain: return form == 0;
        case Slot::ByteSliced: return form <= 7;
        case Slot::QuarterSliced: return form <= 3;
        case Slot::HalfSliced: return form <= 1;
        case Slot::None: return true;
    }
    return false;
}

constexpr std::uint8_t operand_register(std::uint8_t operand_byte) {
    return static_cast<std::uint8_t>(operand_byte & 0x1F);
}

constexpr std::uint8_t operand_form(std::uint8_t operand_byte) {
    return static_cast<std::uint8_t>(operand_byte >> 5);
}

}  // namespace maize::v2

#endif  // MAIZE_V2_DECODE_V2_H
