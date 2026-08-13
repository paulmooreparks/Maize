// appendix_a.h (maize-422): docs/spec-v2/appendix-a-opcode-map.md parsed into a per-byte
// disposition, as the conformance oracle AC-14 requires.
//
// Why parse the specification at all, when the project already has two tables describing the
// same 256 bytes: two tables that check each other cannot detect an error they share. AC-10
// walks kOpcodeTable and mnemonic_v2.h against each other and proves bijection and select/shape
// family consistency, but `subtract` transcribed against `multiply`'s opcode is Shape::OpRRR on
// both sides and both tables agree on the wrong answer. The appendix is the frozen normative
// assignment and is not a transcription either artifact made, so it is the one oracle that can
// catch that class of error.
//
// THE PARSE IS TOTAL, and totality is an acceptance property rather than a quality of this
// implementation. All 256 bytes are accounted for exactly once, no byte is claimed twice, and
// the counts come out at exactly 187 assigned, 7 escape and 62 reserved. A parser that silently
// skipped a row it could not match would therefore fail rather than pass on a subset, which is
// the failure mode this whole approach exists to prevent. Totality is coverage and says nothing
// about content on its own; the content comparisons are the checks in mzasm_conformance.cpp,
// and the two together are what close the trap.
//
// The appendix is NOT one uniform table, and a positional five-column parser is wrong. Four
// shapes appear, and this parser refuses to guess when a fifth turns up:
//
//   1. The standard band tables (A.3 to A.8, A.10 to A.12): Byte, Mnemonic, Operands, Form, Len.
//   2. A.9, extract and insert, which inserts a sixth column, Slots, between Operands and Form.
//      Column identity therefore comes from each table's own header row and never from
//      position. The Slots column is data the appendix states nowhere else, so it is kept and
//      checked against kOpcodeTable's slot classes.
//   3. A.13, the extension escapes, a two-column Byte and Role table over $F8..$FE.
//   4. A.14, which gives $FF in prose with no table row at all.
//
// TWO bytes turn out to be stated only in prose, not one. A.14 gives $FF as `breakpoint`, and
// A.2 gives $00 as the zero-byte guard. They are handled differently and the difference matters:
// $00's reserved status is already stated machine-readably by A.15's enumeration and by A.1's
// band row, so it needs no hand transcription at all, while $FF's mnemonic text, Form and Len
// exist nowhere but A.14's prose and are declared in appendix_a.cpp with a comment citing it.

#ifndef MAIZE_V2_TESTS_APPENDIX_A_H
#define MAIZE_V2_TESTS_APPENDIX_A_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace maize::v2::test {

enum class Disposition : std::uint8_t { Unaccounted, Assigned, Escape, Reserved };

struct AppendixRow {
    Disposition disposition = Disposition::Unaccounted;
    std::string mnemonic;  // Assigned rows only
    std::string operands;  // Assigned rows only: the Operands cell, verbatim
    std::string form;      // Assigned rows only: the length class, e.g. "op r r i4"
    unsigned length = 0;   // Assigned rows only
    std::string slots;     // A.9 only: the Slots cell, verbatim; empty everywhere else
    std::string claimed_by;  // which section claimed this byte, for a double-claim report
};

// One row of A.1. The escape band spells its count as "7 escapes" rather than as an assigned
// count, so the two are kept apart here rather than conflated into one number.
struct BandSummary {
    std::string range;
    std::uint8_t first = 0;
    std::uint8_t last = 0;
    std::string family;
    int assigned = 0;
    int escapes = 0;
    int reserved = 0;
};

struct Appendix {
    std::array<AppendixRow, 256> bytes{};
    std::vector<BandSummary> bands;

    // A.15 states the complete reserved set a second time, in prose. Parsing it as an
    // independent statement and comparing it byte for byte against the reserved runs gathered
    // from the band tables is what catches a reserved row dropped from either place.
    std::vector<std::uint8_t> prose_reserved;
    int prose_reserved_stated_count = 0;

    // Anything the parser could not account for. A non-empty list fails the conformance test:
    // a parser that cannot read a row must say so rather than skip it.
    std::vector<std::string> errors;

    int count(Disposition disposition) const;
};

Appendix parse_appendix(const std::string& path);

// ---------------------------------------------------------------------------------------
// The generic table reader the appendix parse and the CSR check share
// ---------------------------------------------------------------------------------------

// One markdown table lifted out of a chapter, with its cells' back-ticks stripped. Column
// identity comes from the table's own header row and never from position, for the same reason
// it does in the appendix parse: a chapter is free to insert a column, and a positional reader
// silently starts reading the wrong one when it does.
struct MarkdownTable {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;

    int index_of(const std::string& name) const;
    bool has(const std::string& name) const { return index_of(name) >= 0; }
    // The cell in `row` under the column called `name`, or "" when this table has no such
    // column or the row is short.
    std::string cell(std::size_t row, const std::string& name) const;
};

// Every markdown table in a chapter, in document order. A table whose rows do not all carry the
// same number of cells as its header is reported through `errors` rather than being reshaped to
// fit, so a malformed table fails a caller that checks errors instead of quietly losing a cell.
std::vector<MarkdownTable> read_markdown_tables(const std::string& path,
                                                std::vector<std::string>& errors);

// A `$`-prefixed hexadecimal literal of any width, as the specification writes numbers.
bool parse_hex_number(const std::string& text, std::uint64_t& out);

}  // namespace maize::v2::test

#endif  // MAIZE_V2_TESTS_APPENDIX_A_H
