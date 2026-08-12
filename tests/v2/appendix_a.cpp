// appendix_a.cpp (maize-422): the total parse of docs/spec-v2/appendix-a-opcode-map.md.

#include "appendix_a.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace maize::v2::test {

namespace {

std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return "";
    }
    const std::size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// Markdown cells wrap their content in back-ticks throughout this appendix. Strip them so a
// comparison is against the value rather than against the presentation.
std::string strip_ticks(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c != '`') {
            out.push_back(c);
        }
    }
    return trim(out);
}

std::vector<std::string> split_row(const std::string& line) {
    std::vector<std::string> cells;
    std::string current;
    // A markdown row opens and closes with a pipe, so the split leaves an empty cell at each
    // end; both are dropped below.
    for (const char c : line) {
        if (c == '|') {
            cells.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    cells.push_back(current);
    if (!cells.empty()) cells.erase(cells.begin());
    if (!cells.empty()) cells.pop_back();
    for (std::string& cell : cells) {
        cell = trim(cell);
    }
    return cells;
}

bool is_separator_row(const std::string& line) {
    return line.find_first_not_of("|:- \t") == std::string::npos && line.find('-') != std::string::npos;
}

bool parse_hex_byte(const std::string& text, std::uint8_t& out) {
    if (text.size() < 2 || text[0] != '$') {
        return false;
    }
    unsigned value = 0;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        unsigned digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
        else return false;
        value = value * 16 + digit;
    }
    if (value > 255) {
        return false;
    }
    out = static_cast<std::uint8_t>(value);
    return true;
}

// A Byte cell is either a single byte or an inclusive run written `$3E`..`$3F`.
bool parse_byte_cell(const std::string& cell, std::uint8_t& first, std::uint8_t& last) {
    const std::size_t dots = cell.find("..");
    if (dots == std::string::npos) {
        if (!parse_hex_byte(cell, first)) {
            return false;
        }
        last = first;
        return true;
    }
    return parse_hex_byte(trim(cell.substr(0, dots)), first) &&
           parse_hex_byte(trim(cell.substr(dots + 2)), last);
}

// Leading integer of a cell like "7 escapes" or "12".
bool parse_leading_int(const std::string& cell, int& out) {
    std::size_t i = 0;
    while (i < cell.size() && std::isspace(static_cast<unsigned char>(cell[i])) != 0) ++i;
    if (i >= cell.size() || std::isdigit(static_cast<unsigned char>(cell[i])) == 0) {
        return false;
    }
    int value = 0;
    while (i < cell.size() && std::isdigit(static_cast<unsigned char>(cell[i])) != 0) {
        value = value * 10 + (cell[i] - '0');
        ++i;
    }
    out = value;
    return true;
}

// One table's header row, reduced to a column-name-to-index map. Every read below goes through
// this, so A.9's inserted sixth column shifts nothing: the parser asks for "Form" by name and
// gets whichever position that table put it in.
struct Header {
    std::vector<std::string> names;

    int index_of(const std::string& name) const {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i] == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    bool has(const std::string& name) const { return index_of(name) >= 0; }
};

enum class TableShape { Unknown, BandSummary, OpcodeBand, EscapeRoles };

TableShape classify(const Header& header) {
    if (header.has("Range") && header.has("Family") && header.has("Assigned") &&
        header.has("Reserved")) {
        return TableShape::BandSummary;
    }
    if (header.has("Byte") && header.has("Mnemonic") && header.has("Operands") &&
        header.has("Form") && header.has("Len")) {
        return TableShape::OpcodeBand;  // with or without A.9's Slots column
    }
    if (header.has("Byte") && header.has("Role") && header.names.size() == 2) {
        return TableShape::EscapeRoles;
    }
    return TableShape::Unknown;
}

}  // namespace

int Appendix::count(Disposition disposition) const {
    return static_cast<int>(
        std::count_if(bytes.begin(), bytes.end(),
                      [&](const AppendixRow& row) { return row.disposition == disposition; }));
}

Appendix parse_appendix(const std::string& path) {
    Appendix appendix;

    std::ifstream in(path);
    if (!in) {
        appendix.errors.push_back("cannot read " + path);
        return appendix;
    }

    // Claiming a byte twice is itself a finding: it means two tables disagree about who owns it,
    // and a parser that let the second claim win would hide that.
    const auto claim = [&](std::uint8_t byte, const std::string& section, AppendixRow row) {
        AppendixRow& slot = appendix.bytes[byte];
        if (slot.disposition != Disposition::Unaccounted) {
            std::ostringstream message;
            message << "byte $" << std::hex << std::uppercase << static_cast<unsigned>(byte)
                    << " is claimed by both " << slot.claimed_by << " and " << section;
            appendix.errors.push_back(message.str());
            return;
        }
        row.claimed_by = section;
        slot = row;
    };

    std::string line;
    std::string section = "(preamble)";
    Header header;
    bool in_table = false;
    bool expecting_separator = false;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.rfind("## ", 0) == 0) {
            section = trim(line.substr(3));
            in_table = false;
            continue;
        }

        const std::string trimmed = trim(line);

        // A.15 restates the whole reserved set in prose. Parsing it as a second, independent
        // statement of the same fact is what catches a reserved row dropped from a band table.
        if (trimmed.rfind("The complete reserved set is", 0) == 0) {
            std::string paragraph = trimmed;
            std::string next;
            while (std::getline(in, next)) {
                if (!next.empty() && next.back() == '\r') next.pop_back();
                if (trim(next).empty()) break;
                paragraph += " " + trim(next);
            }
            // Walk the paragraph pulling out every `$XX` and every `$XX`..`$YY` run.
            for (std::size_t i = 0; i < paragraph.size();) {
                if (paragraph[i] != '$') {
                    ++i;
                    continue;
                }
                std::size_t end = i + 1;
                while (end < paragraph.size() &&
                       std::isxdigit(static_cast<unsigned char>(paragraph[end])) != 0) {
                    ++end;
                }
                std::uint8_t first = 0;
                if (!parse_hex_byte(paragraph.substr(i, end - i), first)) {
                    i = end;
                    continue;
                }
                std::uint8_t last = first;
                // A run is written `$3E`..`$3F`, so the back-tick and the two dots sit between
                // the two literals in the raw text.
                std::size_t after = end;
                if (paragraph.compare(after, 1, "`") == 0) ++after;
                if (paragraph.compare(after, 2, "..") == 0) {
                    std::size_t second = after + 2;
                    if (paragraph.compare(second, 1, "`") == 0) ++second;
                    std::size_t second_end = second + 1;
                    while (second_end < paragraph.size() &&
                           std::isxdigit(static_cast<unsigned char>(paragraph[second_end])) != 0) {
                        ++second_end;
                    }
                    if (parse_hex_byte(paragraph.substr(second, second_end - second), last)) {
                        end = second_end;
                    } else {
                        last = first;
                    }
                }
                for (unsigned b = first; b <= last; ++b) {
                    appendix.prose_reserved.push_back(static_cast<std::uint8_t>(b));
                }
                i = end;
            }
            const std::size_t which = paragraph.find("which is ");
            if (which != std::string::npos) {
                parse_leading_int(paragraph.substr(which + 9),
                                  appendix.prose_reserved_stated_count);
            }
            continue;
        }

        if (trimmed.empty() || trimmed[0] != '|') {
            in_table = false;
            continue;
        }

        if (!in_table) {
            header.names = split_row(trimmed);
            in_table = true;
            expecting_separator = true;
            if (classify(header) == TableShape::Unknown) {
                std::ostringstream message;
                message << "an unrecognized table shape in " << section << ": columns are";
                for (const std::string& name : header.names) {
                    message << " [" << name << "]";
                }
                message << ". Four shapes are known (band summary, opcode band with or without "
                           "Slots, escape roles); a fifth needs handling rather than skipping";
                appendix.errors.push_back(message.str());
            }
            continue;
        }

        if (expecting_separator) {
            expecting_separator = false;
            if (is_separator_row(trimmed)) {
                continue;
            }
            // Not a separator, so this table had none and the row above was data rather than a
            // header. Say so instead of silently treating data as column names.
            appendix.errors.push_back("a table in " + section + " has no separator row");
            continue;
        }

        const std::vector<std::string> cells = split_row(trimmed);
        const TableShape shape = classify(header);

        if (shape == TableShape::BandSummary) {
            BandSummary band;
            band.range = strip_ticks(cells[static_cast<std::size_t>(header.index_of("Range"))]);
            band.family = cells[static_cast<std::size_t>(header.index_of("Family"))];
            if (!parse_byte_cell(band.range, band.first, band.last)) {
                appendix.errors.push_back("A.1 range '" + band.range + "' is not a byte range");
                continue;
            }
            const std::string assigned_cell =
                strip_ticks(cells[static_cast<std::size_t>(header.index_of("Assigned"))]);
            const std::string reserved_cell =
                strip_ticks(cells[static_cast<std::size_t>(header.index_of("Reserved"))]);
            int value = 0;
            if (parse_leading_int(assigned_cell, value)) {
                // The escape band spells its count "7 escapes" rather than as assigned
                // instructions, and conflating the two would let an escape pass as an opcode.
                if (assigned_cell.find("escape") != std::string::npos) {
                    band.escapes = value;
                } else {
                    band.assigned = value;
                }
            }
            if (parse_leading_int(reserved_cell, value)) {
                band.reserved = value;
            }
            appendix.bands.push_back(band);
            continue;
        }

        if (shape == TableShape::EscapeRoles) {
            const std::string byte_cell =
                strip_ticks(cells[static_cast<std::size_t>(header.index_of("Byte"))]);
            std::uint8_t first = 0;
            std::uint8_t last = 0;
            if (!parse_byte_cell(byte_cell, first, last)) {
                appendix.errors.push_back("an escape row in " + section + " has byte cell '" +
                                          byte_cell + "'");
                continue;
            }
            for (unsigned b = first; b <= last; ++b) {
                AppendixRow row;
                row.disposition = Disposition::Escape;
                claim(static_cast<std::uint8_t>(b), section, row);
            }
            continue;
        }

        if (shape == TableShape::OpcodeBand) {
            const std::string byte_cell =
                strip_ticks(cells[static_cast<std::size_t>(header.index_of("Byte"))]);
            std::uint8_t first = 0;
            std::uint8_t last = 0;
            if (!parse_byte_cell(byte_cell, first, last)) {
                appendix.errors.push_back("a row in " + section + " has byte cell '" + byte_cell +
                                          "'");
                continue;
            }
            const std::string mnemonic =
                strip_ticks(cells[static_cast<std::size_t>(header.index_of("Mnemonic"))]);

            if (mnemonic == "reserved") {
                for (unsigned b = first; b <= last; ++b) {
                    AppendixRow row;
                    row.disposition = Disposition::Reserved;
                    claim(static_cast<std::uint8_t>(b), section, row);
                }
                continue;
            }

            if (first != last) {
                appendix.errors.push_back("an assigned row in " + section +
                                          " spans a range of bytes: '" + byte_cell + "'");
                continue;
            }

            AppendixRow row;
            row.disposition = Disposition::Assigned;
            row.mnemonic = mnemonic;
            row.operands = strip_ticks(cells[static_cast<std::size_t>(header.index_of("Operands"))]);
            row.form = strip_ticks(cells[static_cast<std::size_t>(header.index_of("Form"))]);
            const int slots_column = header.index_of("Slots");
            if (slots_column >= 0) {
                row.slots = strip_ticks(cells[static_cast<std::size_t>(slots_column)]);
            }
            int length = 0;
            if (!parse_leading_int(strip_ticks(cells[static_cast<std::size_t>(header.index_of("Len"))]),
                                   length)) {
                appendix.errors.push_back("a row in " + section + " for byte " + byte_cell +
                                          " has no length");
                continue;
            }
            row.length = static_cast<unsigned>(length);
            claim(first, section, row);
            continue;
        }

        // classify() already recorded the unknown shape; the rows are not guessed at.
    }

    // TWO bytes are stated in prose rather than in a table row, not one. A.14 gives $FF as
    // `breakpoint`, which the criterion this parser serves names; A.2 gives $00 as the zero-byte
    // guard, which it does not. They are handled differently, and the difference is worth
    // stating because it is the reason only one of them is a hand transcription.
    //
    // $00 needs no declaration at all. A.15's enumeration opens with `$00` and is a normative,
    // machine-parseable statement of the complete reserved set, so the loop below lets that
    // enumeration claim any reserved byte no band table claimed. A.1's band-summary row for
    // `$00` independently reads 0 assigned and 1 reserved, and the band reconciliation checks
    // that too, so the byte's disposition rests on two parsed statements and on nothing typed
    // here. A byte A.15 calls reserved that a table has already assigned is a contradiction
    // between two parts of the appendix, and it is reported rather than resolved.
    for (const std::uint8_t byte : appendix.prose_reserved) {
        AppendixRow& slot = appendix.bytes[byte];
        if (slot.disposition == Disposition::Unaccounted) {
            AppendixRow row;
            row.disposition = Disposition::Reserved;
            claim(byte, "A.15 Reserved bytes, enumerated (prose)", row);
        } else if (slot.disposition != Disposition::Reserved) {
            std::ostringstream message;
            message << "A.15 lists byte $" << std::hex << std::uppercase
                    << static_cast<unsigned>(byte) << " as reserved, and " << slot.claimed_by
                    << " does not";
            appendix.errors.push_back(message.str());
        }
    }

    // A.14 gives $FF as `breakpoint` in prose with no table row at all, so these three facts
    // are declared here rather than parsed. Its DISPOSITION is still derived rather than
    // declared: A.1's band-summary row for $FF reads 1 assigned and 0 reserved, that table is
    // machine-parsed above, and the band reconciliation reads it for every other band too, so
    // breakpoint folds into the same check. What is irreducible is the mnemonic text, the Form
    // and the Len, which exist only in A.14's prose. This is a hand transcription checked
    // against nothing and it is admitted as one; the blast radius is a single one-byte
    // instruction with no operands and nothing to get wrong, and the totality assertion
    // guarantees the declaration cannot be quietly dropped.
    {
        AppendixRow row;
        row.disposition = Disposition::Assigned;
        row.mnemonic = "breakpoint";  // A.14, prose
        row.operands = "breakpoint";
        row.form = "op";  // A.14, prose: "a one-byte trap-class instruction"
        row.length = 1;   // A.14, prose
        claim(0xFF, "A.14 Breakpoint, `$FF` (prose, no table row)", row);
    }

    return appendix;
}

}  // namespace maize::v2::test
