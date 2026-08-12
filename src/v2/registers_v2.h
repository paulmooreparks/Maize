// registers_v2.h (maize-418): the v2 general register file.
//
// Thirty-two 64-bit registers, r0 hardwired to zero. register-model.md makes r0's behaviour a
// read rule and a write rule rather than a special case any instruction has to know about: r0
// reads as zero everywhere and discards every write, and EVERY other effect of an instruction
// naming r0 still happens. A `load @r9 r0` performs the access and raises whatever fault the
// access raises; a `divide_signed r4 r5 r0` still traps when r5 is zero. Routing every operand
// read through read() and every destination write through write() is what makes that true by
// construction rather than by remembering it at 180 call sites.
//
// The fourteen positional names (rN.b0 through rN.b7, rN.q0 through rN.q3, rN.h0 and rN.h1)
// are bit-range views computed from the element index the operand byte's form field carries.
// There is no sub-register file and no separate storage, so they live here as free functions
// over a 64-bit value rather than as accessors on the file.

#ifndef MAIZE_V2_REGISTERS_V2_H
#define MAIZE_V2_REGISTERS_V2_H

#include <array>
#include <cstdint>

namespace maize::v2 {

inline constexpr unsigned kRegisterCount = 32;
inline constexpr unsigned kLinkRegister = 31;  // a call/return convention, not a hardwired rule

class RegistersV2 {
  public:
    std::uint64_t read(unsigned n) const {
        return n == 0 ? 0u : file_[n];
    }

    void write(unsigned n, std::uint64_t value) {
        if (n != 0) {
            file_[n] = value;
        }
    }

    // Raw access for a host that is setting up or inspecting a machine (a fixture harness, the
    // mzvm entry point). Guest execution never uses these, because they do not honour r0.
    std::uint64_t raw(unsigned n) const { return file_[n]; }
    void set_raw(unsigned n, std::uint64_t value) { file_[n] = value; }
    void reset() { file_.fill(0); }

  private:
    // boot.md: every general register holds zero when the first instruction executes, r30 and
    // r31 included, because the stack-pointer and link-register roles are calling-convention
    // roles that no hardware mechanism establishes.
    std::array<std::uint64_t, kRegisterCount> file_{};
};

// Positional views. Indices count from the least significant end, matching the little-endian
// byte order, per register-model.md's table.
constexpr std::uint8_t byte_element(std::uint64_t value, unsigned index) {
    return static_cast<std::uint8_t>(value >> (index * 8));
}

constexpr std::uint16_t quarter_element(std::uint64_t value, unsigned index) {
    return static_cast<std::uint16_t>(value >> (index * 16));
}

constexpr std::uint32_t half_element(std::uint64_t value, unsigned index) {
    return static_cast<std::uint32_t>(value >> (index * 32));
}

constexpr std::uint64_t insert_byte(std::uint64_t target, unsigned index, std::uint8_t value) {
    const unsigned shift = index * 8;
    const std::uint64_t mask = std::uint64_t{0xFF} << shift;
    return (target & ~mask) | (static_cast<std::uint64_t>(value) << shift);
}

constexpr std::uint64_t insert_quarter(std::uint64_t target, unsigned index,
                                       std::uint16_t value) {
    const unsigned shift = index * 16;
    const std::uint64_t mask = std::uint64_t{0xFFFF} << shift;
    return (target & ~mask) | (static_cast<std::uint64_t>(value) << shift);
}

constexpr std::uint64_t insert_half(std::uint64_t target, unsigned index, std::uint32_t value) {
    const unsigned shift = index * 32;
    const std::uint64_t mask = std::uint64_t{0xFFFFFFFF} << shift;
    return (target & ~mask) | (static_cast<std::uint64_t>(value) << shift);
}

// Extension helpers. Every narrow value in the base is either zero-extended or sign-extended
// into the full 64-bit register, and the choice is a property of the opcode.
constexpr std::uint64_t sign_extend(std::uint64_t value, unsigned bits) {
    if (bits >= 64) {
        return value;
    }
    const std::uint64_t sign = std::uint64_t{1} << (bits - 1);
    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
    const std::uint64_t truncated = value & mask;
    return (truncated ^ sign) - sign;
}

constexpr std::uint64_t zero_extend(std::uint64_t value, unsigned bits) {
    if (bits >= 64) {
        return value;
    }
    return value & ((std::uint64_t{1} << bits) - 1);
}

}  // namespace maize::v2

#endif  // MAIZE_V2_REGISTERS_V2_H
