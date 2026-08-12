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

DecodeResult decode_v2(const MemoryV2& memory, std::uint64_t pc) {
    // Step 1. The fetch is itself a physical access in bare mode, so an opcode byte outside
    // populated memory raises cause 11 rather than being read as some default byte.
    if (!memory.accessible(pc)) {
        return trapped(cause::kPhysicalMemoryFault, 0, pc, pc);
    }

    const std::uint8_t opcode_byte = memory.read_byte(pc);
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
        if (!memory.accessible(cursor)) {
            return trapped(cause::kPhysicalMemoryFault, 0, cursor, pc);
        }
        const std::uint8_t operand_byte = memory.read_byte(cursor);
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
            if (!memory.accessible(cursor)) {
                return trapped(cause::kPhysicalMemoryFault, 0, cursor, pc);
            }
            value |= static_cast<std::uint64_t>(memory.read_byte(cursor)) << (b * 8);
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
