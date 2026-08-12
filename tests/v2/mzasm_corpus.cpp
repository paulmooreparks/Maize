// mzasm_corpus.cpp (maize-422): AC-4, the full-corpus encode and decode.
//
// The corpus is GENERATED from the appendix's own Operands spellings rather than written by
// hand, which is what makes its coverage a fact rather than a claim: every assigned byte
// contributes exactly one line, so all 187 forms are exercised and none can be forgotten.
//
// Two independent oracles check the result, and each catches what the other cannot.
//
//   The appendix supplies the expected opcode BYTE at each instruction's offset. This is the
//   check that proves the specific opcode chosen was the correct one. It never reads a byte
//   mzasm emitted and it never reads kOpcodeTable, because an expectation computed from the
//   thing under test proves nothing: maize-418 shipped 149 fixtures built that way and every
//   one of them stayed green through a deliberate permutation of the implementation.
//
//   decode_v2.cpp, written independently on maize-418 and untouched here, supplies the shape
//   and length. That proves the encoder round-trips through a decoder that shares none of its
//   code. It cannot prove opcode identity on its own, because a same-shaped wrong opcode
//   decodes just as cleanly, which is exactly why the appendix lookup above exists.

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "appendix_a.h"
#include "decode_v2.h"
#include "memory_v2.h"
#include "mzasm_test_support.h"
#include "opcode_v2.h"

namespace maize::v2::test {

namespace {

std::string byte_text(std::uint8_t byte) {
    std::ostringstream out;
    out << "$" << std::hex << std::uppercase << (byte < 16 ? "0" : "")
        << static_cast<unsigned>(byte);
    return out.str();
}

// Turn one appendix operand token into a concrete operand. The register choices are distinct
// across the whole set so the block-memory band's rule (no two of its three slots name the same
// register, and no pointer or count slot names r0) is satisfied without a special case.
bool instantiate(const std::string& token, std::string& out) {
    static const std::map<std::string, std::string> table = {
        {"rs1", "r1"},     {"rs2", "r2"},        {"rs3", "r3"},      {"rd", "r4"},
        {"rs", "r5"},      {"rc", "r6"},         {"rn", "r7"},       {"rv", "r8"},
        {"rb", "r9"},      {"rp", "r10"},
        {"rs.bN", "r5.b3"}, {"rs.qN", "r5.q2"},  {"rs.hN", "r5.h1"},
        {"rd.bN", "r4.b3"}, {"rd.qN", "r4.q2"},  {"rd.hN", "r4.h1"},
        {"@rb", "@r9"},    {"@rb+$disp", "@r9+$20"},
        {"@rs", "@r5"},    {"@rd", "@r4"},
        {"$imm", "$10"},   {"#imm", "#8"},
        {"#pos", "#12"},   {"#width", "#5"},
        {"$csr", "$4000"},
        // A numeric literal in a target slot IS the displacement, so the corpus writes one and
        // needs no label to reach.
        {"target", "$10"},
    };
    const auto it = table.find(token);
    if (it == table.end()) {
        return false;
    }
    out = it->second;
    return true;
}

struct CorpusLine {
    std::uint8_t byte = 0;
    std::uint64_t offset = 0;
    unsigned length = 0;
    std::string text;
};

// Build the corpus source and, alongside it, the offset and expected opcode byte of every
// instruction. The offsets come from the appendix's own Len column, so nothing mzasm produced
// is consulted to work out where an instruction begins.
bool build_corpus(const Appendix& parsed, std::string& source, std::vector<CorpusLine>& lines) {
    std::ostringstream out;
    std::uint64_t offset = 0;
    bool ok = true;

    for (unsigned b = 0; b < 256; ++b) {
        const AppendixRow& row = parsed.bytes[b];
        if (row.disposition != Disposition::Assigned) {
            continue;
        }
        std::istringstream operands(row.operands);
        std::string mnemonic;
        operands >> mnemonic;
        if (mnemonic != row.mnemonic) {
            record_failure("the appendix's Operands cell for " +
                           byte_text(static_cast<std::uint8_t>(b)) + " opens with '" + mnemonic +
                           "' and its Mnemonic cell says '" + row.mnemonic + "'");
            ok = false;
            continue;
        }
        std::string line = mnemonic;
        std::string token;
        bool line_ok = true;
        while (operands >> token) {
            std::string concrete;
            if (!instantiate(token, concrete)) {
                record_failure("no corpus instantiation for operand token '" + token + "' in '" +
                               row.operands + "'");
                line_ok = false;
                ok = false;
                break;
            }
            line += " " + concrete;
        }
        if (!line_ok) {
            continue;
        }
        out << "    " << line << "\n";
        lines.push_back({static_cast<std::uint8_t>(b), offset, row.length, line});
        offset += row.length;
    }
    source = out.str();
    return ok;
}

}  // namespace

MZ_FIXTURE(corpus_covers_every_assigned_opcode) {
    const Appendix parsed = parse_appendix(repo_root() + "/docs/spec-v2/appendix-a-opcode-map.md");
    for (const std::string& error : parsed.errors) {
        record_failure("appendix parse: " + error);
    }

    std::string source;
    std::vector<CorpusLine> lines;
    if (!build_corpus(parsed, source, lines)) {
        return;
    }
    MZ_CHECK_EQ(lines.size(), 187u);

    ScratchDir scratch("corpus");
    const std::string input = scratch.write("corpus.mzasm", source);
    const RunResult run = run_mzasm({input});
    if (run.exit_code != 0) {
        record_failure("the corpus did not assemble cleanly:\n" + run.output);
        return;
    }

    std::vector<std::uint8_t> image;
    if (!read_file_bytes(scratch.file("corpus.mzi"), image)) {
        record_failure("mzasm wrote no corpus.mzi");
        return;
    }

    // The image length is the appendix's own sum of lengths, so a single wrong-length encoding
    // shows up here before any per-instruction check runs.
    std::uint64_t expected_size = 0;
    for (const CorpusLine& line : lines) {
        expected_size += line.length;
    }
    MZ_CHECK_EQ(image.size(), expected_size);
    if (image.size() != expected_size) {
        return;
    }

    // Check one: the opcode byte at each instruction's offset is the byte the appendix assigns
    // to that instruction's mnemonic and operand-selected form.
    for (const CorpusLine& line : lines) {
        if (image[static_cast<std::size_t>(line.offset)] != line.byte) {
            record_failure("'" + line.text + "' assembled to opcode " +
                           byte_text(image[static_cast<std::size_t>(line.offset)]) +
                           ", and the appendix assigns it " + byte_text(line.byte));
        }
    }

    // Check two: the independently written decoder walks the whole image, and every instruction
    // decodes to the length, shape and slot classes kOpcodeTable declares for its opcode byte,
    // with no illegal-instruction or illegal-operand trap.
    MemoryV2 memory(static_cast<std::size_t>(expected_size) + 4096);
    if (!memory.load_image(0, image.data(), image.size())) {
        record_failure("the corpus image did not load into a decode buffer");
        return;
    }
    for (const CorpusLine& line : lines) {
        const DecodeResult decoded = decode_v2(memory, line.offset);
        if (decoded.status != DecodeStatus::Ok) {
            record_failure("'" + line.text + "' at offset " + std::to_string(line.offset) +
                           " does not decode");
            continue;
        }
        const OpcodeInfo& info = kOpcodeTable[decoded.instruction.opcode];
        // A reserved byte never reaches this point, since decode_v2 traps on one, so this is
        // the structural half of the no-synthesis policy (AC-8): the assembler never emits a
        // byte outside kOpcodeTable's Assigned set.
        if (info.kind != OpcodeKind::Assigned) {
            record_failure("'" + line.text + "' emitted a byte that is not an assigned opcode");
            continue;
        }
        const ShapeInfo shape = shape_info(info.shape);
        MZ_CHECK_EQ(decoded.instruction.length, info.length);
        MZ_CHECK_EQ(decoded.instruction.operand_count, shape.operands);
        MZ_CHECK_EQ(decoded.instruction.immediate_count, shape.immediates);
        MZ_CHECK_EQ(decoded.instruction.next_pc, line.offset + line.length);
        for (unsigned i = 0; i < decoded.instruction.operand_count; ++i) {
            if (!form_is_legal(info.slots[i], decoded.instruction.form[i])) {
                record_failure("'" + line.text + "' operand " + std::to_string(i) +
                               " carries a form field its slot class does not define");
            }
        }
    }
}

}  // namespace maize::v2::test
