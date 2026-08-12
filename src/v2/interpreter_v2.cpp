// interpreter_v2.cpp (maize-418): the v2 execute stage.

#include "interpreter_v2.h"

namespace maize::v2 {
namespace {

constexpr std::uint64_t kAllOnes = ~std::uint64_t{0};

constexpr std::uint64_t low_half(std::uint64_t value) {
    return value & 0xFFFFFFFFu;
}

// A .h operation reads the low half-word of each source, computes a 32-bit result, and writes
// that result ZERO-EXTENDED into the full destination, so bits 63 through 32 are always zero
// afterward. This one helper is how every .h entry in the file gets that right.
constexpr std::uint64_t half_result(std::uint32_t value) {
    return static_cast<std::uint64_t>(value);
}

constexpr std::uint64_t field_mask(unsigned width) {
    return width >= 64 ? kAllOnes : ((std::uint64_t{1} << width) - 1);
}

constexpr std::uint64_t reverse_bytes_word(std::uint64_t value) {
    std::uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        result |= ((value >> (i * 8)) & 0xFF) << ((7 - i) * 8);
    }
    return result;
}

constexpr std::uint32_t reverse_bytes_half(std::uint32_t value) {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        result |= ((value >> (i * 8)) & 0xFF) << ((3 - i) * 8);
    }
    return result;
}

}  // namespace

bool evaluate_predicate(unsigned predicate, std::uint64_t left, std::uint64_t right) {
    const std::int64_t signed_left = static_cast<std::int64_t>(left);
    const std::int64_t signed_right = static_cast<std::int64_t>(right);
    switch (predicate) {
        case 0: return left == right;                  // eq
        case 1: return left != right;                  // ne
        case 2: return signed_left < signed_right;     // lt_signed
        case 3: return signed_left <= signed_right;    // le_signed
        case 4: return signed_left > signed_right;     // gt_signed
        case 5: return signed_left >= signed_right;    // ge_signed
        case 6: return left < right;                   // lt_unsigned
        case 7: return left <= right;                  // le_unsigned
        case 8: return left > right;                   // gt_unsigned
        case 9: return left >= right;                  // ge_unsigned
        default: return false;
    }
}

void multiply_full_unsigned(std::uint64_t a, std::uint64_t b, std::uint64_t& low,
                            std::uint64_t& high) {
    const std::uint64_t a_low = a & 0xFFFFFFFFu;
    const std::uint64_t a_high = a >> 32;
    const std::uint64_t b_low = b & 0xFFFFFFFFu;
    const std::uint64_t b_high = b >> 32;

    const std::uint64_t low_low = a_low * b_low;
    const std::uint64_t low_high = a_low * b_high;
    const std::uint64_t high_low = a_high * b_low;
    const std::uint64_t high_high = a_high * b_high;

    const std::uint64_t middle = (low_low >> 32) + (low_high & 0xFFFFFFFFu) + (high_low & 0xFFFFFFFFu);
    low = (middle << 32) | (low_low & 0xFFFFFFFFu);
    high = high_high + (low_high >> 32) + (high_low >> 32) + (middle >> 32);
}

std::uint64_t multiply_high_signed_value(std::uint64_t a, std::uint64_t b) {
    std::uint64_t low = 0;
    std::uint64_t high = 0;
    multiply_full_unsigned(a, b, low, high);
    // The signed high half is the unsigned high half corrected for each operand's sign, which
    // is the standard two's-complement adjustment: subtract the other operand once per negative
    // operand.
    if (static_cast<std::int64_t>(a) < 0) {
        high -= b;
    }
    if (static_cast<std::int64_t>(b) < 0) {
        high -= a;
    }
    return high;
}

StepResult InterpreterV2::advance(const DecodedV2& decoded) {
    pc_ = decoded.next_pc;
    StepResult result;
    result.status = StepStatus::Advanced;
    result.opcode = decoded.opcode;
    result.pc = decoded.pc;
    return result;
}

StepResult InterpreterV2::branch_to(const DecodedV2& decoded, std::uint64_t target) {
    pc_ = target;
    StepResult result;
    result.status = StepStatus::Advanced;
    result.opcode = decoded.opcode;
    result.pc = decoded.pc;
    return result;
}

StepResult InterpreterV2::raise(const DecodedV2& decoded, std::uint8_t cause_number,
                                std::uint8_t subcode_number, std::uint64_t aux) {
    // The program counter does NOT move. A fault-class trap is restartable, and a handler that
    // fixes the cause and resumes at the faulting address gets a clean re-execution.
    StepResult result;
    result.status = StepStatus::Trapped;
    result.opcode = decoded.opcode;
    result.pc = decoded.pc;
    result.trap.cause = cause_number;
    result.trap.subcode = subcode_number;
    result.trap.aux = aux;
    result.trap.pc = decoded.pc;
    return result;
}

StepResult InterpreterV2::step() {
    if (halted_) {
        StepResult result;
        result.status = StepStatus::Halted;
        result.pc = pc_;
        return result;
    }

    const DecodeResult decoded = decode_v2(memory_, pc_);
    if (decoded.status == DecodeStatus::Trap) {
        StepResult result;
        result.status = StepStatus::Trapped;
        result.trap = decoded.trap;
        result.pc = decoded.trap.pc;
        return result;
    }

    ++steps_taken_;
    return execute(decoded.instruction);
}

StepResult InterpreterV2::run(std::uint64_t max_steps) {
    StepResult result;
    std::uint64_t taken = 0;
    for (;;) {
        result = step();
        if (result.status != StepStatus::Advanced) {
            return result;
        }
        ++taken;
        if (max_steps != 0 && taken >= max_steps) {
            return result;
        }
    }
}

StepResult InterpreterV2::execute_load(const DecodedV2& decoded, unsigned width_bytes,
                                       bool sign_extended, bool displaced) {
    const unsigned base_register = decoded.reg[0];
    const unsigned destination = decoded.reg[1];
    const std::uint64_t displacement =
        displaced ? sign_extend(decoded.immediate[0], 16) : std::uint64_t{0};
    const std::uint64_t address = registers_.read(base_register) + displacement;

    // Validate the WHOLE access before writing the destination. A load that would write its
    // destination on success must not have written it when the second half of a two-region
    // access is inaccessible, which is what makes the instruction restartable.
    std::uint64_t lowest_inaccessible = 0;
    if (!memory_.check_range(address, width_bytes, lowest_inaccessible)) {
        return raise(decoded, cause::kPhysicalMemoryFault, 0, lowest_inaccessible);
    }

    const std::uint64_t raw = memory_.read_little_endian(address, width_bytes);
    const unsigned bits = width_bytes * 8;
    const std::uint64_t value =
        sign_extended ? sign_extend(raw, bits) : (bits >= 64 ? raw : zero_extend(raw, bits));
    registers_.write(destination, value);
    return advance(decoded);
}

StepResult InterpreterV2::execute_store(const DecodedV2& decoded, unsigned width_bytes,
                                        bool displaced) {
    const unsigned source = decoded.reg[0];
    const unsigned base_register = decoded.reg[1];
    const std::uint64_t displacement =
        displaced ? sign_extend(decoded.immediate[0], 16) : std::uint64_t{0};
    const std::uint64_t address = registers_.read(base_register) + displacement;

    // Same ordering as the load, from the other side: no byte of memory changes when any byte
    // of the access is inaccessible, including the case where the first part of the access is
    // fine and the second is not.
    std::uint64_t lowest_inaccessible = 0;
    if (!memory_.check_range(address, width_bytes, lowest_inaccessible)) {
        return raise(decoded, cause::kPhysicalMemoryFault, 0, lowest_inaccessible);
    }

    memory_.write_little_endian(address, width_bytes, registers_.read(source));
    return advance(decoded);
}

StepResult InterpreterV2::execute_block(const DecodedV2& decoded) {
    const bool is_set = decoded.opcode == op::kBlockSet;
    const unsigned slot0 = decoded.reg[0];  // source pointer, or the fill value for block_set
    const unsigned slot1 = decoded.reg[1];  // destination pointer
    const unsigned slot2 = decoded.reg[2];  // count

    // Both encoding rules are decided before any byte transfers, because they constrain
    // encodings rather than values. Aliased slots would make the mid-operation restart state
    // unrepresentable, and a register that discards writes cannot hold a pointer that must
    // advance or a count that must reach zero. The one slot that admits r0 is block_set's value
    // slot, which carries no operation state and is never written.
    const bool slot0_carries_state = !is_set;
    if ((slot0_carries_state && slot0 == 0) || slot1 == 0 || slot2 == 0) {
        const unsigned offending = (slot0_carries_state && slot0 == 0) ? 0u : (slot1 == 0 ? 1u : 2u);
        return raise(decoded, cause::kIllegalOperand, subcode::kBlockMemoryOperands,
                     decoded.reg[offending]);
    }
    // Report the later slot of the colliding pair, so the reported operand byte is the one a
    // reader scanning the encoding left to right meets second and can see is a repeat.
    unsigned aliased = 4;
    if (slot1 == slot0) {
        aliased = 1;
    } else if (slot2 == slot0 || slot2 == slot1) {
        aliased = 2;
    }
    if (aliased != 4) {
        return raise(decoded, cause::kIllegalOperand, subcode::kBlockMemoryOperands,
                     decoded.reg[aliased]);
    }

    const std::uint64_t count = registers_.read(slot2);
    if (count == 0) {
        // A count of zero is valid, performs no access, raises no fault, and leaves the named
        // registers as it found them, which is also the completion state (pointer plus zero).
        return advance(decoded);
    }

    if (is_set) {
        const std::uint8_t fill = static_cast<std::uint8_t>(registers_.read(slot0));
        const std::uint64_t destination = registers_.read(slot1);
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint64_t address = destination + i;
            if (!memory_.accessible(address)) {
                // Restart state: the untransferred bytes are the ones from here up.
                registers_.write(slot1, address);
                registers_.write(slot2, count - i);
                return raise(decoded, cause::kPhysicalMemoryFault, 0, address);
            }
            memory_.write_byte(address, fill);
        }
        registers_.write(slot1, destination + count);
        registers_.write(slot2, 0);
        return advance(decoded);
    }

    const std::uint64_t source = registers_.read(slot0);
    const std::uint64_t destination = registers_.read(slot1);

    // block_copy gives the result that reading the whole source before writing any of the
    // destination would give, for every overlap including full coincidence. That is exactly
    // memmove's condition, and it is met by choosing the direction of travel rather than by
    // buffering the region: an ascending pass is safe unless the destination begins strictly
    // inside the source region, in which case a descending pass is. block_copy_forward's
    // ascending byte-by-byte result is architectural, so it never chooses.
    const std::uint64_t offset = destination - source;  // wraps modulo 2^64
    const bool descending =
        decoded.opcode == op::kBlockCopy && offset != 0 && offset < count;

    if (!descending) {
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint64_t from = source + i;
            const std::uint64_t to = destination + i;
            if (!memory_.accessible(from) || !memory_.accessible(to)) {
                const std::uint64_t faulting = !memory_.accessible(from) ? from : to;
                // Low to high shows its progress by advancing both pointers and decrementing
                // the count together.
                registers_.write(slot0, from);
                registers_.write(slot1, to);
                registers_.write(slot2, count - i);
                return raise(decoded, cause::kPhysicalMemoryFault, 0, faulting);
            }
            memory_.write_byte(to, memory_.read_byte(from));
        }
    } else {
        for (std::uint64_t i = count; i-- > 0;) {
            const std::uint64_t from = source + i;
            const std::uint64_t to = destination + i;
            if (!memory_.accessible(from) || !memory_.accessible(to)) {
                const std::uint64_t faulting = !memory_.accessible(from) ? from : to;
                // High to low shows its progress by decrementing the count alone: the bytes not
                // yet transferred are still the LOW ones, so both pointers already name them
                // and neither moves.
                registers_.write(slot2, i + 1);
                return raise(decoded, cause::kPhysicalMemoryFault, 0, faulting);
            }
            memory_.write_byte(to, memory_.read_byte(from));
        }
    }

    // Both directions converge here. On normal completion the count is zero and each pointer
    // holds its original value plus the ORIGINAL count, so it points just past the last byte of
    // its region, whatever direction the transfer travelled.
    registers_.write(slot0, source + count);
    registers_.write(slot1, destination + count);
    registers_.write(slot2, 0);
    return advance(decoded);
}

StepResult InterpreterV2::execute(const DecodedV2& decoded) {
    const std::uint8_t opcode = decoded.opcode;

    // Constants and moves, $01..$09.
    switch (opcode) {
        case op::kMove:
            registers_.write(decoded.reg[1], registers_.read(decoded.reg[0]));
            return advance(decoded);
        case op::kMoveW:
            registers_.write(decoded.reg[0], decoded.immediate[0]);
            return advance(decoded);
        case op::kMoveZb:
            registers_.write(decoded.reg[0], zero_extend(decoded.immediate[0], 8));
            return advance(decoded);
        case op::kMoveSb:
            registers_.write(decoded.reg[0], sign_extend(decoded.immediate[0], 8));
            return advance(decoded);
        case op::kMoveZq:
            registers_.write(decoded.reg[0], zero_extend(decoded.immediate[0], 16));
            return advance(decoded);
        case op::kMoveSq:
            registers_.write(decoded.reg[0], sign_extend(decoded.immediate[0], 16));
            return advance(decoded);
        case op::kMoveZh:
            registers_.write(decoded.reg[0], zero_extend(decoded.immediate[0], 32));
            return advance(decoded);
        case op::kMoveSh:
            registers_.write(decoded.reg[0], sign_extend(decoded.immediate[0], 32));
            return advance(decoded);
        case op::kPcAdd:
            // The displacement is measured from the address of the FOLLOWING instruction, so
            // pc_add #0 rd writes this instruction's address plus six.
            registers_.write(decoded.reg[0], decoded.next_pc + sign_extend(decoded.immediate[0], 32));
            return advance(decoded);
        default:
            break;
    }

    // Integer arithmetic and logic, register forms $10..$28.
    if (opcode >= op::kAdd && opcode <= op::kShiftRightArithmeticH) {
        const std::uint64_t a = registers_.read(decoded.reg[0]);
        const std::uint64_t b = registers_.read(decoded.reg[1]);
        const unsigned destination = decoded.reg[2];
        const std::uint32_t a_half = static_cast<std::uint32_t>(low_half(a));
        const std::uint32_t b_half = static_cast<std::uint32_t>(low_half(b));

        switch (opcode) {
            case op::kAdd:
                registers_.write(destination, a + b);
                return advance(decoded);
            case op::kAddH:
                registers_.write(destination, half_result(a_half + b_half));
                return advance(decoded);
            case op::kSubtract:
                registers_.write(destination, a - b);
                return advance(decoded);
            case op::kSubtractH:
                registers_.write(destination, half_result(a_half - b_half));
                return advance(decoded);
            case op::kMultiply:
                registers_.write(destination, a * b);
                return advance(decoded);
            case op::kMultiplyH:
                registers_.write(destination, half_result(a_half * b_half));
                return advance(decoded);
            case op::kMultiplyHighSigned:
                registers_.write(destination, multiply_high_signed_value(a, b));
                return advance(decoded);
            case op::kMultiplyHighUnsigned: {
                std::uint64_t low = 0;
                std::uint64_t high = 0;
                multiply_full_unsigned(a, b, low, high);
                registers_.write(destination, high);
                return advance(decoded);
            }
            case op::kDivideSigned: {
                // Check first, compute second. The destination keeps its prior value on either
                // trap, which is what a fault test probes.
                if (b == 0) {
                    return raise(decoded, cause::kDivideError, subcode::kDivideByZero, 0);
                }
                if (a == (std::uint64_t{1} << 63) && b == kAllOnes) {
                    return raise(decoded, cause::kDivideError, subcode::kQuotientOverflow, 0);
                }
                registers_.write(destination,
                                 static_cast<std::uint64_t>(static_cast<std::int64_t>(a) /
                                                            static_cast<std::int64_t>(b)));
                return advance(decoded);
            }
            case op::kDivideSignedH: {
                if (b_half == 0) {
                    return raise(decoded, cause::kDivideError, subcode::kDivideByZero, 0);
                }
                if (a_half == 0x80000000u && b_half == 0xFFFFFFFFu) {
                    return raise(decoded, cause::kDivideError, subcode::kQuotientOverflow, 0);
                }
                const std::int32_t quotient =
                    static_cast<std::int32_t>(a_half) / static_cast<std::int32_t>(b_half);
                registers_.write(destination, half_result(static_cast<std::uint32_t>(quotient)));
                return advance(decoded);
            }
            case op::kDivideUnsigned:
                if (b == 0) {
                    return raise(decoded, cause::kDivideError, subcode::kDivideByZero, 0);
                }
                registers_.write(destination, a / b);
                return advance(decoded);
            case op::kDivideUnsignedH:
                if (b_half == 0) {
                    return raise(decoded, cause::kDivideError, subcode::kDivideByZero, 0);
                }
                registers_.write(destination, half_result(a_half / b_half));
                return advance(decoded);
            case op::kRemainderSigned: {
                if (b == 0) {
                    return raise(decoded, cause::kDivideError, subcode::kDivideByZero, 0);
                }
                // The most negative value remainder -1 is exactly zero and IS representable,
                // even though the corresponding quotient is not, so this writes zero and raises
                // nothing where divide_signed traps.
                if (a == (std::uint64_t{1} << 63) && b == kAllOnes) {
                    registers_.write(destination, 0);
                    return advance(decoded);
                }
                registers_.write(destination,
                                 static_cast<std::uint64_t>(static_cast<std::int64_t>(a) %
                                                            static_cast<std::int64_t>(b)));
                return advance(decoded);
            }
            case op::kRemainderSignedH: {
                if (b_half == 0) {
                    return raise(decoded, cause::kDivideError, subcode::kDivideByZero, 0);
                }
                if (a_half == 0x80000000u && b_half == 0xFFFFFFFFu) {
                    registers_.write(destination, 0);
                    return advance(decoded);
                }
                const std::int32_t remainder =
                    static_cast<std::int32_t>(a_half) % static_cast<std::int32_t>(b_half);
                registers_.write(destination, half_result(static_cast<std::uint32_t>(remainder)));
                return advance(decoded);
            }
            case op::kRemainderUnsigned:
                if (b == 0) {
                    return raise(decoded, cause::kDivideError, subcode::kDivideByZero, 0);
                }
                registers_.write(destination, a % b);
                return advance(decoded);
            case op::kRemainderUnsignedH:
                if (b_half == 0) {
                    return raise(decoded, cause::kDivideError, subcode::kDivideByZero, 0);
                }
                registers_.write(destination, half_result(a_half % b_half));
                return advance(decoded);
            case op::kAnd:
                registers_.write(destination, a & b);
                return advance(decoded);
            case op::kOr:
                registers_.write(destination, a | b);
                return advance(decoded);
            case op::kXor:
                registers_.write(destination, a ^ b);
                return advance(decoded);
            case op::kShiftLeft:
                registers_.write(destination, a << (b & 63));
                return advance(decoded);
            case op::kShiftLeftH:
                registers_.write(destination, half_result(a_half << (b_half & 31)));
                return advance(decoded);
            case op::kShiftRightLogical:
                registers_.write(destination, a >> (b & 63));
                return advance(decoded);
            case op::kShiftRightLogicalH:
                registers_.write(destination, half_result(a_half >> (b_half & 31)));
                return advance(decoded);
            case op::kShiftRightArithmetic:
                registers_.write(destination, static_cast<std::uint64_t>(
                                                  static_cast<std::int64_t>(a) >> (b & 63)));
                return advance(decoded);
            case op::kShiftRightArithmeticH:
                registers_.write(destination,
                                 half_result(static_cast<std::uint32_t>(
                                     static_cast<std::int32_t>(a_half) >> (b_half & 31))));
                return advance(decoded);
            default:
                break;
        }
    }

    // Unary forms, $29..$2E.
    if (opcode >= op::kNot && opcode <= op::kByteReverseH) {
        const std::uint64_t a = registers_.read(decoded.reg[0]);
        const unsigned destination = decoded.reg[1];
        const std::uint32_t a_half = static_cast<std::uint32_t>(low_half(a));
        switch (opcode) {
            case op::kNot:
                registers_.write(destination, ~a);
                return advance(decoded);
            case op::kNotH:
                registers_.write(destination, half_result(~a_half));
                return advance(decoded);
            case op::kNegate:
                registers_.write(destination, std::uint64_t{0} - a);
                return advance(decoded);
            case op::kNegateH:
                registers_.write(destination, half_result(std::uint32_t{0} - a_half));
                return advance(decoded);
            case op::kByteReverse:
                registers_.write(destination, reverse_bytes_word(a));
                return advance(decoded);
            case op::kByteReverseH:
                registers_.write(destination, half_result(reverse_bytes_half(a_half)));
                return advance(decoded);
            default:
                break;
        }
    }

    // The carry pair, $2F..$30. Both write rd FIRST and rc SECOND, and that ordering is what
    // makes an rd naming the same register as rc yield the carry-out rather than the sum.
    if (opcode == op::kAddCarry || opcode == op::kSubtractBorrow) {
        const std::uint64_t a = registers_.read(decoded.reg[0]);
        const std::uint64_t b = registers_.read(decoded.reg[1]);
        const unsigned carry_register = decoded.reg[2];
        const unsigned destination = decoded.reg[3];
        // Only bit 0 of the carry register is read; the rest is ignored on the way in.
        const std::uint64_t carry_in = registers_.read(carry_register) & 1u;

        if (opcode == op::kAddCarry) {
            const std::uint64_t partial = a + b;
            const bool carry_from_sum = partial < a;
            const std::uint64_t total = partial + carry_in;
            const bool carry_from_increment = total < partial;
            registers_.write(destination, total);
            registers_.write(carry_register,
                             (carry_from_sum || carry_from_increment) ? 1u : 0u);
        } else {
            const std::uint64_t partial = a - b;
            const bool borrow_from_difference = a < b;
            const std::uint64_t total = partial - carry_in;
            const bool borrow_from_decrement = partial < carry_in;
            registers_.write(destination, total);
            registers_.write(carry_register,
                             (borrow_from_difference || borrow_from_decrement) ? 1u : 0u);
        }
        return advance(decoded);
    }

    // Immediate arithmetic and logic, $31..$37. The literal is always the second source, and
    // the operand bytes name the register source and then the destination.
    if (opcode >= op::kAddImm && opcode <= op::kXorImm) {
        const std::uint64_t a = registers_.read(decoded.reg[0]);
        const unsigned destination = decoded.reg[1];
        // A word-form literal is a 32-bit value sign-extended to 64 bits, bitwise operations
        // included. A .h-form literal IS the half-word operand and no extension applies.
        const std::uint64_t literal = sign_extend(decoded.immediate[0], 32);
        const std::uint32_t literal_half = static_cast<std::uint32_t>(decoded.immediate[0]);
        const std::uint32_t a_half = static_cast<std::uint32_t>(low_half(a));
        switch (opcode) {
            case op::kAddImm:
                registers_.write(destination, a + literal);
                return advance(decoded);
            case op::kAddHImm:
                registers_.write(destination, half_result(a_half + literal_half));
                return advance(decoded);
            case op::kSubtractImm:
                registers_.write(destination, a - literal);
                return advance(decoded);
            case op::kSubtractHImm:
                registers_.write(destination, half_result(a_half - literal_half));
                return advance(decoded);
            case op::kAndImm:
                registers_.write(destination, a & literal);
                return advance(decoded);
            case op::kOrImm:
                registers_.write(destination, a | literal);
                return advance(decoded);
            case op::kXorImm:
                registers_.write(destination, a ^ literal);
                return advance(decoded);
            default:
                break;
        }
    }

    // Immediate shifts, $38..$3D. The count is an 8-bit literal, masked before use, so its
    // signedness never arises and every one of the 256 values is defined.
    if (opcode >= op::kShiftLeftImm && opcode <= op::kShiftRightArithmeticHImm) {
        const std::uint64_t a = registers_.read(decoded.reg[0]);
        const unsigned destination = decoded.reg[1];
        const std::uint64_t count = decoded.immediate[0];
        const std::uint32_t a_half = static_cast<std::uint32_t>(low_half(a));
        switch (opcode) {
            case op::kShiftLeftImm:
                registers_.write(destination, a << (count & 63));
                return advance(decoded);
            case op::kShiftLeftHImm:
                registers_.write(destination, half_result(a_half << (count & 31)));
                return advance(decoded);
            case op::kShiftRightLogicalImm:
                registers_.write(destination, a >> (count & 63));
                return advance(decoded);
            case op::kShiftRightLogicalHImm:
                registers_.write(destination, half_result(a_half >> (count & 31)));
                return advance(decoded);
            case op::kShiftRightArithmeticImm:
                registers_.write(destination, static_cast<std::uint64_t>(
                                                  static_cast<std::int64_t>(a) >> (count & 63)));
                return advance(decoded);
            case op::kShiftRightArithmeticHImm:
                registers_.write(destination,
                                 half_result(static_cast<std::uint32_t>(
                                     static_cast<std::int32_t>(a_half) >> (count & 31))));
                return advance(decoded);
            default:
                break;
        }
    }

    // Compares, register forms $40..$49. The destination is written whole, 1 or 0 across all
    // 64 bits, so a compare result is already a canonical boolean.
    if (opcode >= op::kCompareEq && opcode <= op::kCompareGeUnsigned) {
        const unsigned predicate = opcode - op::kCompareEq;
        const bool held = evaluate_predicate(predicate, registers_.read(decoded.reg[0]),
                                             registers_.read(decoded.reg[1]));
        registers_.write(decoded.reg[2], held ? 1u : 0u);
        return advance(decoded);
    }

    // Compares, immediate forms $4A..$53, the same ten predicates in the same order. An
    // unsigned comparison against a literal sign-extends the literal first and then reads it as
    // unsigned, which is what gives a 32-bit literal access to the top of the unsigned range.
    if (opcode >= op::kCompareImmBase && opcode <= op::kCompareImmBase + 9) {
        const unsigned predicate = opcode - op::kCompareImmBase;
        const bool held = evaluate_predicate(predicate, registers_.read(decoded.reg[0]),
                                             sign_extend(decoded.immediate[0], 32));
        registers_.write(decoded.reg[1], held ? 1u : 0u);
        return advance(decoded);
    }

    // Branches, $60..$69, the same ten predicates again. The displacement is a signed 32-bit
    // byte count measured from the address of the instruction FOLLOWING the branch, so a
    // displacement of zero falls through.
    if (opcode >= op::kBranchBase && opcode <= op::kBranchBase + 9) {
        const unsigned predicate = opcode - op::kBranchBase;
        const bool held = evaluate_predicate(predicate, registers_.read(decoded.reg[0]),
                                             registers_.read(decoded.reg[1]));
        if (!held) {
            return advance(decoded);
        }
        return branch_to(decoded, decoded.next_pc + sign_extend(decoded.immediate[0], 32));
    }

    // Control transfer and select, $70..$76.
    switch (opcode) {
        case op::kJumpDisp:
            return branch_to(decoded, decoded.next_pc + sign_extend(decoded.immediate[0], 32));
        case op::kJumpReg:
            // The register form takes an ABSOLUTE address from the whole register, and neither
            // form of jump disturbs r31.
            return branch_to(decoded, registers_.read(decoded.reg[0]));
        case op::kCallDisp: {
            const std::uint64_t target =
                decoded.next_pc + sign_extend(decoded.immediate[0], 32);
            registers_.write(kLinkRegister, decoded.next_pc);
            return branch_to(decoded, target);
        }
        case op::kCallReg: {
            // Read the target BEFORE writing the link, which is what makes `call r31` a
            // well-defined call through the current link register.
            const std::uint64_t target = registers_.read(decoded.reg[0]);
            registers_.write(kLinkRegister, decoded.next_pc);
            return branch_to(decoded, target);
        }
        case op::kReturn:
            return branch_to(decoded, registers_.read(kLinkRegister));
        case op::kSelectNz:
        case op::kSelectZ: {
            const std::uint64_t source = registers_.read(decoded.reg[0]);
            const std::uint64_t condition = registers_.read(decoded.reg[1]);
            // The condition is the WHOLE 64-bit value tested against zero; no single bit is
            // privileged. When it does not hold the destination is not written at all, which is
            // the difference between select and a masked arithmetic sequence.
            const bool holds =
                (opcode == op::kSelectNz) ? (condition != 0) : (condition == 0);
            if (holds) {
                registers_.write(decoded.reg[2], source);
            }
            return advance(decoded);
        }
        default:
            break;
    }

    // Loads, bare $80..$86 and displaced $87..$8D. Width and the extension rule live entirely
    // in the opcode, so no operand byte in this family carries anything but a register number.
    if (opcode >= op::kLoad && opcode <= op::kLoadDisp + 6) {
        const bool displaced = opcode >= op::kLoadDisp;
        const unsigned index = displaced ? (opcode - op::kLoadDisp) : (opcode - op::kLoad);
        switch (index) {
            case 0: return execute_load(decoded, 8, false, displaced);  // load
            case 1: return execute_load(decoded, 1, false, displaced);  // load.zb
            case 2: return execute_load(decoded, 1, true, displaced);   // load.sb
            case 3: return execute_load(decoded, 2, false, displaced);  // load.zq
            case 4: return execute_load(decoded, 2, true, displaced);   // load.sq
            case 5: return execute_load(decoded, 4, false, displaced);  // load.zh
            case 6: return execute_load(decoded, 4, true, displaced);   // load.sh
            default: break;
        }
    }

    // Stores, bare $8E..$91 and displaced $92..$95. A store reads the low bytes of its source
    // at the named width, ignores the bits above it, and modifies no register at all.
    if (opcode >= op::kStore && opcode <= op::kStoreDisp + 3) {
        const bool displaced = opcode >= op::kStoreDisp;
        const unsigned index = displaced ? (opcode - op::kStoreDisp) : (opcode - op::kStore);
        switch (index) {
            case 0: return execute_store(decoded, 8, displaced);  // store
            case 1: return execute_store(decoded, 1, displaced);  // store.b
            case 2: return execute_store(decoded, 2, displaced);  // store.q
            case 3: return execute_store(decoded, 4, displaced);  // store.h
            default: break;
        }
    }

    // Extract, $A0..$A5. A dotted SOURCE extracts: the instruction produces a fresh full-width
    // value from the named element and creates no dependency on what the destination held.
    if (opcode >= op::kExtractZb && opcode <= op::kExtractSh) {
        const std::uint64_t source = registers_.read(decoded.reg[0]);
        const unsigned element = decoded.form[0];
        const unsigned destination = decoded.reg[1];
        std::uint64_t raw = 0;
        unsigned bits = 0;
        switch (opcode) {
            case op::kExtractZb:
            case op::kExtractSb:
                raw = byte_element(source, element);
                bits = 8;
                break;
            case op::kExtractZq:
            case op::kExtractSq:
                raw = quarter_element(source, element);
                bits = 16;
                break;
            default:
                raw = half_element(source, element);
                bits = 32;
                break;
        }
        const bool signed_form = (opcode == op::kExtractSb) || (opcode == op::kExtractSq) ||
                                 (opcode == op::kExtractSh);
        registers_.write(destination, signed_form ? sign_extend(raw, bits) : raw);
        return advance(decoded);
    }

    // Insert, $A6..$A8. A dotted DESTINATION inserts: read the destination, replace the named
    // element, write the whole register back. Both operands are read before the write, so
    // naming the same register in both slots is well defined.
    if (opcode >= op::kInsertB && opcode <= op::kInsertH) {
        const std::uint64_t source = registers_.read(decoded.reg[0]);
        const unsigned destination = decoded.reg[1];
        const unsigned element = decoded.form[1];
        const std::uint64_t target = registers_.read(destination);
        std::uint64_t merged = target;
        switch (opcode) {
            case op::kInsertB:
                merged = insert_byte(target, element, static_cast<std::uint8_t>(source));
                break;
            case op::kInsertQ:
                merged = insert_quarter(target, element, static_cast<std::uint16_t>(source));
                break;
            default:
                merged = insert_half(target, element, static_cast<std::uint32_t>(source));
                break;
        }
        registers_.write(destination, merged);
        return advance(decoded);
    }

    // The general bitfield instructions, $A9..$AB. Both immediates are unsigned 8-bit values,
    // the position first and the width second.
    if (opcode >= op::kBitfieldExtract && opcode <= op::kBitfieldInsert) {
        const unsigned position = static_cast<unsigned>(decoded.immediate[0] & 0xFF);
        const unsigned width = static_cast<unsigned>(decoded.immediate[1] & 0xFF);

        // Validate BEFORE reading or writing any register. A position of 64 or more is caught
        // by the same test, and neither condition has a defaulted interpretation.
        //
        // trap-model.md fixes the cause (1) and the subcode (1, an invalid immediate) but does
        // not say WHICH of the two immediates the auxiliary word carries when the pair is what
        // is wrong. This build reports the width when the width is zero, since the width alone
        // is the defect there, and the position otherwise, since the position is the value that
        // has to change to make the encoding legal for a given width. Recorded here as a
        // spec-silent choice rather than an inference from the text.
        if (width == 0) {
            return raise(decoded, cause::kIllegalOperand, subcode::kInvalidImmediate, width);
        }
        if (position + width > 64) {
            return raise(decoded, cause::kIllegalOperand, subcode::kInvalidImmediate, position);
        }

        const std::uint64_t source = registers_.read(decoded.reg[0]);
        const unsigned destination = decoded.reg[1];
        const std::uint64_t mask = field_mask(width);

        if (opcode == op::kBitfieldInsert) {
            const std::uint64_t field = source & mask;
            const std::uint64_t target = registers_.read(destination);
            registers_.write(destination, (target & ~(mask << position)) | (field << position));
            return advance(decoded);
        }

        const std::uint64_t field = (source >> position) & mask;
        registers_.write(destination, opcode == op::kBitfieldExtractSigned
                                          ? sign_extend(field, width)
                                          : field);
        return advance(decoded);
    }

    // Block memory, $B0..$B2.
    if (opcode >= op::kBlockCopy && opcode <= op::kBlockSet) {
        return execute_block(decoded);
    }

    // halt, $BD. The sole member of the system band this build executes (D-2). The
    // privileged-operation check is written out because the instruction has one, and it can
    // never fire here: reset is supervisor and the only path to user level is trap_return,
    // which is maize-420.
    if (opcode == op::kHalt) {
        if (privilege_ != Privilege::Supervisor) {
            return raise(decoded, cause::kPrivilegedOperation, 0, opcode);
        }
        pc_ = decoded.next_pc;
        halted_ = true;
        StepResult result;
        result.status = StepStatus::Halted;
        result.opcode = opcode;
        result.pc = decoded.pc;
        return result;
    }

    // port_in and port_out, $C2 and $C3 (maize-451). The port space is disjoint from memory and
    // these two instructions are the only way to reach it.
    //
    // Both are privileged, and the check is written out for the same reason kHalt's is: the
    // instruction has one, and it cannot fire in this build, because reset is supervisor and the
    // only path to user level is trap_return, which is maize-420. The auxiliary word of cause 4
    // is the offending opcode byte rather than a register number, since neither instruction
    // names a CSR.
    if (opcode == op::kPortIn || opcode == op::kPortOut) {
        if (privilege_ != Privilege::Supervisor) {
            return raise(decoded, cause::kPrivilegedOperation, 0, opcode);
        }
        if (opcode == op::kPortIn) {
            // port_in rp rd. The port identifier is the low quarter-word of the port register
            // and the upper 48 bits are ignored rather than checked, so a computed port number
            // pays for no range test.
            const std::uint16_t port =
                static_cast<std::uint16_t>(registers_.read(decoded.reg[0]));
            registers_.write(decoded.reg[1], devices_.port_in(port));
        } else {
            // port_out rs rp.
            const std::uint64_t value = registers_.read(decoded.reg[0]);
            const std::uint16_t port =
                static_cast<std::uint16_t>(registers_.read(decoded.reg[1]));
            devices_.port_out(port, value);
        }
        return advance(decoded);
    }

    // Everything left is a real assigned opcode whose family this build does not implement:
    // $B8..$C7 other than halt, port_in and port_out (maize-420), $C8..$F7 (maize-419), and $FF
    // breakpoint. The two
    // guard bytes of Appendix A.14 reach "does not execute" by two different and both explicit
    // routes: $00 because it is reserved and the decoder stops on it, $FF because it is
    // assigned and lands here.
    StepResult result;
    result.status = StepStatus::Unimplemented;
    result.opcode = opcode;
    result.pc = decoded.pc;
    return result;
}

}  // namespace maize::v2
