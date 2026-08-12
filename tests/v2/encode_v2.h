// encode_v2.h (maize-418): the fixture byte emitter.
//
// mazm v2 does not exist yet, so every fixture in this suite is hand-assembled bytes. Writing
// those as inline byte literals would put a form-field mistake exactly where a reviewer cannot
// see it, so the bytes come out of this instead: one function per length class of
// instruction-encoding.md, named after the shape string the appendix uses.
//
// This is deliberately NOT a step toward the real assembler. There is no label resolution, no
// parsing, no mnemonic table, and no operand-order knowledge: a caller passes the opcode byte
// from opcode_v2.h and the operands in encoded order. What the emitter DOES know is the shape
// each opcode declares, and it refuses a call whose shape does not match, so a fixture that
// reaches for the wrong length class fails at the point of the mistake instead of decoding
// into something plausible.
//
// When mazm v2 lands (maize-422) the fixtures keep working: they assert on machine state and
// trap records rather than on assembly text, so a later pass can add an assembled counterpart
// beside a byte-level one without discarding either.

#ifndef MAIZE_V2_TESTS_ENCODE_V2_H
#define MAIZE_V2_TESTS_ENCODE_V2_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "opcode_v2.h"

namespace maize::v2::test {

// One encoded operand: a register number and a form field. A plain slot wants form 0, and a
// sliced slot carries its element index here.
struct Operand {
    std::uint8_t number = 0;
    std::uint8_t form = 0;
};

inline Operand reg(unsigned number) {
    return Operand{static_cast<std::uint8_t>(number), 0};
}

// A sliced operand: rN.b5 is slice(3, 5) over a byte-sliced opcode. This is also how a fixture
// spells a DELIBERATELY malformed operand byte, by putting a nonzero form on a plain slot or an
// out-of-range element index on a sliced one.
inline Operand slice(unsigned number, unsigned element) {
    return Operand{static_cast<std::uint8_t>(number), static_cast<std::uint8_t>(element)};
}

class Encoder {
  public:
    explicit Encoder(std::uint64_t base_address = 0) : base_address_(base_address) {}

    const std::vector<std::uint8_t>& bytes() const { return bytes_; }
    std::uint64_t base_address() const { return base_address_; }

    // The address the NEXT emitted instruction will occupy. Branch and call fixtures compute
    // their displacements from this and from a recorded earlier value, which is arithmetic a
    // reviewer can check against the encoding chapter rather than a label mechanism.
    std::uint64_t current_address() const {
        return base_address_ + static_cast<std::uint64_t>(bytes_.size());
    }

    // Raw bytes, for a fixture that is deliberately encoding something the table calls
    // reserved, or for filling data into the image.
    Encoder& raw(std::initializer_list<std::uint8_t> values) {
        for (std::uint8_t value : values) {
            bytes_.push_back(value);
        }
        return *this;
    }

    Encoder& raw_byte(std::uint8_t value) {
        bytes_.push_back(value);
        return *this;
    }

    Encoder& op(std::uint8_t opcode) {
        begin(opcode, Shape::Op);
        return *this;
    }

    Encoder& op_r(std::uint8_t opcode, Operand a) {
        begin(opcode, Shape::OpR);
        operand(a);
        return *this;
    }

    Encoder& op_r_r(std::uint8_t opcode, Operand a, Operand b) {
        begin(opcode, Shape::OpRR);
        operand(a);
        operand(b);
        return *this;
    }

    Encoder& op_r_r_r(std::uint8_t opcode, Operand a, Operand b, Operand c) {
        begin(opcode, Shape::OpRRR);
        operand(a);
        operand(b);
        operand(c);
        return *this;
    }

    Encoder& op_r_r_r_r(std::uint8_t opcode, Operand a, Operand b, Operand c, Operand d) {
        begin(opcode, Shape::OpRRRR);
        operand(a);
        operand(b);
        operand(c);
        operand(d);
        return *this;
    }

    Encoder& op_i1(std::uint8_t opcode, std::uint64_t immediate) {
        begin(opcode, Shape::OpI1);
        little_endian(immediate, 1);
        return *this;
    }

    Encoder& op_i4(std::uint8_t opcode, std::uint64_t immediate) {
        begin(opcode, Shape::OpI4);
        little_endian(immediate, 4);
        return *this;
    }

    Encoder& op_r_i1(std::uint8_t opcode, Operand a, std::uint64_t immediate) {
        begin(opcode, Shape::OpRI1);
        operand(a);
        little_endian(immediate, 1);
        return *this;
    }

    Encoder& op_r_i2(std::uint8_t opcode, Operand a, std::uint64_t immediate) {
        begin(opcode, Shape::OpRI2);
        operand(a);
        little_endian(immediate, 2);
        return *this;
    }

    Encoder& op_r_i4(std::uint8_t opcode, Operand a, std::uint64_t immediate) {
        begin(opcode, Shape::OpRI4);
        operand(a);
        little_endian(immediate, 4);
        return *this;
    }

    Encoder& op_r_i8(std::uint8_t opcode, Operand a, std::uint64_t immediate) {
        begin(opcode, Shape::OpRI8);
        operand(a);
        little_endian(immediate, 8);
        return *this;
    }

    Encoder& op_r_r_i1(std::uint8_t opcode, Operand a, Operand b, std::uint64_t immediate) {
        begin(opcode, Shape::OpRRI1);
        operand(a);
        operand(b);
        little_endian(immediate, 1);
        return *this;
    }

    Encoder& op_r_r_i2(std::uint8_t opcode, Operand a, Operand b, std::uint64_t immediate) {
        begin(opcode, Shape::OpRRI2);
        operand(a);
        operand(b);
        little_endian(immediate, 2);
        return *this;
    }

    Encoder& op_r_r_i4(std::uint8_t opcode, Operand a, Operand b, std::uint64_t immediate) {
        begin(opcode, Shape::OpRRI4);
        operand(a);
        operand(b);
        little_endian(immediate, 4);
        return *this;
    }

    Encoder& op_r_r_i1_i1(std::uint8_t opcode, Operand a, Operand b, std::uint64_t first,
                          std::uint64_t second) {
        begin(opcode, Shape::OpRRI1I1);
        operand(a);
        operand(b);
        little_endian(first, 1);
        little_endian(second, 1);
        return *this;
    }

    // The one-byte instruction every fixture ends with, so the machine stops somewhere the
    // fixture chose rather than running off into whatever follows the image.
    Encoder& halt() { return op(op::kHalt); }

  private:
    void begin(std::uint8_t opcode, Shape expected) {
        const OpcodeInfo& info = kOpcodeTable[opcode];
        if (info.kind != OpcodeKind::Assigned || info.shape != expected) {
            std::fprintf(stderr,
                         "encode_v2: opcode $%02X does not have the length class this call "
                         "emits; use raw() if the malformed encoding is the point\n",
                         opcode);
            std::abort();
        }
        bytes_.push_back(opcode);
    }

    void operand(Operand value) {
        if (value.number > 31 || value.form > 7) {
            std::fprintf(stderr, "encode_v2: operand r%u form %u does not fit an operand byte\n",
                         value.number, value.form);
            std::abort();
        }
        bytes_.push_back(static_cast<std::uint8_t>((value.form << 5) | value.number));
    }

    void little_endian(std::uint64_t value, unsigned width_bytes) {
        for (unsigned i = 0; i < width_bytes; ++i) {
            bytes_.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
        }
    }

    std::vector<std::uint8_t> bytes_;
    std::uint64_t base_address_ = 0;
};

}  // namespace maize::v2::test

#endif  // MAIZE_V2_TESTS_ENCODE_V2_H
