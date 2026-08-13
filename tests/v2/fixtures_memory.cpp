// fixtures_memory.cpp (maize-418): loads and stores, extract and insert, bitfield, block memory.

#include <cstdio>
#include <vector>

#include "fixture_support.h"

namespace maize::v2::test {
namespace {

constexpr std::uint64_t kBase = 0x100;
constexpr std::uint64_t kSentinel = 0x0123456789ABCDEFull;
constexpr std::uint64_t kData = 0x180;

void fill_pattern(MemoryV2& memory, std::uint64_t address, std::uint64_t length,
                  std::uint8_t seed) {
    for (std::uint64_t i = 0; i < length; ++i) {
        memory.write_byte(address + i, static_cast<std::uint8_t>(seed + i * 7));
    }
}

std::vector<std::uint8_t> snapshot(const MemoryV2& memory, std::uint64_t address,
                                   std::uint64_t length) {
    std::vector<std::uint8_t> bytes;
    for (std::uint64_t i = 0; i < length; ++i) {
        bytes.push_back(memory.read_byte(address + i));
    }
    return bytes;
}

}  // namespace

V2_FIXTURE(loads_and_stores_at_every_width) {
    // Memory is little-endian at every width, so the lowest address of a multi-byte access
    // holds the least significant byte, and a load writes the FULL destination register with
    // the extension rule the mnemonic names.
    struct Load {
        const char* what;
        std::uint8_t opcode;
        std::uint64_t expected;
    };

    const Load loads[] = {
        {"load", op::kLoad, 0xF0DEBC9A78563412ull},
        {"load.zb", op::kLoadZb, 0x0000000000000012ull},
        {"load.sb", op::kLoadSb, 0x0000000000000012ull},
        {"load.zq", op::kLoadZq, 0x0000000000003412ull},
        {"load.sq", op::kLoadSq, 0x0000000000003412ull},
        {"load.zh", op::kLoadZh, 0x0000000078563412ull},
        {"load.sh", op::kLoadSh, 0x0000000078563412ull},
    };

    for (const Load& one : loads) {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r(one.opcode, reg(9), reg(4)).halt();
        machine.load(program);
        const std::uint8_t bytes[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
        for (unsigned i = 0; i < 8; ++i) {
            machine.memory().write_byte(kData + i, bytes[i]);
        }
        machine.set(9, kData);
        machine.set(4, kSentinel);
        expect_halted(machine.run(), one.what);
        check_equal_u64(machine.get(4), one.expected, one.what, __FILE__, __LINE__);
    }

    // The sign-extending narrow loads, on a value whose top bit is set at each width.
    struct SignedLoad {
        const char* what;
        std::uint8_t opcode;
        std::uint64_t expected;
    };
    const SignedLoad signed_loads[] = {
        {"load.sb of $FF", op::kLoadSb, 0xFFFFFFFFFFFFFFFFull},
        {"load.sq of $FFFF", op::kLoadSq, 0xFFFFFFFFFFFFFFFFull},
        {"load.sh of $FFFFFFFF", op::kLoadSh, 0xFFFFFFFFFFFFFFFFull},
        {"load.zb of $FF", op::kLoadZb, 0x00000000000000FFull},
        {"load.zq of $FFFF", op::kLoadZq, 0x000000000000FFFFull},
        {"load.zh of $FFFFFFFF", op::kLoadZh, 0x00000000FFFFFFFFull},
    };
    for (const SignedLoad& one : signed_loads) {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r(one.opcode, reg(9), reg(4)).halt();
        machine.load(program);
        for (unsigned i = 0; i < 8; ++i) {
            machine.memory().write_byte(kData + i, 0xFF);
        }
        machine.set(9, kData);
        expect_halted(machine.run(), one.what);
        check_equal_u64(machine.get(4), one.expected, one.what, __FILE__, __LINE__);
    }

    // The displaced form is the bare form with a signed 16-bit displacement, and the assembler
    // spelling @rN-$disp is the negation of the written value.
    {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r_i2(op::kLoadDisp, reg(9), reg(4), 0x20)
            .op_r_r_i2(op::kLoadDisp, reg(9), reg(5), 0xFFF8ull)  // a displacement of -8
            .halt();
        machine.load(program);
        machine.memory().write_little_endian(kData + 0x20, 8, 0x1122334455667788ull);
        machine.memory().write_little_endian(kData - 8, 8, 0x99AABBCCDDEEFF00ull);
        machine.set(9, kData);
        expect_halted(machine.run(), "displaced loads");
        V2_CHECK_EQ(machine.get(4), 0x1122334455667788ull);
        V2_CHECK_EQ(machine.get(5), 0x99AABBCCDDEEFF00ull);
    }

    // A store reads the low bytes of its source at the named width, ignores the bits above it,
    // and modifies no register at all.
    struct Store {
        const char* what;
        std::uint8_t opcode;
        unsigned width;
    };
    const Store stores[] = {
        {"store", op::kStore, 8},
        {"store.b", op::kStoreB, 1},
        {"store.q", op::kStoreQ, 2},
        {"store.h", op::kStoreH, 4},
    };
    for (const Store& one : stores) {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r(one.opcode, reg(5), reg(9)).halt();
        machine.load(program);
        machine.set(5, 0x1122334455667788ull);
        machine.set(9, kData);
        expect_halted(machine.run(), one.what);
        for (unsigned i = 0; i < 8; ++i) {
            const std::uint8_t expected =
                (i < one.width) ? static_cast<std::uint8_t>(0x1122334455667788ull >> (i * 8)) : 0;
            check_equal_u64(machine.memory().read_byte(kData + i), expected, one.what, __FILE__,
                            __LINE__);
        }
        V2_CHECK_EQ(machine.get(5), 0x1122334455667788ull);
        V2_CHECK_EQ(machine.get(9), kData);
    }

    // When rd and rb name the same register, the loaded value replaces the address after the
    // access completes.
    {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r(op::kLoad, reg(9), reg(9)).halt();
        machine.load(program);
        machine.memory().write_little_endian(kData, 8, kSentinel);
        machine.set(9, kData);
        expect_halted(machine.run(), "load @r9 r9");
        V2_CHECK_EQ(machine.get(9), kSentinel);
    }
}

V2_FIXTURE(memory_faults_report_the_lowest_inaccessible_address) {
    // An access that spans the populated-memory boundary is ONE access for fault purposes, and
    // it reports the lowest inaccessible address the access covers, which makes the reported
    // address a function of the access alone and not of the order an implementation touches
    // bytes in.
    const std::size_t size = 0x200;

    {
        Machine machine(size);
        Encoder program(kBase);
        program.op_r_r(op::kLoad, reg(9), reg(4)).halt();
        machine.load(program);
        machine.set(9, 0x1FC);  // eight bytes from $1FC covers $1FC through $203
        machine.set(4, kSentinel);
        const StepResult result = machine.step();
        expect_trap(result, cause::kPhysicalMemoryFault, 0, 0x200, kBase,
                    "a load spanning the populated-memory boundary");
        // Trap-writes-nothing: the destination register is untouched.
        V2_CHECK_EQ(machine.get(4), kSentinel);
    }

    // A load naming r0 as its destination raises the SAME fault at the SAME address that the
    // identical load into r1 raises, because r0 discards the write and suppresses nothing else.
    {
        Machine into_r0(size);
        Encoder program(kBase);
        program.op_r_r(op::kLoad, reg(9), reg(0)).halt();
        into_r0.load(program);
        into_r0.set(9, 0x1FC);
        const StepResult zero_result = into_r0.step();

        Machine into_r1(size);
        Encoder other(kBase);
        other.op_r_r(op::kLoad, reg(9), reg(1)).halt();
        into_r1.load(other);
        into_r1.set(9, 0x1FC);
        const StepResult one_result = into_r1.step();

        expect_trap(zero_result, cause::kPhysicalMemoryFault, 0, 0x200, kBase, "load into r0");
        expect_trap(one_result, cause::kPhysicalMemoryFault, 0, 0x200, kBase, "load into r1");
        V2_CHECK(zero_result.trap.cause == one_result.trap.cause);
        V2_CHECK(zero_result.trap.aux == one_result.trap.aux);
    }
}

V2_FIXTURE(a_trapping_access_writes_nothing) {
    // The ordering probe. An implementation that writes the accessible part of an access and
    // only then discovers the inaccessible part passes every test that does not arrange this
    // exact shape, and fails this one.
    const std::size_t size = 0x200;

    // A store whose first four bytes are inside populated memory and whose last four are not
    // must leave EVERY byte of memory unmodified, the accessible ones included.
    {
        Machine machine(size);
        Encoder program(kBase);
        program.op_r_r(op::kStore, reg(5), reg(9)).halt();
        machine.load(program);
        fill_pattern(machine.memory(), 0x1F0, 0x10, 0xA0);
        const std::vector<std::uint8_t> before = snapshot(machine.memory(), 0x1F0, 0x10);
        machine.set(5, 0xFFFFFFFFFFFFFFFFull);
        machine.set(9, 0x1FC);
        const StepResult result = machine.step();
        expect_trap(result, cause::kPhysicalMemoryFault, 0, 0x200, kBase,
                    "a store spanning the populated-memory boundary");
        const std::vector<std::uint8_t> after = snapshot(machine.memory(), 0x1F0, 0x10);
        V2_CHECK(before == after);
    }

    // The same shape one byte at a time, so the boundary itself is exercised: an access that
    // ends exactly at the last accessible byte succeeds, and one that runs a single byte past
    // it fails and changes nothing.
    for (unsigned offset = 0; offset <= 1; ++offset) {
        Machine machine(size);
        Encoder program(kBase);
        program.op_r_r(op::kStore, reg(5), reg(9)).halt();
        machine.load(program);
        const std::uint64_t address = 0x1F8 + offset;
        fill_pattern(machine.memory(), 0x1F0, 0x10, 0x40);
        const std::vector<std::uint8_t> before = snapshot(machine.memory(), 0x1F0, 0x10);
        machine.set(5, kSentinel);
        machine.set(9, address);
        const StepResult result = machine.step();
        if (offset == 0) {
            V2_CHECK(result.status == StepStatus::Advanced);
            V2_CHECK_EQ(machine.memory().read_little_endian(address, 8), kSentinel);
        } else {
            expect_trap(result, cause::kPhysicalMemoryFault, 0, 0x200, kBase,
                        "a store one byte past the boundary");
            V2_CHECK(before == snapshot(machine.memory(), 0x1F0, 0x10));
        }
    }

    // A bitfield instruction that traps modifies no register either, which is the same contract
    // reached through an immediate check rather than an address check.
    {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r_i1_i1(op::kBitfieldInsert, reg(5), reg(6), 60, 8).halt();
        machine.load(program);
        machine.set(5, 0xFFFFFFFFFFFFFFFFull);
        machine.set(6, kSentinel);
        const StepResult result = machine.step();
        expect_trap(result, cause::kIllegalOperand, subcode::kInvalidImmediate, 60, kBase,
                    "bitfield_insert with position 60 and width 8");
        V2_CHECK_EQ(machine.get(6), kSentinel);
        V2_CHECK_EQ(machine.get(5), 0xFFFFFFFFFFFFFFFFull);
    }
}

V2_FIXTURE(positional_extract_and_insert) {
    // All fourteen positional forms are reachable, each by exactly one encoding, and each
    // selects the bit range the register model tabulates. Indices count from the least
    // significant end.
    //
    // Every expected value below is a LITERAL, written out by hand from register-model.md's
    // table against the one source word. Nothing here recomputes an expectation with the
    // production helpers (byte_element, insert_byte, sign_extend and the rest), because an
    // expectation computed by the code under test agrees with that code however wrong both are:
    // an off-by-one in a shift inside a helper would have matched itself.
    //
    // The source is $8070605040302010, whose bytes from the least significant end are $10, $20,
    // $30, $40, $50, $60, $70, $80; whose quarter-words are $2010, $4030, $6050, $8070; and
    // whose half-words are $40302010 and $80706050. Only the topmost element at each width has
    // its sign bit set, which is what makes the .s forms distinguishable from the .z forms.
    const std::uint64_t source = 0x8070605040302010ull;

    const std::uint64_t extract_zb[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    const std::uint64_t extract_sb[8] = {0x10, 0x20, 0x30, 0x40,
                                         0x50, 0x60, 0x70, 0xFFFFFFFFFFFFFF80ull};
    for (unsigned element = 0; element < 8; ++element) {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r(op::kExtractZb, slice(3, element), reg(7))
            .op_r_r(op::kExtractSb, slice(3, element), reg(8))
            .halt();
        machine.load(program);
        machine.set(3, source);
        expect_halted(machine.run(), "extract.zb and extract.sb");
        char label[96];
        std::snprintf(label, sizeof(label), "extract.zb r3.b%u", element);
        check_equal_u64(machine.get(7), extract_zb[element], label, __FILE__, __LINE__);
        std::snprintf(label, sizeof(label), "extract.sb r3.b%u", element);
        check_equal_u64(machine.get(8), extract_sb[element], label, __FILE__, __LINE__);
    }

    const std::uint64_t extract_zq[4] = {0x2010, 0x4030, 0x6050, 0x8070};
    const std::uint64_t extract_sq[4] = {0x2010, 0x4030, 0x6050, 0xFFFFFFFFFFFF8070ull};
    for (unsigned element = 0; element < 4; ++element) {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r(op::kExtractZq, slice(3, element), reg(7))
            .op_r_r(op::kExtractSq, slice(3, element), reg(8))
            .halt();
        machine.load(program);
        machine.set(3, source);
        expect_halted(machine.run(), "extract.zq and extract.sq");
        char label[96];
        std::snprintf(label, sizeof(label), "extract.zq r3.q%u", element);
        check_equal_u64(machine.get(7), extract_zq[element], label, __FILE__, __LINE__);
        std::snprintf(label, sizeof(label), "extract.sq r3.q%u", element);
        check_equal_u64(machine.get(8), extract_sq[element], label, __FILE__, __LINE__);
    }

    const std::uint64_t extract_zh[2] = {0x40302010ull, 0x80706050ull};
    const std::uint64_t extract_sh[2] = {0x40302010ull, 0xFFFFFFFF80706050ull};
    for (unsigned element = 0; element < 2; ++element) {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r(op::kExtractZh, slice(3, element), reg(7))
            .op_r_r(op::kExtractSh, slice(3, element), reg(8))
            .halt();
        machine.load(program);
        machine.set(3, source);
        expect_halted(machine.run(), "extract.zh and extract.sh");
        char label[96];
        std::snprintf(label, sizeof(label), "extract.zh r3.h%u", element);
        check_equal_u64(machine.get(7), extract_zh[element], label, __FILE__, __LINE__);
        std::snprintf(label, sizeof(label), "extract.sh r3.h%u", element);
        check_equal_u64(machine.get(8), extract_sh[element], label, __FILE__, __LINE__);
    }

    // Insert preserves every bit outside the named element, which is the only merge site in the
    // instruction set. The source register carries $AA in its low byte, $BBAA in its low
    // quarter-word and $DDCCBBAA in its low half-word, with every higher bit set, so a form
    // that failed to ignore the bits above its width would show at once.
    const std::uint64_t insert_b[8] = {
        0x80706050403020AAull, 0x807060504030AA10ull, 0x8070605040AA2010ull,
        0x80706050AA302010ull, 0x807060AA40302010ull, 0x8070AA5040302010ull,
        0x80AA605040302010ull, 0xAA70605040302010ull,
    };
    for (unsigned element = 0; element < 8; ++element) {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r(op::kInsertB, reg(4), slice(3, element)).halt();
        machine.load(program);
        machine.set(3, source);
        machine.set(4, 0xFFFFFFFFFFFFFFAAull);
        expect_halted(machine.run(), "insert.b");
        char label[96];
        std::snprintf(label, sizeof(label), "insert.b r4 r3.b%u", element);
        check_equal_u64(machine.get(3), insert_b[element], label, __FILE__, __LINE__);
    }

    const std::uint64_t insert_q[4] = {
        0x807060504030BBAAull, 0x80706050BBAA2010ull, 0x8070BBAA40302010ull,
        0xBBAA605040302010ull,
    };
    for (unsigned element = 0; element < 4; ++element) {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r(op::kInsertQ, reg(4), slice(3, element)).halt();
        machine.load(program);
        machine.set(3, source);
        machine.set(4, 0xFFFFFFFFFFFFBBAAull);
        expect_halted(machine.run(), "insert.q");
        char label[96];
        std::snprintf(label, sizeof(label), "insert.q r4 r3.q%u", element);
        check_equal_u64(machine.get(3), insert_q[element], label, __FILE__, __LINE__);
    }

    const std::uint64_t insert_h[2] = {0x80706050DDCCBBAAull, 0xDDCCBBAA40302010ull};
    for (unsigned element = 0; element < 2; ++element) {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r(op::kInsertH, reg(4), slice(3, element)).halt();
        machine.load(program);
        machine.set(3, source);
        machine.set(4, 0xFFFFFFFFDDCCBBAAull);
        expect_halted(machine.run(), "insert.h");
        char label[96];
        std::snprintf(label, sizeof(label), "insert.h r4 r3.h%u", element);
        check_equal_u64(machine.get(3), insert_h[element], label, __FILE__, __LINE__);
    }

    // Naming the same register in both slots is well defined: the source value is read before
    // the destination is written.
    {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r(op::kInsertB, reg(5), slice(5, 3)).halt();
        machine.load(program);
        machine.set(5, 0x00000000000000AAull);
        expect_halted(machine.run(), "insert.b r5 r5.b3");
        V2_CHECK_EQ(machine.get(5), 0x00000000AA0000AAull);
    }

    // A dotted name over r0 obeys the zero register rather than the notation.
    {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r(op::kExtractZb, slice(0, 5), reg(7))
            .op_r_r(op::kInsertB, reg(4), slice(0, 5))
            .halt();
        machine.load(program);
        machine.set(4, 0xFF);
        expect_halted(machine.run(), "extract and insert over r0");
        V2_CHECK_EQ(machine.get(7), 0u);
        V2_CHECK_EQ(machine.get(0), 0u);
    }
}

V2_FIXTURE(bitfield_extract_and_insert) {
    // The reference chapter's own worked example: the five bits starting at bit 12 of r5.
    {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r_i1_i1(op::kBitfieldExtract, reg(5), reg(6), 12, 5)
            .op_r_r_i1_i1(op::kBitfieldExtractSigned, reg(5), reg(7), 12, 5)
            .halt();
        machine.load(program);
        machine.set(5, 0x000000000001F000ull);  // bits 16 through 12 all set
        expect_halted(machine.run(), "bitfield_extract r5 #12 #5");
        V2_CHECK_EQ(machine.get(6), 0x1Full);
        V2_CHECK_EQ(machine.get(7), 0xFFFFFFFFFFFFFFFFull);  // -1 as a signed five-bit field
    }

    // A position of 0 with a width of 64 copies the whole register, and the signed form has
    // nothing left to extend.
    {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r_i1_i1(op::kBitfieldExtract, reg(5), reg(6), 0, 64)
            .op_r_r_i1_i1(op::kBitfieldExtractSigned, reg(5), reg(7), 0, 64)
            .halt();
        machine.load(program);
        machine.set(5, 0x8000000000000001ull);
        expect_halted(machine.run(), "bitfield_extract of the whole register");
        V2_CHECK_EQ(machine.get(6), 0x8000000000000001ull);
        V2_CHECK_EQ(machine.get(7), 0x8000000000000001ull);
    }

    // bitfield_insert ignores bits of the source at or above the width, so a caller need not
    // mask its source, and it preserves every other bit of the destination.
    {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r_i1_i1(op::kBitfieldInsert, reg(6), reg(5), 12, 5).halt();
        machine.load(program);
        machine.set(6, 0xFFFFFFFFFFFFFFFFull);
        machine.set(5, 0);
        expect_halted(machine.run(), "bitfield_insert r6 #12 #5 r5");
        V2_CHECK_EQ(machine.get(5), 0x000000000001F000ull);
    }

    // A width of zero, and a position plus width above 64, both raise the illegal-operand trap
    // and modify no register. A position of 64 or more is caught by the same test.
    struct Bad {
        const char* what;
        std::uint8_t opcode;
        unsigned position;
        unsigned width;
        std::uint64_t aux;
    };
    const Bad bad[] = {
        {"bitfield_extract width 0", op::kBitfieldExtract, 0, 0, 0},
        {"bitfield_extract_signed width 0", op::kBitfieldExtractSigned, 12, 0, 0},
        {"bitfield_insert width 0", op::kBitfieldInsert, 12, 0, 0},
        {"bitfield_extract position 1 width 64", op::kBitfieldExtract, 1, 64, 1},
        {"bitfield_extract position 64 width 1", op::kBitfieldExtract, 64, 1, 64},
        {"bitfield_insert position 255 width 255", op::kBitfieldInsert, 255, 255, 255},
    };
    for (const Bad& one : bad) {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r_i1_i1(one.opcode, reg(5), reg(6), one.position, one.width).halt();
        machine.load(program);
        machine.set(5, kSentinel);
        machine.set(6, kSentinel);
        const StepResult result = machine.step();
        expect_trap(result, cause::kIllegalOperand, subcode::kInvalidImmediate, one.aux, kBase,
                    one.what);
        check_equal_u64(machine.get(5), kSentinel, one.what, __FILE__, __LINE__);
        check_equal_u64(machine.get(6), kSentinel, one.what, __FILE__, __LINE__);
    }
}

V2_FIXTURE(block_memory_operand_validity) {
    // Both rules constrain ENCODINGS rather than values, so they are decided before any byte is
    // transferred. Each case below arranges live pointers and a nonzero count, so an
    // implementation that checked after transferring would be caught by the memory contents as
    // well as by the trap.
    struct Case {
        const char* what;
        std::uint8_t opcode;
        unsigned a;
        unsigned b;
        unsigned c;
        std::uint64_t aux;
    };

    const Case cases[] = {
        {"block_copy with the first two slots aliased", op::kBlockCopy, 4, 4, 6, 4},
        {"block_copy with the outer slots aliased", op::kBlockCopy, 4, 5, 4, 4},
        {"block_copy with the last two slots aliased", op::kBlockCopy, 4, 5, 5, 5},
        {"block_copy with r0 as the source pointer", op::kBlockCopy, 0, 5, 6, 0},
        {"block_copy with r0 as the destination pointer", op::kBlockCopy, 4, 0, 6, 0},
        {"block_copy with r0 as the count", op::kBlockCopy, 4, 5, 0, 0},
        {"block_copy_forward with r0 as the count", op::kBlockCopyForward, 4, 5, 0, 0},
        {"block_set with r0 as the destination pointer", op::kBlockSet, 4, 0, 6, 0},
        {"block_set with r0 as the count", op::kBlockSet, 4, 5, 0, 0},
        {"block_set with the pointer and count aliased", op::kBlockSet, 4, 5, 5, 5},
    };

    for (const Case& one : cases) {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r_r(one.opcode, reg(one.a), reg(one.b), reg(one.c)).halt();
        machine.load(program);
        fill_pattern(machine.memory(), kData, 0x40, 0x10);
        const std::vector<std::uint8_t> before = snapshot(machine.memory(), kData, 0x40);
        machine.set(4, kData);
        machine.set(5, kData + 0x20);
        machine.set(6, 0x10);
        const StepResult result = machine.step();
        expect_trap(result, cause::kIllegalOperand, subcode::kBlockMemoryOperands, one.aux, kBase,
                    one.what);
        if (before != snapshot(machine.memory(), kData, 0x40)) {
            record_failure(std::string(one.what) + ": bytes were transferred before the trap");
        }
    }

    // block_set with r0 in the VALUE slot executes normally as a zero fill, because that
    // register carries no operation state and is never written.
    {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r_r(op::kBlockSet, reg(0), reg(5), reg(6)).halt();
        machine.load(program);
        fill_pattern(machine.memory(), kData, 0x20, 0x55);
        machine.set(5, kData);
        machine.set(6, 0x10);
        expect_halted(machine.run(), "block_set r0 @r5 r6");
        for (unsigned i = 0; i < 0x10; ++i) {
            V2_CHECK_EQ(machine.memory().read_byte(kData + i), 0u);
        }
        V2_CHECK_EQ(machine.memory().read_byte(kData + 0x10), (0x55u + 0x10u * 7u) & 0xFFu);
        V2_CHECK_EQ(machine.get(5), kData + 0x10);
        V2_CHECK_EQ(machine.get(6), 0u);
    }
}

V2_FIXTURE(block_memory_completion_and_overlap) {
    // On normal completion the count register holds zero and each pointer register holds its
    // original value plus the ORIGINAL count, for both copies and regardless of the direction
    // the implementation travelled. block_copy chooses its direction per overlap, so this
    // matrix runs it both ways.
    const std::int64_t offsets[] = {-40, -16, -1, 0, 1, 16, 40};
    constexpr std::uint64_t kCount = 16;
    constexpr std::uint64_t kRegion = 0x200;

    for (std::int64_t offset : offsets) {
        for (int forward = 0; forward < 2; ++forward) {
            const std::uint8_t opcode = forward ? op::kBlockCopyForward : op::kBlockCopy;
            Machine machine(0x400);
            Encoder program(kBase);
            // The instruction appears TWICE. Executing it a second time from the completion
            // state has to change nothing, since a count of zero transfers nothing, and running
            // both from one program is how the fixture checks that without restarting a machine
            // that has already halted.
            program.op_r_r_r(opcode, reg(4), reg(5), reg(6));
            program.op_r_r_r(opcode, reg(4), reg(5), reg(6));
            program.halt();
            machine.load(program);

            const std::uint64_t source = kRegion;
            const std::uint64_t destination =
                static_cast<std::uint64_t>(static_cast<std::int64_t>(kRegion) + offset);
            fill_pattern(machine.memory(), kRegion - 64, 192, 0x03);
            const std::vector<std::uint8_t> before =
                snapshot(machine.memory(), kRegion - 64, 192);

            machine.set(4, source);
            machine.set(5, destination);
            machine.set(6, kCount);

            char label[128];
            std::snprintf(label, sizeof(label), "%s at offset %lld",
                          forward ? "block_copy_forward" : "block_copy",
                          static_cast<long long>(offset));
            expect_halted(machine.run(), label);

            // The reference, computed over the snapshot rather than over live memory.
            std::vector<std::uint8_t> expected = before;
            const std::int64_t source_index = static_cast<std::int64_t>(kRegion) - (kRegion - 64);
            const std::int64_t destination_index =
                static_cast<std::int64_t>(destination) - static_cast<std::int64_t>(kRegion - 64);
            if (forward) {
                // The ascending byte-by-byte result IS the definition here, so a write can be
                // observed by a later read of the same operation.
                for (std::uint64_t i = 0; i < kCount; ++i) {
                    expected[static_cast<std::size_t>(destination_index + static_cast<std::int64_t>(i))] =
                        expected[static_cast<std::size_t>(source_index + static_cast<std::int64_t>(i))];
                }
            } else {
                // Read the whole source before writing any of the destination.
                std::vector<std::uint8_t> staged;
                for (std::uint64_t i = 0; i < kCount; ++i) {
                    staged.push_back(
                        before[static_cast<std::size_t>(source_index + static_cast<std::int64_t>(i))]);
                }
                for (std::uint64_t i = 0; i < kCount; ++i) {
                    expected[static_cast<std::size_t>(destination_index + static_cast<std::int64_t>(i))] =
                        staged[static_cast<std::size_t>(i)];
                }
            }

            const std::vector<std::uint8_t> actual = snapshot(machine.memory(), kRegion - 64, 192);
            if (actual != expected) {
                record_failure(std::string(label) + ": the transferred bytes do not match");
            }

            check_equal_u64(machine.get(4), source + kCount, label, __FILE__, __LINE__);
            check_equal_u64(machine.get(5), destination + kCount, label, __FILE__, __LINE__);
            check_equal_u64(machine.get(6), 0, label, __FILE__, __LINE__);
        }
    }

    // A count of zero is valid, performs no access, raises no fault, and leaves the named
    // registers as it found them.
    {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r_r(op::kBlockCopy, reg(4), reg(5), reg(6)).halt();
        machine.load(program);
        machine.set(4, 0xFFFFFFFFFFFFFFFFull);  // an address that would fault if touched
        machine.set(5, 0xFFFFFFFFFFFFFFFFull - 1);
        machine.set(6, 0);
        expect_halted(machine.run(), "block_copy with a count of zero");
        V2_CHECK_EQ(machine.get(4), 0xFFFFFFFFFFFFFFFFull);
        V2_CHECK_EQ(machine.get(6), 0u);
    }
}

V2_FIXTURE(block_memory_restart_invariant) {
    // At every interruptible point the count register holds the bytes NOT YET transferred and
    // each pointer register holds the LOWEST address in its region not yet transferred. The
    // fixture arms a physical-memory fault partway through, reads the register state, has the
    // host make the missing region populated (standing in for a kernel servicing the fault),
    // re-executes, and requires the result to be byte-identical to an uninterrupted run.
    constexpr std::uint64_t kSource = 0x180;
    constexpr std::uint64_t kDestination = 0x1F8;
    constexpr std::uint64_t kCount = 16;

    // The uninterrupted reference.
    std::vector<std::uint8_t> reference;
    {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r_r(op::kBlockCopyForward, reg(4), reg(5), reg(6)).halt();
        machine.load(program);
        fill_pattern(machine.memory(), kSource, kCount, 0x21);
        machine.set(4, kSource);
        machine.set(5, kDestination);
        machine.set(6, kCount);
        expect_halted(machine.run(), "the uninterrupted block_copy_forward");
        reference = snapshot(machine.memory(), kDestination, kCount);
    }

    {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r_r(op::kBlockCopyForward, reg(4), reg(5), reg(6)).halt();
        machine.load(program);
        fill_pattern(machine.memory(), kSource, kCount, 0x21);
        machine.memory().host_set_size(0x200);  // the destination now runs past populated memory
        machine.set(4, kSource);
        machine.set(5, kDestination);
        machine.set(6, kCount);

        const StepResult result = machine.step();
        expect_trap(result, cause::kPhysicalMemoryFault, 0, 0x200, kBase,
                    "block_copy_forward interrupted partway");

        // Eight bytes fit before the boundary, so eight remain. Ascending travel shows its
        // progress by advancing BOTH pointers and decrementing the count together.
        V2_CHECK_EQ(machine.get(6), 8u);
        V2_CHECK_EQ(machine.get(4), kSource + 8);
        V2_CHECK_EQ(machine.get(5), kDestination + 8);
        V2_CHECK(snapshot(machine.memory(), kDestination, 8) ==
                 std::vector<std::uint8_t>(reference.begin(), reference.begin() + 8));

        // The kernel services the fault and the instruction re-executes from the register state
        // it left behind. No byte is transferred twice and none is skipped.
        //
        // The host resumes the machine rather than a handler doing it, because this machine has
        // no vector table installed, so the fault it just took had nowhere to be delivered and
        // stopped the machine (trap-model.md, "No handler installed"). A fixture that wants the
        // guest-visible route through trap_return has one since maize-464, and
        // block_memory_fault_restart_through_a_real_handler in fixtures_traps.cpp is it.
        machine.memory().host_set_size(0x400);
        machine.interpreter().host_resume_at(kBase);
        expect_halted(machine.run(), "block_copy_forward re-executed after the fault");
        V2_CHECK_EQ(machine.get(6), 0u);
        V2_CHECK_EQ(machine.get(4), kSource + kCount);
        V2_CHECK_EQ(machine.get(5), kDestination + kCount);
        V2_CHECK(snapshot(machine.memory(), kDestination, kCount) == reference);
    }

    // block_set carries the same invariant.
    {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r_r(op::kBlockSet, reg(3), reg(5), reg(6)).halt();
        machine.load(program);
        machine.memory().host_set_size(0x200);
        machine.set(3, 0xAA);
        machine.set(5, kDestination);
        machine.set(6, kCount);

        const StepResult result = machine.step();
        expect_trap(result, cause::kPhysicalMemoryFault, 0, 0x200, kBase,
                    "block_set interrupted partway");
        V2_CHECK_EQ(machine.get(6), 8u);
        V2_CHECK_EQ(machine.get(5), kDestination + 8);
        V2_CHECK_EQ(machine.get(3), 0xAAu);  // the value register is never written

        machine.memory().host_set_size(0x400);
        machine.interpreter().host_resume_at(kBase);
        expect_halted(machine.run(), "block_set re-executed after the fault");
        V2_CHECK_EQ(machine.get(6), 0u);
        V2_CHECK_EQ(machine.get(5), kDestination + kCount);
        for (std::uint64_t i = 0; i < kCount; ++i) {
            V2_CHECK_EQ(machine.memory().read_byte(kDestination + i), 0xAAu);
        }
    }

    // The high-to-low half of the same invariant. block_copy travels descending when the
    // destination begins strictly inside the source region, and a descending pass shows its
    // progress by decrementing the COUNT ALONE: the untransferred bytes are still the low ones,
    // so the pointers already name them and neither moves. An implementation that advanced the
    // pointers here would describe work it had not done.
    //
    // First at the trivial point, where the very first byte the descending pass touches is
    // inaccessible, so nothing has been transferred at all.
    {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r_r(op::kBlockCopy, reg(4), reg(5), reg(6)).halt();
        machine.load(program);
        machine.memory().host_set_size(0x200);
        const std::uint64_t source = 0x1F0;
        const std::uint64_t destination = 0x1F8;  // eight above the source, so the copy overlaps
        machine.set(4, source);
        machine.set(5, destination);
        machine.set(6, kCount);

        const StepResult result = machine.step();
        expect_trap(result, cause::kPhysicalMemoryFault, 0, 0x207, kBase,
                    "an overlapping block_copy that faults at the top of its destination");
        V2_CHECK_EQ(machine.get(6), kCount);   // nothing transferred yet
        V2_CHECK_EQ(machine.get(4), source);   // and the pointers do not move to say otherwise
        V2_CHECK_EQ(machine.get(5), destination);
    }

    // Then at a real one, where the descending pass has transferred half its bytes before it
    // faults, so the count is PARTLY decremented and the pointers still have to stand still.
    //
    // Reaching that state takes the top of the address space. Populated memory is a single
    // bound here, so the inaccessible addresses are always the high ones, and a descending pass
    // meets the high ones first: it can only ever fault on its first byte. Wrapping the source
    // region past address zero inverts that. The source begins eight bytes below the top of the
    // address space and its upper half continues at address zero, so the bytes the descending
    // pass reaches FIRST are the wrapped, accessible ones and the bytes it reaches LAST are the
    // ones off the end of memory.
    {
        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r_r(op::kBlockCopy, reg(4), reg(5), reg(6)).halt();
        machine.load(program);
        const std::uint64_t source = 0xFFFFFFFFFFFFFFF8ull;
        const std::uint64_t destination = 0;  // eight above the source modulo 2^64, so descending
        fill_pattern(machine.memory(), 0, 0x10, 0x31);
        const std::vector<std::uint8_t> before = snapshot(machine.memory(), 0, 0x10);
        machine.set(4, source);
        machine.set(5, destination);
        machine.set(6, kCount);

        const StepResult result = machine.step();
        expect_trap(result, cause::kPhysicalMemoryFault, 0, 0xFFFFFFFFFFFFFFFFull, kBase,
                    "a descending block_copy that faults halfway");

        // Eight of the sixteen bytes moved, so eight remain. The count says so ALONE: both
        // pointers still name the lowest untransferred address in their region, which is where
        // they started.
        V2_CHECK_EQ(machine.get(6), 8u);
        V2_CHECK_EQ(machine.get(4), source);
        V2_CHECK_EQ(machine.get(5), destination);

        // And the bytes actually transferred are exactly the ones the registers do not describe:
        // the high half of the destination holds the wrapped low half of the source, and the
        // rest of the region is untouched.
        for (unsigned i = 0; i < 8; ++i) {
            V2_CHECK_EQ(machine.memory().read_byte(8 + i), before[i]);
        }
        for (unsigned i = 0; i < 8; ++i) {
            V2_CHECK_EQ(machine.memory().read_byte(i), before[i]);
        }
    }

    // Re-executing from a descending restart state completes the transfer byte-identical to an
    // uninterrupted run. This is the composition the case above cannot reach, because the top of
    // the address space cannot be made populated, so the restart state is built here from the
    // invariant's own description of it rather than from an implementation detail.
    //
    // It is worth its own case because re-execution flips the implementation to its ASCENDING
    // branch: the count has shrunk to the offset between the regions, so the overlap test that
    // chose descending the first time is false the second time. Two different code paths
    // therefore have to compose into one correct result.
    {
        constexpr std::uint64_t kSourceRegion = 0x1F0;
        constexpr std::uint64_t kDestinationRegion = 0x1F8;  // overlapping, so the first pass descends
        constexpr std::uint64_t kWindow = 0x40;
        constexpr std::uint64_t kWindowBase = 0x1E0;

        // The uninterrupted reference: read the whole source, then write the destination.
        std::vector<std::uint8_t> uninterrupted;
        {
            Machine machine(0x400);
            Encoder program(kBase);
            program.op_r_r_r(op::kBlockCopy, reg(4), reg(5), reg(6)).halt();
            machine.load(program);
            fill_pattern(machine.memory(), kWindowBase, kWindow, 0x47);
            machine.set(4, kSourceRegion);
            machine.set(5, kDestinationRegion);
            machine.set(6, kCount);
            expect_halted(machine.run(), "the uninterrupted overlapping block_copy");
            uninterrupted = snapshot(machine.memory(), kWindowBase, kWindow);
        }

        Machine machine(0x400);
        Encoder program(kBase);
        program.op_r_r_r(op::kBlockCopy, reg(4), reg(5), reg(6)).halt();
        machine.load(program);
        fill_pattern(machine.memory(), kWindowBase, kWindow, 0x47);

        // Stand in for the interrupted first pass: move the top half of the region descending,
        // one byte at a time, exactly as far as a pass stopped halfway would have got.
        for (std::uint64_t i = kCount; i-- > kCount / 2;) {
            machine.memory().write_byte(kDestinationRegion + i,
                                        machine.memory().read_byte(kSourceRegion + i));
        }
        // The restart state the invariant describes: count is the bytes not yet transferred and
        // each pointer is the lowest address in its region not yet transferred, which for a
        // descending pass is still where it started.
        machine.set(4, kSourceRegion);
        machine.set(5, kDestinationRegion);
        machine.set(6, kCount / 2);

        expect_halted(machine.run(), "an overlapping block_copy re-executed from a descending "
                                     "restart state");
        V2_CHECK(snapshot(machine.memory(), kWindowBase, kWindow) == uninterrupted);
        V2_CHECK_EQ(machine.get(6), 0u);
        V2_CHECK_EQ(machine.get(4), kSourceRegion + kCount / 2);
        V2_CHECK_EQ(machine.get(5), kDestinationRegion + kCount / 2);
    }
}

}  // namespace maize::v2::test
