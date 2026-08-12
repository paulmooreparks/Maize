// mzasm_language.cpp (maize-422): the language and CLI criteria, AC-2, AC-3, AC-5 through
// AC-9, AC-12 and AC-13.
//
// Every fixture here drives the shipped mzasm binary and reads what it wrote back off disk,
// rather than calling the assembler in process. Several criteria are about the binary's
// filesystem behaviour (what --check touches, what a failed run removes, which suffix an output
// takes), and an in-process check would answer a different question.

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/maize_obj.h"
#include "appendix_a.h"
#include "decode_v2.h"
#include "memory_v2.h"
#include "mzasm_test_support.h"
#include "opcode_v2.h"

namespace maize::v2::test {

namespace {

std::vector<std::uint8_t> parse_byte_listing(const std::string& text) {
    std::vector<std::uint8_t> bytes;
    std::istringstream in(text);
    std::string token;
    while (in >> token) {
        if (token.size() < 2 || token[0] != '$') {
            continue;
        }
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(token.substr(1), nullptr, 16)));
    }
    return bytes;
}

// Assemble one flat source and hand back the image. A diagnostic is reported as a failure with
// the assembler's own output attached, since that output is the evidence.
bool assemble_flat(const ScratchDir& scratch, const std::string& name, const std::string& source,
                   std::vector<std::uint8_t>& image) {
    const std::string input = scratch.write(name + ".mzasm", source);
    const RunResult run = run_mzasm({input});
    if (run.exit_code != 0) {
        record_failure("'" + name + "' did not assemble:\n" + source + "\n" + run.output);
        return false;
    }
    if (!read_file_bytes(scratch.file(name + ".mzi"), image)) {
        record_failure("'" + name + "' produced no .mzi");
        return false;
    }
    return true;
}

// Assert that a source is rejected, that the rejection is nonzero-exit, and that no output file
// is left behind. The message substring keeps a fixture from passing on the wrong diagnostic.
void expect_diagnostic(const ScratchDir& scratch, const std::string& name,
                       const std::string& source, const std::string& expected_substring) {
    const std::string input = scratch.write(name + ".mzasm", source);
    const RunResult run = run_mzasm({input});
    if (run.exit_code == 0) {
        record_failure("'" + name + "' assembled and should not have:\n" + source);
        return;
    }
    if (run.output.find("mzasm: ") == std::string::npos ||
        run.output.find(": error: ") == std::string::npos) {
        record_failure("'" + name + "' did not report in the documented diagnostic shape:\n" +
                       run.output);
    }
    if (!expected_substring.empty() &&
        run.output.find(expected_substring) == std::string::npos) {
        record_failure("'" + name + "' reported:\n" + run.output + "expected it to mention '" +
                       expected_substring + "'");
    }
    if (file_exists(scratch.file(name + ".mzi"))) {
        record_failure("'" + name + "' produced a diagnostic and still wrote an output file");
    }
}

void expect_accepted(const ScratchDir& scratch, const std::string& name,
                     const std::string& source) {
    const std::string input = scratch.write(name + ".mzasm", source);
    const RunResult run = run_mzasm({"--check", input});
    if (run.exit_code != 0) {
        record_failure("'" + name + "' was rejected and should not have been:\n" + source + "\n" +
                       run.output);
    }
}

std::string read_chapter(const std::string& relative) {
    std::string text;
    if (!read_file_text(repo_root() + "/docs/spec-v2/" + relative, text)) {
        record_failure("cannot read docs/spec-v2/" + relative);
    }
    return text;
}

// Pull the indented lines of a chapter section out of the chapter, which is how the two worked
// examples below stay tied to the specification rather than to a copy of it.
std::vector<std::string> indented_lines_of_section(const std::string& chapter,
                                                   const std::string& heading) {
    std::vector<std::string> lines;
    std::istringstream in(chapter);
    std::string line;
    bool inside = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("## ", 0) == 0) {
            inside = line.substr(3) == heading;
            continue;
        }
        if (!inside) continue;
        if (line.rfind("    ", 0) == 0) {
            lines.push_back(line.substr(4));
        }
    }
    return lines;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// AC-2: instruction-encoding.md's worked byte listings
// ---------------------------------------------------------------------------------------

MZ_FIXTURE(encoding_chapter_worked_examples) {
    ScratchDir scratch("worked");

    // The assembly and the bytes of each worked example in instruction-encoding.md's "Worked
    // examples" section, in the chapter's own order. The branch example is written with the
    // literal displacement form, because the chapter states the assembly with a label
    // (`loop_top`) and then gives the bytes for a displacement of -24; the literal form is how
    // the assembler is told to emit that displacement unconverted, and $-18 is -24.
    struct WorkedExample {
        const char* assembly;
        const char* bytes;
    };
    static const std::vector<WorkedExample> examples = {
        {"add r1 r2 r3", "$10 $01 $02 $03"},
        {"add r4 $1000 r4", "$31 $04 $04 $00 $10 $00 $00"},
        {"load @r30+$20 r5", "$87 $1E $05 $20 $00"},
        {"load.zb @r9 r4", "$81 $09 $04"},
        {"extract.zb r3.b5 r7", "$A0 $A3 $07"},
        {"bitfield_extract r5 #12 #5 r6", "$A9 $05 $06 $0C $05"},
        {"branch_lt_signed r4 r5 $-18", "$62 $04 $05 $E8 $FF $FF $FF"},
        {"move.w $1122334455667788 r10", "$02 $0A $88 $77 $66 $55 $44 $33 $22 $11"},
        {"return", "$74"},
    };

    // The chapter is the authority on how many listings there are, so the count is read from it
    // rather than assumed. A tenth example added to the chapter fails here instead of being
    // quietly untested.
    const std::string chapter = read_chapter("instruction-encoding.md");
    const std::vector<std::string> indented = indented_lines_of_section(chapter, "Worked examples");
    int listings = 0;
    for (const std::string& line : indented) {
        if (!line.empty() && line[0] == '$') {
            ++listings;
        }
    }
    MZ_CHECK_EQ(static_cast<std::uint64_t>(listings), examples.size());

    for (const WorkedExample& example : examples) {
        const std::vector<std::uint8_t> expected = parse_byte_listing(example.bytes);
        std::vector<std::uint8_t> actual;
        if (!assemble_flat(scratch, "we", std::string("    ") + example.assembly + "\n", actual)) {
            continue;
        }
        if (actual != expected) {
            record_failure(std::string("'") + example.assembly + "' assembled to " +
                           hex_dump(actual) + ", and the chapter gives " + hex_dump(expected));
        }
    }
}

// ---------------------------------------------------------------------------------------
// AC-3: assembler.md's own worked example, in section mode, as real .mzo bytes
// ---------------------------------------------------------------------------------------

MZ_FIXTURE(assembler_chapter_worked_example) {
    using namespace maize::obj;

    const std::string chapter = read_chapter("assembler.md");
    const std::vector<std::string> lines = indented_lines_of_section(chapter, "A worked example");
    std::ostringstream source;
    for (const std::string& line : lines) {
        source << line << "\n";
    }
    MZ_CHECK(lines.size() > 50);

    ScratchDir scratch("worked-object");
    const std::string input = scratch.write("word_to_hex.mzasm", source.str());
    const RunResult run = run_mzasm({"-c", input});
    if (run.exit_code != 0) {
        record_failure("the chapter's worked example did not assemble:\n" + run.output);
        return;
    }

    std::vector<std::uint8_t> object;
    if (!read_file_bytes(scratch.file("word_to_hex.mzo"), object)) {
        record_failure("no .mzo was written; D-10 rules out stubbing the write");
        return;
    }
    MZ_CHECK(object.size() > MZO_HEADER_SIZE);
    if (object.size() <= MZO_HEADER_SIZE) {
        return;
    }

    const std::uint8_t* bytes = object.data();
    MZ_CHECK_EQ(get_u8(bytes, 0), static_cast<std::uint64_t>(MZO_MAGIC0));
    MZ_CHECK_EQ(get_u8(bytes, 1), static_cast<std::uint64_t>(MZO_MAGIC1));
    MZ_CHECK_EQ(get_u8(bytes, 2), static_cast<std::uint64_t>(MZO_MAGIC2));
    // The header carries the v2 discriminator, so nothing can read this as a v1 object.
    MZ_CHECK_EQ(get_u8(bytes, 3), static_cast<std::uint64_t>(MZO_VERSION_V2));

    const std::uint16_t section_count = get_u16(bytes, 6);
    const std::uint64_t section_headers = get_u64(bytes, 8);
    const std::uint64_t symbol_offset = get_u64(bytes, 16);
    const std::uint32_t symbol_count = get_u32(bytes, 24);
    const std::uint64_t string_offset = get_u64(bytes, 28);
    MZ_CHECK_EQ(section_count, 2u);  // the routine declares code and rodata

    const auto symbol_name = [&](std::uint32_t index) {
        const std::size_t entry = static_cast<std::size_t>(symbol_offset) + index * SYMBOL_SIZE;
        const std::uint32_t name_offset = get_u32(bytes, entry);
        return std::string(reinterpret_cast<const char*>(bytes) + string_offset + name_offset);
    };

    // Find the code section and its relocations.
    std::uint64_t code_offset = 0;
    std::uint64_t code_size = 0;
    std::uint64_t code_relocations = 0;
    std::uint64_t code_relocation_count = 0;
    for (std::uint16_t i = 0; i < section_count; ++i) {
        const std::size_t header = static_cast<std::size_t>(section_headers) + i * SECTION_HDR_SIZE;
        if (get_u8(bytes, header + 4) == SEC_CODE) {
            code_offset = get_u64(bytes, header + 8);
            code_size = get_u64(bytes, header + 16);
            code_relocations = get_u64(bytes, header + 24);
            code_relocation_count = get_u64(bytes, header + 32);
        }
    }
    MZ_CHECK(code_size > 0);

    // The pc_add line crosses a section boundary, so it emits a placeholder and one
    // program-counter-relative relocation the linker resolves. That is the only relocation in
    // the routine.
    MZ_CHECK_EQ(code_relocation_count, 1u);
    if (code_relocation_count == 1) {
        const std::size_t entry = static_cast<std::size_t>(code_relocations);
        const std::uint64_t field = get_u64(bytes, entry);
        const std::uint32_t symbol = get_u32(bytes, entry + 8);
        const std::uint8_t type = get_u8(bytes, entry + 12);
        MZ_CHECK_EQ(type, static_cast<std::uint64_t>(R_MAIZE_REL32));
        MZ_CHECK_TEXT(symbol_name(symbol), "hex_digits");
        MZ_CHECK(symbol < symbol_count);

        // The field it names holds a four-byte zero placeholder, and the opcode two bytes
        // earlier is pc_add with its destination register between them.
        MZ_CHECK(field >= 2 && field + 4 <= code_size);
        if (field >= 2 && field + 4 <= code_size) {
            MZ_CHECK_EQ(bytes[code_offset + field - 2], op::kPcAdd);
            for (int i = 0; i < 4; ++i) {
                MZ_CHECK_EQ(bytes[code_offset + field + i], 0u);
            }
        }
    }

    // Four lines of the listing are worth reading twice, the chapter says, and every one of them
    // is a non-relocatable instruction whose bytes the encoding chapter fixes exactly.
    struct Expected {
        const char* what;
        std::vector<std::uint8_t> bytes;
    };
    const std::vector<Expected> spot_checks = {
        // store ra @sp+slot_link: the displaced store, r31 as the source, r30 as the base, and
        // the constant expression slot_link folding to 24 exactly as a literal would.
        {"store ra @sp+slot_link", {op::kStoreDisp, 0x1F, 0x1E, 0x18, 0x00}},
        // extract.zb r5.b7 r2: a slice in the only slot that admits one, its index travelling in
        // the operand byte's form field rather than in an immediate.
        {"extract.zb r5.b7 r2", {op::kExtractZb, 0xE5, 0x02}},
        // insert.b r12 r11.b1: the one merge site in the routine.
        {"insert.b r12 r11.b1", {op::kInsertB, 0x0C, 0x2B}},
        // store.q r11 @r3: the bare quarter-word store.
        {"store.q r11 @r3", {op::kStoreQ, 0x0B, 0x03}},
    };
    const std::vector<std::uint8_t> code(bytes + code_offset, bytes + code_offset + code_size);
    for (const Expected& check : spot_checks) {
        const auto found = std::search(code.begin(), code.end(), check.bytes.begin(),
                                       check.bytes.end());
        if (found == code.end()) {
            record_failure(std::string("the code section does not contain ") + check.what +
                           " encoded as " + hex_dump(check.bytes));
        }
    }

    // The whole code section decodes cleanly and the walk lands exactly on its end, so no
    // instruction in the routine is a length the decoder disagrees with.
    MemoryV2 memory(static_cast<std::size_t>(code_size) + 4096);
    MZ_CHECK(memory.load_image(0, code.data(), code.size()));
    std::uint64_t pc = 0;
    int instructions = 0;
    while (pc < code_size) {
        const DecodeResult decoded = decode_v2(memory, pc);
        if (decoded.status != DecodeStatus::Ok) {
            record_failure("the routine's code section does not decode at offset " +
                           std::to_string(pc));
            break;
        }
        pc = decoded.instruction.next_pc;
        ++instructions;
    }
    MZ_CHECK_EQ(pc, code_size);
    MZ_CHECK(instructions > 25);
}

// ---------------------------------------------------------------------------------------
// AC-5: every register name and every calling-convention alias is reserved
// ---------------------------------------------------------------------------------------

MZ_FIXTURE(register_names_are_reserved_words) {
    ScratchDir scratch("reserved");

    // abi.md's whole register table, plus the three architectural aliases, plus every canonical
    // rN spelling. A program that could define a label called `a0` would give `jump a0` two
    // derivations that emit different bytes and shift every address after them.
    std::vector<std::string> names;
    for (int i = 0; i <= 31; ++i) {
        names.push_back("r" + std::to_string(i));
    }
    for (const char* alias : {"zero", "tp", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "t0",
                              "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9", "s0", "s1",
                              "s2", "s3", "s4", "s5", "s6", "s7", "s8", "fp", "sp", "ra"}) {
        names.emplace_back(alias);
    }
    MZ_CHECK_EQ(names.size(), 64u);

    for (const std::string& name : names) {
        expect_diagnostic(scratch, "label", name + ":\n    nop\n", "reserved word");
        expect_diagnostic(scratch, "constant", "    constant " + name + " #1\n    nop\n",
                          "reserved word");
    }
    // `here` is reserved on the same footing, since it names the address of the statement it
    // appears in and a program that redefined it would have two readings of the same token.
    expect_diagnostic(scratch, "here_label", "here:\n    nop\n", "reserved word");
    expect_diagnostic(scratch, "here_constant", "    constant here #1\n", "reserved word");
}

// ---------------------------------------------------------------------------------------
// AC-6: the dual-reading field-fit rule at both boundaries of all nine categories
// ---------------------------------------------------------------------------------------

MZ_FIXTURE(field_fit_is_dual_reading_at_every_boundary) {
    ScratchDir scratch("fit");

    // Every immediate field assembler.md's "Expressions" section enumerates, with the width the
    // chapter gives it. An N-bit field accepts -2^(N-1) through 2^N - 1 under the dual reading,
    // so the two accepted edges and the two rejected values one past them are what each row
    // below exercises.
    struct Field {
        const char* what;
        const char* before;  // the source, with %VALUE% standing in for the immediate
        unsigned bits;
    };
    static const std::vector<Field> fields = {
        {"memory displacement", "    load @r9+%VALUE% r4\n", 16},
        {"ALU immediate", "    add r1 %VALUE% r3\n", 32},
        {"branch displacement", "    branch_eq r1 r2 %VALUE%\n", 32},
        {"jump displacement", "    jump %VALUE%\n", 32},
        {"call displacement", "    call %VALUE%\n", 32},
        {"pc_add displacement", "    pc_add %VALUE% r4\n", 32},
        {"shift count", "    shift_left r1 %VALUE% r3\n", 8},
        {"syscall number", "    sys %VALUE%\n", 8},
        {"CSR number", "    csr_read %VALUE% r4\n", 16},
        {"bitfield position", "    bitfield_extract r1 %VALUE% #5 r3\n", 8},
        {"bitfield width", "    bitfield_extract r1 #12 %VALUE% r3\n", 8},
        {"move.zb immediate", "    move.zb %VALUE% r4\n", 8},
        {"move.sb immediate", "    move.sb %VALUE% r4\n", 8},
        {"move.zq immediate", "    move.zq %VALUE% r4\n", 16},
        {"move.sq immediate", "    move.sq %VALUE% r4\n", 16},
        {"move.zh immediate", "    move.zh %VALUE% r4\n", 32},
        {"move.sh immediate", "    move.sh %VALUE% r4\n", 32},
        {"data_byte operand", "    data_byte %VALUE%\n", 8},
        {"data_quarter_word operand", "    data_quarter_word %VALUE%\n", 16},
        {"data_half_word operand", "    data_half_word %VALUE%\n", 32},
    };

    const auto substitute = [](const std::string& pattern, const std::string& value) {
        std::string out = pattern;
        const std::size_t at = out.find("%VALUE%");
        out.replace(at, 7, value);
        return out;
    };
    const auto decimal = [](long long value) {
        std::ostringstream out;
        out << "#" << value;
        return out.str();
    };

    for (const Field& field : fields) {
        const long long unsigned_max = (1LL << field.bits) - 1;
        const long long signed_min = -(1LL << (field.bits - 1));
        const std::string tag = std::string("fit-") + std::to_string(field.bits);

        expect_accepted(scratch, tag, substitute(field.before, decimal(unsigned_max)));
        expect_accepted(scratch, tag, substitute(field.before, decimal(signed_min)));
        expect_diagnostic(scratch, tag, substitute(field.before, decimal(unsigned_max + 1)),
                          "does not fit");
        expect_diagnostic(scratch, tag, substitute(field.before, decimal(signed_min - 1)),
                          "does not fit");
    }

    // move.w and data_word are 64-bit fields, and a 64-bit field accepts every value expression
    // arithmetic can compute, so neither has an out-of-range boundary to test. Both edges of the
    // 64-bit range are checked here instead, to show the rule holds rather than that a check was
    // skipped.
    expect_accepted(scratch, "fit64", "    move.w $FFFFFFFFFFFFFFFF r4\n");
    expect_accepted(scratch, "fit64", "    move.w $-8000000000000000 r4\n");
    expect_accepted(scratch, "fit64", "    data_word $FFFFFFFFFFFFFFFF\n");
    expect_accepted(scratch, "fit64", "    data_word $-8000000000000000\n");

    // The three worked dual-reading examples the chapter states in prose, each assembling to the
    // exact bytes it gives.
    struct DualReading {
        const char* assembly;
        std::vector<std::uint8_t> bytes;
    };
    static const std::vector<DualReading> dual = {
        {"move.sb $FF r3", {op::kMoveSb, 0x03, 0xFF}},
        {"move.sq $8000 r4", {op::kMoveSq, 0x04, 0x00, 0x80}},
        {"move.sh $FFFFFFF8 r5", {op::kMoveSh, 0x05, 0xF8, 0xFF, 0xFF, 0xFF}},
    };
    for (const DualReading& example : dual) {
        std::vector<std::uint8_t> image;
        if (!assemble_flat(scratch, "dual", std::string("    ") + example.assembly + "\n", image)) {
            continue;
        }
        if (image != example.bytes) {
            record_failure(std::string("'") + example.assembly + "' assembled to " +
                           hex_dump(image) + ", and the chapter gives " + hex_dump(example.bytes));
        }
    }
}

// ---------------------------------------------------------------------------------------
// AC-7: where mnemonic selection stops in the move family
// ---------------------------------------------------------------------------------------

MZ_FIXTURE(move_family_selection_stops_where_the_chapter_says) {
    ScratchDir scratch("move");

    // The bare mnemonic is register-to-register and nothing else.
    std::vector<std::uint8_t> image;
    if (assemble_flat(scratch, "bare", "    move r0 r5\n", image)) {
        const std::vector<std::uint8_t> expected = {op::kMove, 0x00, 0x05};
        if (image != expected) {
            record_failure("'move r0 r5' assembled to " + hex_dump(image) + ", expected " +
                           hex_dump(expected));
        }
    }

    // An immediate move written without a width is a diagnostic naming the missing width, not a
    // silently chosen encoding, and neither the base marker nor the digit count of a literal
    // carries width information.
    expect_diagnostic(scratch, "unwidthed", "    move $5 r5\n", "names its width");
    expect_diagnostic(scratch, "unwidthed_wide", "    move $1122334455667788 r5\n",
                      "names its width");
    expect_diagnostic(scratch, "unwidthed_decimal", "    move #5 r5\n", "names its width");

    // A width-suffixed move given a register operand is equally a diagnostic: the width names
    // the immediate form, and there is no register form of it to fall back to.
    for (const char* mnemonic : {"move.zb", "move.sb", "move.zq", "move.sq", "move.zh", "move.sh",
                                 "move.w"}) {
        expect_diagnostic(scratch, "widthed_register",
                          std::string("    ") + mnemonic + " r0 r5\n", "");
    }
}

// ---------------------------------------------------------------------------------------
// AC-8: the no-synthesis policy
// ---------------------------------------------------------------------------------------

MZ_FIXTURE(the_assembler_synthesizes_nothing) {
    ScratchDir scratch("nosynth");

    // No constant materialization. A value too wide for the form the source names is a
    // diagnostic, not a silently emitted pair of instructions and not a truncation.
    expect_diagnostic(scratch, "too_wide", "    move.zb $100 r4\n", "does not fit");
    expect_diagnostic(scratch, "too_wide_alu", "    add r1 $100000000 r3\n", "does not fit");

    // No branch relaxation and no long-branch expansion. A displacement that does not fit is a
    // diagnostic and the branch is not rewritten into a longer sequence.
    std::ostringstream far;
    far << "    origin #0\n"
           "far_target:\n"
           "    nop\n"
           "    origin #4294967300\n"
           "    branch_eq r1 r2 far_target\n";
    expect_diagnostic(scratch, "far_branch", far.str(), "signed 32-bit");

    // The structural half of the property, that no byte outside kOpcodeTable's Assigned set is
    // ever emitted, is proved over the whole instruction set by the corpus fixture's decode
    // pass, which would trap on a reserved byte.
}

// ---------------------------------------------------------------------------------------
// AC-9: what --check touches, and what a failed run leaves behind
// ---------------------------------------------------------------------------------------

MZ_FIXTURE(check_touches_nothing_and_a_failure_removes_stale_output) {
    ScratchDir scratch("cli");

    const std::string clean_source = "    nop\n    halt\n";
    const std::string broken_source = "    nop\n    this_is_not_a_mnemonic r1\n";

    const std::string clean = scratch.write("clean.mzasm", clean_source);
    const std::string broken = scratch.write("broken.mzasm", broken_source);

    // --check on a clean source: the full pipeline runs and writes nothing.
    {
        const RunResult run = run_mzasm({"--check", clean});
        MZ_CHECK_EQ(static_cast<std::uint64_t>(run.exit_code), 0u);
        MZ_CHECK(!file_exists(scratch.file("clean.mzi")));
    }

    // --check on a broken source: it reports, exits nonzero, and still writes nothing.
    {
        const RunResult run = run_mzasm({"--check", broken});
        MZ_CHECK(run.exit_code != 0);
        MZ_CHECK(!file_exists(scratch.file("broken.mzi")));
    }

    // --check removes nothing either. A stale output sitting at the target path survives a
    // --check run untouched, which is what "no filesystem effect" has to mean to be worth
    // stating.
    {
        std::ofstream stale(scratch.file("broken.mzi"), std::ios::binary);
        stale << "stale";
        stale.close();
        const RunResult run = run_mzasm({"--check", broken});
        MZ_CHECK(run.exit_code != 0);
        MZ_CHECK(file_exists(scratch.file("broken.mzi")));
        std::string contents;
        read_file_text(scratch.file("broken.mzi"), contents);
        MZ_CHECK_TEXT(contents, "stale");
    }

    // An ordinary run on the broken source removes the stale output before assembly begins and
    // writes nothing new, so a failed assembly can never be mistaken for a successful one that
    // simply did not rerun.
    {
        const RunResult run = run_mzasm({broken});
        MZ_CHECK(run.exit_code != 0);
        MZ_CHECK(!file_exists(scratch.file("broken.mzi")));
    }

    // The clean source does produce an output when it is not a --check run.
    {
        const RunResult run = run_mzasm({clean});
        MZ_CHECK_EQ(static_cast<std::uint64_t>(run.exit_code), 0u);
        MZ_CHECK(file_exists(scratch.file("clean.mzi")));
    }
}

// ---------------------------------------------------------------------------------------
// AC-12: inclusion, the shipped CSR file, and the cycle diagnostic
// ---------------------------------------------------------------------------------------

MZ_FIXTURE(include_resolves_csr_names_and_reports_a_cycle) {
    ScratchDir scratch("include");

    // The shipped declarations file the toolchain provides, used exactly as the specification's
    // own examples assume it is.
    const std::string csr_path = repo_root() + "/asm/v2/csr.mzasm";
    MZ_CHECK(file_exists(csr_path));

    std::ostringstream source;
    source << "    include \"" << csr_path << "\"\n"
           << "    csr_read fcsr r5\n"
           << "    csr_write r1 status\n"
           << "    csr_swap r2 trap_vector_base r3\n";
    std::vector<std::uint8_t> image;
    if (assemble_flat(scratch, "csr", source.str(), image)) {
        const std::vector<std::uint8_t> expected = {
            op::kCsrRead, 0x05, 0x00, 0x00,        // csr_read fcsr r5, fcsr is $0000
            op::kCsrWrite, 0x01, 0x00, 0x40,       // csr_write r1 status, status is $4000
            op::kCsrSwap, 0x02, 0x03, 0x02, 0x40,  // csr_swap r2 trap_vector_base r3
        };
        if (image != expected) {
            record_failure("the CSR example assembled to " + hex_dump(image) + ", expected " +
                           hex_dump(expected));
        }
    }

    // A relative path resolves against the directory of the file containing the directive, never
    // against the working directory, so a source tree moves without breaking.
    scratch.write("shared.mzasm", "    constant shared_value #7\n");
    expect_accepted(scratch, "relative",
                    "    include \"shared.mzasm\"\n    move.zb shared_value r4\n");

    // A cycle among included files is a diagnostic naming the cycle.
    scratch.write("cycle_a.mzasm", "    include \"cycle_b.mzasm\"\n");
    scratch.write("cycle_b.mzasm", "    include \"cycle_a.mzasm\"\n");
    {
        const RunResult run = run_mzasm({scratch.file("cycle_a.mzasm")});
        MZ_CHECK(run.exit_code != 0);
        if (run.output.find("include cycle") == std::string::npos) {
            record_failure("a cyclic include did not name the cycle:\n" + run.output);
        }
    }

    // A self-referential include is the same diagnostic with a one-file cycle.
    scratch.write("self.mzasm", "    include \"self.mzasm\"\n");
    {
        const RunResult run = run_mzasm({scratch.file("self.mzasm")});
        MZ_CHECK(run.exit_code != 0);
        if (run.output.find("include cycle") == std::string::npos) {
            record_failure("a self-referential include did not name the cycle:\n" + run.output);
        }
    }

    // A diagnostic inside an included file reports the included file and its line, then the
    // including file and its line.
    scratch.write("bad.mzasm", "    nop\n    not_a_mnemonic\n");
    {
        const std::string top = scratch.write("top.mzasm", "    nop\n    include \"bad.mzasm\"\n");
        const RunResult run = run_mzasm({top});
        MZ_CHECK(run.exit_code != 0);
        if (run.output.find("bad.mzasm:2:") == std::string::npos) {
            record_failure("the diagnostic did not name the included file and line:\n" +
                           run.output);
        }
        if (run.output.find("top.mzasm:2:") == std::string::npos) {
            record_failure("the diagnostic did not name the including file and line:\n" +
                           run.output);
        }
    }
}

// ---------------------------------------------------------------------------------------
// The two path normalizations, which must agree
// ---------------------------------------------------------------------------------------

// assemble_file and the include directive each turn a path into the key that identifies a file,
// and those two keys are compared against each other to detect an include cycle. They must
// therefore normalize the same way. Until now that invariant was held by nothing but a comment
// at each site, on a card whose whole subject is checks that bite, and reverting either site
// alone left the full suite green.
//
// A correction to how the consequence was described, because getting this wrong is the same
// error in miniature. Reverting assemble_file alone does NOT make the cycle go undetected and
// does NOT fail AC-12. The cycle is still reported, one nesting level late, because the two keys
// stop colliding until the included file includes itself a second time. The observable
// difference is the chain: it reads `self.mzasm -> self.mzasm` where the correct output names
// the path once. That string is what separates the two states, so that string is what this
// asserts.
//
// Both spellings of the path are exercised. On a host whose preferred separator is a backslash,
// running only one spelling would leave it open whether the check passes because the
// normalization is right or because the command line happened to be written the way the
// normalization produces.
MZ_FIXTURE(include_paths_normalize_identically_at_both_sites) {
    ScratchDir scratch("normalize");

    const auto count_occurrences = [](const std::string& haystack, const std::string& needle) {
        std::size_t count = 0;
        for (std::size_t at = haystack.find(needle); at != std::string::npos;
             at = haystack.find(needle, at + needle.size())) {
            ++count;
        }
        return count;
    };

    // The cycle chain, which is the text after "include cycle: " up to the end of that line.
    const auto cycle_chain = [](const std::string& output) {
        const std::size_t at = output.find("include cycle: ");
        if (at == std::string::npos) {
            return std::string();
        }
        const std::size_t from = at + std::string("include cycle: ").size();
        std::string line = output.substr(from, output.find('\n', from) - from);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        return line;
    };

    scratch.write("self.mzasm", "    include \"self.mzasm\"\n");
    const std::filesystem::path written = scratch.file("self.mzasm");

    struct Spelling {
        const char* what;
        std::string path;
    };
    const std::vector<Spelling> spellings = {
        {"the host's preferred separator", std::filesystem::path(written).make_preferred().string()},
        {"forward slashes", written.generic_string()},
    };

    for (const Spelling& spelling : spellings) {
        const RunResult run = run_mzasm({spelling.path});
        if (run.exit_code == 0) {
            record_failure(std::string("a self-including file assembled cleanly, given ") +
                           spelling.what);
            continue;
        }
        const std::string chain = cycle_chain(run.output);
        if (chain.empty()) {
            record_failure(std::string("no include cycle was reported, given ") + spelling.what +
                           ":\n" + run.output);
            continue;
        }
        // The file includes itself directly, so the cycle is one file long and the chain names
        // it once. Naming it twice means the two sites disagreed and the collision only happened
        // one level deeper.
        const std::size_t named = count_occurrences(chain, "self.mzasm");
        if (named != 1) {
            std::ostringstream message;
            message << "given " << spelling.what << ", the cycle chain names the file " << named
                    << " times and a directly self-including file is a one-file cycle; the chain "
                       "reads '"
                    << chain << "'";
            record_failure(message.str());
        }
    }

    // The same invariant seen from the other side. A diagnostic chain crossing an include
    // boundary prints two file names, one from each normalization site, so a disagreement shows
    // up as two separator conventions in one chain.
    scratch.write("inner.mzasm", "    nop\n    not_a_mnemonic\n");
    const std::string outer =
        scratch.write("outer.mzasm", "    nop\n    include \"inner.mzasm\"\n");
    const RunResult nested = run_mzasm({std::filesystem::path(outer).make_preferred().string()});
    MZ_CHECK(nested.exit_code != 0);
    if (nested.output.find('\\') != std::string::npos) {
        record_failure(
            "a diagnostic chain crossing an include boundary mixes path separators, so the two "
            "normalization sites disagree:\n" +
            nested.output);
    }
    // Both ends of the chain are present, so the check above is looking at a real two-file chain
    // rather than passing because one of the lines never printed.
    MZ_CHECK(nested.output.find("inner.mzasm:2:") != std::string::npos);
    MZ_CHECK(nested.output.find("outer.mzasm:2:") != std::string::npos);
}

// ---------------------------------------------------------------------------------------
// AC-13: the .mzi suffix, and mzvm running what mzasm wrote
// ---------------------------------------------------------------------------------------

MZ_FIXTURE(flat_output_takes_the_mzi_suffix) {
    ScratchDir scratch("suffix");

    const std::string source = "    origin $1000\n    move.zb #7 r4\n    halt\n";
    const std::string input = scratch.write("foo.mzasm", source);
    const RunResult run = run_mzasm({input});
    MZ_CHECK_EQ(static_cast<std::uint64_t>(run.exit_code), 0u);
    MZ_CHECK(file_exists(scratch.file("foo.mzi")));
    // v1's suffix is never written and never named. Two machines whose images are
    // indistinguishable by name invite feeding one to the other, and neither loader inspects a
    // file before loading it.
    MZ_CHECK(!file_exists(scratch.file("foo.mzb")));
    if (run.output.find(".mzb") != std::string::npos) {
        record_failure("mzasm named .mzb in its output:\n" + run.output);
    }

    // The help text says .mzi and never .mzb.
    const RunResult help = run_mzasm({"--help"});
    MZ_CHECK_EQ(static_cast<std::uint64_t>(help.exit_code), 0u);
    if (help.output.find(".mzi") == std::string::npos) {
        record_failure("the help text does not name .mzi:\n" + help.output);
    }
    if (help.output.find(".mzb") != std::string::npos) {
        record_failure("the help text names .mzb:\n" + help.output);
    }

    // A diagnostic that names an output path names a .mzi one.
    const std::string unwritable = scratch.write("bad.mzasm", "    not_a_mnemonic\n");
    const RunResult failed = run_mzasm({unwritable});
    MZ_CHECK(failed.exit_code != 0);
    if (failed.output.find(".mzb") != std::string::npos) {
        record_failure("a diagnostic named .mzb:\n" + failed.output);
    }
}

MZ_FIXTURE(mzvm_runs_what_mzasm_wrote) {
    ScratchDir scratch("run");

    // A small program at mzvm's default load address, so the two tools meet with no argument
    // negotiation: the assembler places the image and the machine loads it there.
    const std::string source =
        "    origin $1000\n"
        "    move.zb #7 r4\n"
        "    move.zb #35 r5\n"
        "    add r4 r5 r6\n"
        "    halt\n";
    const std::string input = scratch.write("run.mzasm", source);
    const RunResult assembled = run_mzasm({input});
    MZ_CHECK_EQ(static_cast<std::uint64_t>(assembled.exit_code), 0u);

    const std::string mzvm = sibling_binary("mzvm");
    MZ_CHECK(file_exists(mzvm));
    if (!file_exists(mzvm)) {
        return;
    }
    const RunResult ran = run_binary(mzvm, {scratch.file("run.mzi")});
    MZ_CHECK_EQ(static_cast<std::uint64_t>(ran.exit_code), 0u);
    if (ran.output.find("halted") == std::string::npos) {
        record_failure("mzvm did not halt on the image mzasm wrote:\n" + ran.output);
    }
}

MZ_FIXTURE(mzvm_prints_hello_world) {
    // maize-451, and the milestone the whole card exists for: the SHIPPED asm/v2/hello.mzasm
    // assembles with the SHIPPED mzasm and prints through the SHIPPED mzvm. Nothing here is
    // hand-assembled and nothing is driven in process, because what is being tested is that the
    // three artifacts a person would actually use agree with each other.
    //
    // The scratch tag is "hello" rather than "run" deliberately. ScratchDir builds its path as
    // "mzasm-<tag>" verbatim, so two fixtures sharing a tag share a directory and delete each
    // other's files under `ctest -j` (maize-444, which cost nine v2 fixtures).
    ScratchDir scratch("hello");

    // The shipped sources, copied into the scratch directory rather than assembled where they
    // live: mzasm writes its image beside its input, and a test that leaves a build artifact in
    // asm/v2 dirties the working tree of whoever ran it. The bytes assembled are the repository's
    // own, which is what the criterion is about; only the directory differs, and the include
    // resolves against the input file's directory either way.
    std::string hello_source;
    std::string devices_source;
    const bool read_hello = read_file_text(repo_root() + "/asm/v2/hello.mzasm", hello_source);
    const bool read_devices = read_file_text(repo_root() + "/asm/v2/devices.mzasm", devices_source);
    MZ_CHECK(read_hello);
    MZ_CHECK(read_devices);
    if (!read_hello || !read_devices) {
        return;
    }
    scratch.write("devices.mzasm", devices_source);
    const std::string input = scratch.write("hello.mzasm", hello_source);

    const RunResult assembled = run_mzasm({input});
    MZ_CHECK_EQ(static_cast<std::uint64_t>(assembled.exit_code), 0u);
    if (assembled.exit_code != 0) {
        record_failure("mzasm rejected the shipped hello.mzasm:\n" + assembled.output);
        return;
    }
    const std::string image = scratch.file("hello.mzi");
    MZ_CHECK(file_exists(image));

    const std::string mzvm = sibling_binary("mzvm");
    MZ_CHECK(file_exists(mzvm));
    if (!file_exists(mzvm)) {
        return;
    }

    const RunResult ran = run_binary(mzvm, {image});
    MZ_CHECK_EQ(static_cast<std::uint64_t>(ran.exit_code), 0u);

    // The exact bytes, on standard output alone. Asserting the exit code would pass on a machine
    // that printed nothing at all, and asserting the combined stream would pass on a machine that
    // printed the greeting to stderr. This is the assertion the card's exit criterion names, and
    // it is the reason RunResult carries the two streams separately.
    const std::string expected = "hello, maize\n";
    MZ_CHECK_TEXT(ran.standard_output, expected);

    // mzvm's own diagnostics are on stderr, where a guest's output is not. Both halves are
    // checked: the status line is somewhere, and it is not on stdout.
    if (ran.standard_error.find("halted") == std::string::npos) {
        record_failure("mzvm did not report halting on stderr:\n" + ran.standard_error);
    }
    if (ran.standard_output.find("halted") != std::string::npos) {
        record_failure("a diagnostic reached standard output:\n" + ran.standard_output);
    }
}

namespace {
// The files that make up the v2 assembler, DISCOVERED rather than listed. A hand-maintained
// list would be a rule generalizing over a set whose membership nothing enforces: correct on the
// day it is written and silently short by one the first time a source file is added. This card
// ships the right pattern for that problem two files away, in the CMake-versus-C++ fixture
// registry cross-check, and the same reasoning applies here.
//
// Discovery is by directory and name pattern rather than by a glob string, so the rule a reader
// checks is the rule the code runs.
// Each place the v2 assembler keeps files, with the pattern that discovers them AND the files
// that must turn up there. Both halves are load-bearing and they guard opposite failures.
// Discovery means a file added tomorrow is scanned with no list to edit. The named requirements
// mean a file that DISAPPEARS from a category fails by name.
//
// The requirement is per category and per file rather than one aggregate count, because an
// aggregate has slack and slack is where a file hides. A floor of fifteen over sixteen files let
// the scripts pattern narrow to `install-mzasm.sh` alone, silently dropping `install-mzasm.ps1`,
// with a planted suffix inside it unseen and the fixture green. That file is the PowerShell twin
// of the shell installer, and this project's history is explicit that Windows CI rots when a
// change lands in one script of a pair and not the other, so it is named here rather than
// counted. So is every other file: ten of the sixteen were unnamed under the old floor and any
// one of them could be lost the same way.
struct SourceCategory {
    const char* directory;
    bool (*matches)(const std::string& filename);
    std::vector<const char*> required;
};

const std::vector<SourceCategory>& v2_source_categories() {
    static const std::vector<SourceCategory> categories = {
        {"src/v2",
         [](const std::string& n) { return n.rfind("mzasm", 0) == 0 || n == "mnemonic_v2.h"; },
         {"mzasm.h", "mzasm_lexer.cpp", "mzasm_assemble.cpp", "mzasm_object.cpp",
          "mzasm_main.cpp", "mnemonic_v2.h"}},
        {"tests/v2",
         [](const std::string& n) {
             return n.rfind("mzasm", 0) == 0 || n.rfind("appendix_a", 0) == 0;
         },
         {"appendix_a.h", "appendix_a.cpp", "mzasm_test_support.h", "mzasm_test_support.cpp",
          "mzasm_conformance.cpp", "mzasm_corpus.cpp", "mzasm_language.cpp"}},
        {"scripts",
         [](const std::string& n) { return n.rfind("install-mzasm", 0) == 0; },
         // Both, by name. Losing either one is the failure this entry exists to catch.
         {"install-mzasm.sh", "install-mzasm.ps1"}},
        {"asm/v2",
         [](const std::string& n) {
             return n.size() > 6 && n.compare(n.size() - 6, 6, ".mzasm") == 0;
         },
         {"csr.mzasm"}},
    };
    return categories;
}

std::vector<std::string> discover_in(const SourceCategory& category) {
    std::vector<std::string> found;
    std::error_code ec;
    const std::filesystem::path directory = repo_root() + "/" + category.directory;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string filename = entry.path().filename().string();
        if (category.matches(filename)) {
            found.push_back(std::string(category.directory) + "/" + filename);
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

// Does the VS Code task labelled `label` mention `needle`? Factored out of the fixture so the
// window bound below can be tested against synthetic input, which is the only way to show that
// the bound holds rather than to assert that it does.
//
// `found_label` distinguishes "the task exists and is clean" from "there is no such task", which
// a bare bool would conflate into a pass.
bool task_object_mentions(const std::string& tasks, const std::string& label,
                          const std::string& needle, bool& found_label) {
    const std::size_t at = tasks.find(label);
    found_label = at != std::string::npos;
    if (!found_label) {
        return false;
    }
    // The window opens at the task object's own brace rather than at the label text, because
    // "label" need not be the object's first key and anything ahead of it would otherwise go
    // unscanned. That error's direction is a false pass, which is the direction that matters.
    const std::size_t brace = tasks.rfind('{', at);
    const std::size_t start = brace == std::string::npos ? at : brace;

    // It closes at the next task's label or at the end of the file. Bounding it at a fixed byte
    // count instead would read into whichever task happens to follow, which passes only for as
    // long as that neighbour stays clean: this file holds v1 tasks that legitimately name the v1
    // suffix, so a fixed window turns a reordering of tasks.json into a failure about nothing.
    // The search for the next label starts at the label TEXT, not at the brace, so this task's
    // own "label" key does not end the window before it begins.
    const std::size_t next = tasks.find("\"label\"", at);
    const std::size_t end = next == std::string::npos ? tasks.size() : next;
    return tasks.find(needle, start) < end;
}

}  // namespace

MZ_FIXTURE(nothing_in_the_v2_assembler_names_the_v1_suffix) {
    // AC-13's source-level half. mzasm, its tests, the install scripts and the two new VS Code
    // tasks must not name .mzb anywhere, so a reader is never told that a v2 image might take
    // v1's suffix. The v1 tasks and v1's own mazm keep .mzb and are deliberately not scanned:
    // v1 is untouched by this card.
    // Each category is satisfied on its own terms. A file missing from one category fails on
    // that category's own requirement rather than being absorbed by slack in another, which is
    // what an aggregate floor allowed.
    std::vector<std::string> sources;
    for (const SourceCategory& category : v2_source_categories()) {
        const std::vector<std::string> found = discover_in(category);
        for (const char* required : category.required) {
            const std::string path = std::string(category.directory) + "/" + required;
            if (std::find(found.begin(), found.end(), path) == found.end()) {
                record_failure("the source discovery did not find " + path +
                               ", so it is not scanning what it claims to scan");
            }
        }
        sources.insert(sources.end(), found.begin(), found.end());
    }

    for (const std::string& relative : sources) {
        // This file is the one exclusion, and it is deliberate. It has to spell the v1 suffix,
        // because spelling it is how every check here looks for it, and a scanner that failed on
        // its own search term could never pass.
        if (relative == "tests/v2/mzasm_language.cpp") {
            continue;
        }
        std::string text;
        if (!read_file_text(repo_root() + "/" + relative, text)) {
            record_failure("cannot read " + relative);
            continue;
        }
        if (text.find(".mzb") != std::string::npos) {
            record_failure(relative + " names .mzb, and no part of the v2 assembler may");
        }
    }

    // The two new VS Code tasks, read out of tasks.json by label, must not name .mzb either.
    std::string tasks;
    if (!read_file_text(repo_root() + "/.vscode/tasks.json", tasks)) {
        record_failure("cannot read .vscode/tasks.json");
        return;
    }
    for (const char* label : {"Assemble current .mzasm", "Check current .mzasm (no output file)"}) {
        bool found_label = false;
        const bool mentions = task_object_mentions(tasks, label, ".mzb", found_label);
        if (!found_label) {
            record_failure(std::string("tasks.json has no task labelled '") + label + "'");
            continue;
        }
        if (mentions) {
            record_failure(std::string("the '") + label + "' task names .mzb");
        }
    }
}

// The scanner above is only as good as its window, and a window is exactly the kind of thing
// that looks right and is not. This drives it with synthetic input that pins both directions:
// it must see the needle inside the task it was asked about, and it must NOT see one in the
// task next door. The second half is the property the previous fixed-byte window failed to
// have, and no assertion against the real tasks.json could have caught that, because the real
// neighbours happen to be clean.
MZ_FIXTURE(the_task_scanner_stops_at_the_next_task) {
    const std::string tasks =
        "{\n"
        "  \"tasks\": [\n"
        "    {\n"
        "      \"label\": \"Assemble current .mzasm\",\n"
        "      \"command\": \"mzasm\",\n"
        "      \"args\": [\"${file}\"]\n"
        "    },\n"
        "    {\n"
        "      \"label\": \"Assemble current .mazm\",\n"
        "      \"detail\": \"the v1 task, which legitimately writes a .mzb\",\n"
        "      \"command\": \"mazm\"\n"
        "    }\n"
        "  ]\n"
        "}\n";

    bool found = false;

    // The v1 neighbour names the suffix and the v2 task does not, so a correctly bounded scan
    // reports clean. A window that overran into the neighbour would report a failure here.
    MZ_CHECK(!task_object_mentions(tasks, "Assemble current .mzasm", ".mzb", found));
    MZ_CHECK(found);

    // The same scan does find the suffix when it really is inside the task asked about.
    MZ_CHECK(task_object_mentions(tasks, "Assemble current .mazm", ".mzb", found));
    MZ_CHECK(found);

    // A label that is not there is reported as absent rather than as clean.
    found = true;
    MZ_CHECK(!task_object_mentions(tasks, "No such task", ".mzb", found));
    MZ_CHECK(!found);

    // The last task in a file has no following label, so the window runs to the end rather than
    // collapsing to nothing.
    const std::string trailing =
        "{\n  \"tasks\": [\n    {\n      \"label\": \"Only task\",\n"
        "      \"command\": \"mazm foo.mzb\"\n    }\n  ]\n}\n";
    MZ_CHECK(task_object_mentions(trailing, "Only task", ".mzb", found));
    MZ_CHECK(found);

    // "label" is not required to be a task object's first key, so the window opens at the
    // object's brace. A needle sitting AHEAD of the label, inside the same object, is inside the
    // task and must be seen; a window opening at the label text would step over it and pass.
    const std::string label_not_first =
        "{\n"
        "  \"tasks\": [\n"
        "    {\n"
        "      \"detail\": \"writes a foo.mzb, which this task may not\",\n"
        "      \"label\": \"Assemble current .mzasm\",\n"
        "      \"command\": \"mzasm\"\n"
        "    }\n"
        "  ]\n"
        "}\n";
    MZ_CHECK(task_object_mentions(label_not_first, "Assemble current .mzasm", ".mzb", found));
    MZ_CHECK(found);
}


}  // namespace maize::v2::test
