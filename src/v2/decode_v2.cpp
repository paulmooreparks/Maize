// decode_v2.cpp (maize-418): the decode sequence of instruction-encoding.md.

#include "decode_v2.h"

namespace maize::v2 {
namespace {

DecodeResult trapped(std::uint8_t cause_number, std::uint8_t subcode_number, std::uint64_t aux,
                     std::uint64_t pc) {
    DecodeResult result;
    result.status = DecodeStatus::Trap;
    result.trap.cause = cause_number;
    result.trap.subcode = subcode_number;
    result.trap.aux = aux;
    result.trap.pc = pc;
    return result;
}

}  // namespace

DecodeResult decode_v2(const FetchSourceV2& source, std::uint64_t pc) {
    // Step 1. The fetch is an access like any other and is translated like any other
    // (maize-465), so an opcode byte the walk cannot map raises cause 8 and one whose physical
    // address is outside populated memory raises cause 11, rather than either being read as
    // some default byte. In bare mode the source translates nothing and both roads lead to
    // cause 11, exactly as they did before Sv48 existed.
    TrapV2 fetch_trap;
    std::uint8_t opcode_byte = 0;
    if (!source.byte(pc, opcode_byte, fetch_trap)) {
        return trapped(fetch_trap.cause, fetch_trap.subcode, fetch_trap.aux, pc);
    }
    const OpcodeInfo& info = kOpcodeTable[opcode_byte];

    // A reserved byte and an escape byte reach the same trap by two different routes, and both
    // routes are explicit rather than incidental. An escape byte's following byte is never
    // fetched, which is what makes the absence of an extension observable.
    if (info.kind != OpcodeKind::Assigned) {
        return trapped(cause::kIllegalInstruction, 0, opcode_byte, pc);
    }

    DecodedV2 decoded;
    decoded.opcode = opcode_byte;
    decoded.pc = pc;
    decoded.length = info.length;

    // Step 2. The address of the following instruction is fixed here, before a single operand
    // byte is read, and it wraps modulo 2^64 like every other address computation.
    decoded.next_pc = pc + info.length;

    const ShapeInfo shape = shape_info(info.shape);
    decoded.operand_count = shape.operands;
    decoded.immediate_count = shape.immediates;

    // Step 3. Operand bytes, in order, each checked against its declared slot class.
    std::uint64_t cursor = pc + 1;
    for (unsigned i = 0; i < shape.operands; ++i) {
        std::uint8_t operand_byte = 0;
        if (!source.byte(cursor, operand_byte, fetch_trap)) {
            return trapped(fetch_trap.cause, fetch_trap.subcode, fetch_trap.aux, pc);
        }
        const std::uint8_t form = operand_form(operand_byte);
        if (!form_is_legal(info.slots[i], form)) {
            // The offending BYTE accompanies the trap, not the form field alone, per
            // trap-model.md's auxiliary-word column for cause 1.
            return trapped(cause::kIllegalOperand, subcode::kOperandForm, operand_byte, pc);
        }
        decoded.reg[i] = operand_register(operand_byte);
        decoded.form[i] = form;
        ++cursor;
    }

    // Step 4. Immediates, in order, little-endian and whole. The raw field is handed on
    // zero-extended; the opcode's own entry decides whether it is read as signed.
    for (unsigned i = 0; i < shape.immediates; ++i) {
        const unsigned width = shape.immediate_bytes[i];
        std::uint64_t value = 0;
        for (unsigned b = 0; b < width; ++b) {
            std::uint8_t immediate_byte = 0;
            if (!source.byte(cursor, immediate_byte, fetch_trap)) {
                return trapped(fetch_trap.cause, fetch_trap.subcode, fetch_trap.aux, pc);
            }
            value |= static_cast<std::uint64_t>(immediate_byte) << (b * 8);
            ++cursor;
        }
        decoded.immediate[i] = value;
        decoded.immediate_bytes[i] = static_cast<std::uint8_t>(width);
    }

    DecodeResult result;
    result.status = DecodeStatus::Ok;
    result.instruction = decoded;
    return result;
}

}  // namespace maize::v2
