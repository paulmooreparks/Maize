// mzasm_assemble.cpp (maize-422): the statement parser, the two passes, and the encoder.
//
// One statement per line, no continuation, and one production per line: assembler.md puts a
// label definition on a line of its own and never on the same line as an instruction, which is
// what keeps the instruction column from shifting when a label is added or removed.
//
// The operand splitter is where the comma-free operand list is actually decided. An expression
// contains no whitespace and whitespace ends an operand, so splitting a line on whitespace
// (outside a string or character literal) yields exactly the operand list the grammar means. A
// comma-separated operand list from another assembler therefore arrives here as one malformed
// field and fails, which is the point of retiring the comma rather than tolerating it.

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../maize_obj.h"
#include "mzasm.h"

namespace maize::v2::asmr {

bool decode_escape(const std::string& text, std::size_t& position, std::uint8_t& out);

namespace {

// ---------------------------------------------------------------------------------------
// The assembly operand pattern of an opcode
// ---------------------------------------------------------------------------------------

// What the assembly syntax writes in each operand position, in source order. This is NOT a
// second declaration of the encoding: shape, slot classes, immediate widths and total length
// all still come from kOpcodeTable, and the emitter below derives the byte layout from the
// shape rather than from this. What lives here is the one fact neither kOpcodeTable nor
// mnemonic_v2.h carries, and the appendix states in its Operands column: the order the SOURCE
// writes operands in, which diverges from byte order wherever an immediate is a source.
// `csr_read $csr rd` and `csr_write rs $csr` share a shape and differ only here.
//
// AC-4 is what proves this table right: the corpus is generated from the appendix's own
// Operands spellings, so a wrong pattern here cannot assemble the appendix's own syntax.
enum class Syn : std::uint8_t {
    Reg,       // a register operand, plain or sliced as kOpcodeTable's slot class declares
    MemBare,   // @rb, no displacement
    MemDisp,   // @rb+$disp, one immediate
    Imm,       // a constant expression immediate
    Target,    // a branch, jump, call or pc_add target: an address, or a literal displacement
    ImmAbs,    // move.w's immediate, the one instruction immediate that accepts a relocation
};

struct Pattern {
    std::array<Syn, 4> items{};
    std::uint8_t count = 0;
};

Pattern make(std::initializer_list<Syn> items) {
    Pattern p;
    for (Syn s : items) {
        p.items[p.count++] = s;
    }
    return p;
}

Pattern pattern_for(std::uint8_t opcode) {
    switch (opcode) {
        case op::kMove: return make({Syn::Reg, Syn::Reg});
        case op::kMoveW: return make({Syn::ImmAbs, Syn::Reg});
        case op::kPcAdd: return make({Syn::Target, Syn::Reg});
        case op::kJumpDisp:
        case op::kCallDisp: return make({Syn::Target});
        case op::kJumpReg:
        case op::kCallReg:
        case op::kSysReg:
        case op::kTlbInvalidateAddress: return make({Syn::Reg});
        case op::kSysImm: return make({Syn::Imm});
        case op::kCsrRead: return make({Syn::Imm, Syn::Reg});
        case op::kCsrWrite: return make({Syn::Reg, Syn::Imm});
        case op::kCsrSwap: return make({Syn::Reg, Syn::Imm, Syn::Reg});
        case op::kBlockCopy:
        case op::kBlockCopyForward: return make({Syn::MemBare, Syn::MemBare, Syn::Reg});
        case op::kBlockSet: return make({Syn::Reg, Syn::MemBare, Syn::Reg});
        default: break;
    }
    // move.zb through move.sh: the narrow immediate moves, each naming its width.
    if (opcode >= op::kMoveZb && opcode <= op::kMoveSh) {
        return make({Syn::Imm, Syn::Reg});
    }
    // The ALU and compare immediate forms, and the shift-count forms.
    if ((opcode >= op::kAddImm && opcode <= op::kShiftRightArithmeticHImm) ||
        (opcode >= op::kCompareImmBase && opcode <= op::kCompareImmBase + 9)) {
        return make({Syn::Reg, Syn::Imm, Syn::Reg});
    }
    // The fused compare-and-branch forms.
    if (opcode >= op::kBranchBase && opcode <= op::kBranchBase + 9) {
        return make({Syn::Reg, Syn::Reg, Syn::Target});
    }
    // Loads and stores. The displaced opcode is the bare opcode plus seven for a load and plus
    // four for a store, and the two differ in this table only by which memory form they take.
    if (opcode >= op::kLoad && opcode <= op::kLoadSh) {
        return make({Syn::MemBare, Syn::Reg});
    }
    if (opcode >= op::kLoadDisp && opcode <= op::kLoadDisp + 6) {
        return make({Syn::MemDisp, Syn::Reg});
    }
    if (opcode >= op::kStore && opcode <= op::kStoreH) {
        return make({Syn::Reg, Syn::MemBare});
    }
    if (opcode >= op::kStoreDisp && opcode <= op::kStoreDisp + 3) {
        return make({Syn::Reg, Syn::MemDisp});
    }
    // The general bitfield instructions, the only base instructions with two immediates.
    if (opcode >= op::kBitfieldExtract && opcode <= op::kBitfieldInsert) {
        return make({Syn::Reg, Syn::Imm, Syn::Imm, Syn::Reg});
    }
    // Everything else is register operands only, as many as its shape declares. Slice-ness
    // rides kOpcodeTable's slot classes rather than this table, so extract and insert need no
    // entry of their own.
    const ShapeInfo info = shape_info(kOpcodeTable[opcode].shape);
    Pattern p;
    for (std::uint8_t i = 0; i < info.operands; ++i) {
        p.items[p.count++] = Syn::Reg;
    }
    return p;
}

// ---------------------------------------------------------------------------------------
// Mnemonic lookup
// ---------------------------------------------------------------------------------------

const std::map<std::string, std::vector<const MnemonicEntry*>>& mnemonic_index() {
    static const std::map<std::string, std::vector<const MnemonicEntry*>> index = [] {
        std::map<std::string, std::vector<const MnemonicEntry*>> result;
        for (const MnemonicEntry& entry : kMnemonics) {
            result[entry.text].push_back(&entry);
        }
        return result;
    }();
    return index;
}

// ---------------------------------------------------------------------------------------
// Line splitting
// ---------------------------------------------------------------------------------------

// Split one line into whitespace-separated fields, honouring string and character literals so a
// space inside `data_string "a b"` does not split the operand, and dropping a comment that
// starts outside one. A semicolon inside a literal is an ordinary character.
bool split_fields(const std::string& line, std::vector<std::string>& out, std::string& error) {
    std::string current;
    bool in_string = false;
    bool in_char = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (in_string || in_char) {
            current.push_back(c);
            if (c == '\\' && i + 1 < line.size()) {
                current.push_back(line[++i]);
                continue;
            }
            if (in_string && c == '"') in_string = false;
            if (in_char && c == '\'') in_char = false;
            continue;
        }
        if (c == ';') {
            break;  // a comment runs to the end of the line
        }
        if (c == '"') {
            in_string = true;
            current.push_back(c);
            continue;
        }
        if (c == '\'') {
            in_char = true;
            current.push_back(c);
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (in_string || in_char) {
        error = in_string ? "unterminated string literal" : "unterminated character literal";
        return false;
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return true;
}

// Outside a literal or a comment, every character is printable ASCII, a space, or a tab. Any
// other byte is a diagnostic rather than something the lexer quietly passes through.
bool line_characters_are_legal(const std::string& line, std::size_t& offending) {
    bool in_string = false;
    bool in_char = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        if (!in_string && !in_char) {
            if (c == ';') return true;  // the rest is a comment
            if (c == '"') { in_string = true; continue; }
            if (c == '\'') { in_char = true; continue; }
            if (c != '\t' && (c < 0x20 || c > 0x7E)) {
                offending = i;
                return false;
            }
        } else {
            if (c == '\\' && i + 1 < line.size()) { ++i; continue; }
            if (in_string && c == '"') in_string = false;
            if (in_char && c == '\'') in_char = false;
        }
    }
    return true;
}

bool is_directive_name(const std::string& name) {
    static const std::array<const char*, 15> names = {{
        "section", "origin", "align", "data_byte", "data_quarter_word", "data_half_word",
        "data_word", "data_string", "data_string_zero", "data_fill", "reserve", "constant",
        "global", "extern", "include",
    }};
    return std::any_of(names.begin(), names.end(),
                       [&](const char* n) { return name == n; });
}

bool is_identifier(const std::string& text) {
    if (text.empty()) return false;
    if (std::isalpha(static_cast<unsigned char>(text[0])) == 0 && text[0] != '_') return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_';
    });
}

// A target slot's two readings are told apart lexically, and the mandatory base markers are
// what make that safe: a symbol never begins with a base marker and a literal always does. A
// literal in a target slot IS the displacement and is emitted unchanged; anything else names an
// address and the assembler does the subtraction.
bool target_is_literal_displacement(const std::string& text) {
    return !text.empty() && (text[0] == '#' || text[0] == '$' || text[0] == '%');
}

std::uint8_t section_kind_from_name(const std::string& name) {
    if (name == "code") return maize::obj::SEC_CODE;
    if (name == "rodata") return maize::obj::SEC_RODATA;
    if (name == "data") return maize::obj::SEC_DATA;
    if (name == "bss") return maize::obj::SEC_BSS;
    return maize::obj::SEC_NULL;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Operand parsing
// ---------------------------------------------------------------------------------------

bool Assembler::parse_operand(const std::string& field, const SourceLoc& where, Operand& out) {
    out = Operand{};
    out.text = field;

    if (field.empty()) {
        diags_.error(where, "an empty operand");
        return false;
    }

    if (field[0] == '"') {
        if (field.size() < 2 || field.back() != '"') {
            diags_.error(where, "unterminated string literal");
            return false;
        }
        out.kind = OperandKind::StringText;
        const std::string body = field.substr(1, field.size() - 2);
        for (std::size_t i = 0; i < body.size();) {
            if (body[i] == '\\') {
                std::uint8_t decoded = 0;
                if (!decode_escape(body, i, decoded)) {
                    diags_.error(where, "unrecognized escape in string literal '" + field + "'");
                    return false;
                }
                out.string_value.push_back(static_cast<char>(decoded));
            } else {
                out.string_value.push_back(body[i++]);
            }
        }
        return true;
    }

    if (field[0] == '@') {
        // A memory operand: the sigil, a register, and optionally a signed displacement whose
        // sign sits outside the expression and governs the whole of it.
        std::size_t split = std::string::npos;
        for (std::size_t i = 1; i < field.size(); ++i) {
            if (field[i] == '+' || field[i] == '-') {
                split = i;
                break;
            }
        }
        const std::string register_text =
            split == std::string::npos ? field.substr(1) : field.substr(1, split - 1);
        std::uint8_t number = 0;
        if (!is_register_name(register_text, number)) {
            diags_.error(where, "'" + register_text + "' in '" + field +
                                    "' is not a register name; a memory operand names a base "
                                    "register after the @ sigil");
            return false;
        }
        out.kind = OperandKind::Memory;
        out.reg = number;
        if (split != std::string::npos) {
            out.has_displacement = true;
            out.displacement_negated = field[split] == '-';
            out.displacement_text = field.substr(split + 1);
            if (out.displacement_text.empty()) {
                diags_.error(where, "'" + field + "' has a sign with no displacement after it");
                return false;
            }
        }
        return true;
    }

    // A slice: a register, a dot, a width letter, and a literal index digit. The dot is not in
    // the identifier alphabet, which is what makes r3.b5 structurally distinct from any label a
    // program can name.
    const std::size_t dot = field.find('.');
    if (dot != std::string::npos) {
        const std::string register_text = field.substr(0, dot);
        std::uint8_t number = 0;
        if (is_register_name(register_text, number)) {
            const std::string suffix = field.substr(dot + 1);
            if (suffix.size() != 2 || std::isdigit(static_cast<unsigned char>(suffix[1])) == 0) {
                diags_.error(where, "'" + field +
                                        "' is not a slice; write a width letter b, q or h and a "
                                        "single index digit, as in r3.b5");
                return false;
            }
            out.kind = OperandKind::Slice;
            out.reg = number;
            out.slice_index = static_cast<std::uint8_t>(suffix[1] - '0');
            switch (suffix[0]) {
                case 'b': out.slice_width = SliceWidth::Byte; break;
                case 'q': out.slice_width = SliceWidth::Quarter; break;
                case 'h': out.slice_width = SliceWidth::Half; break;
                default:
                    diags_.error(where, "'" + field + "' names width '" +
                                            std::string(1, suffix[0]) +
                                            "'; a slice is written .b, .q or .h");
                    return false;
            }
            return true;
        }
    }

    std::uint8_t number = 0;
    if (is_register_name(field, number)) {
        out.kind = OperandKind::Register;
        out.reg = number;
        return true;
    }

    if (field == "code" || field == "rodata" || field == "data" || field == "bss") {
        out.kind = OperandKind::SectionKind;
        out.section_kind = section_kind_from_name(field);
        return true;
    }

    out.kind = OperandKind::Expression;
    out.expression_text = field;
    return true;
}

// ---------------------------------------------------------------------------------------
// Reading and parsing
// ---------------------------------------------------------------------------------------

bool Assembler::read_source(const std::string& path, std::string& out,
                            const SourceLoc& referenced_from) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        diags_.error(referenced_from, "cannot read '" + path + "'");
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

void Assembler::parse_text(const std::string& text, const std::string& file_name,
                           const std::string& dir, std::shared_ptr<SourceLoc> included_from) {
    std::istringstream stream(text);
    std::string line;
    int line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        // A carriage return immediately before the line feed is discarded, so a file written on
        // either host convention assembles identically.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        SourceLoc where{file_name, line_number, included_from};
        parse_line(line, where, dir, included_from);
    }
}

void Assembler::parse_line(const std::string& line, const SourceLoc& where, const std::string& dir,
                           std::shared_ptr<SourceLoc> included_from) {
    std::size_t offending = 0;
    if (!line_characters_are_legal(line, offending)) {
        std::ostringstream message;
        message << "byte 0x" << std::hex << std::uppercase
                << static_cast<unsigned>(static_cast<unsigned char>(line[offending]))
                << " is not a printable ASCII character, a space, or a tab";
        diags_.error(where, message.str());
        return;
    }

    std::vector<std::string> fields;
    std::string error;
    if (!split_fields(line, fields, error)) {
        diags_.error(where, error);
        return;
    }
    if (fields.empty()) {
        return;  // an empty line, or one holding only a comment
    }

    const std::string& head = fields[0];

    // A label definition stands alone on its line.
    if (head.size() >= 2 && head.back() == ':') {
        const std::string name = head.substr(0, head.size() - 1);
        if (fields.size() > 1) {
            diags_.error(where, "a label definition stands alone on its line; '" + fields[1] +
                                    "' follows '" + head + "'");
            return;
        }
        if (!is_identifier(name)) {
            diags_.error(where, "'" + name + "' is not an identifier");
            return;
        }
        if (is_reserved_word(name)) {
            diags_.error(where, "'" + name +
                                    "' is a reserved word and cannot be defined as a label");
            return;
        }
        Statement statement;
        statement.kind = StatementKind::Label;
        statement.where = where;
        statement.name = name;
        statements_.push_back(std::move(statement));
        return;
    }

    Statement statement;
    statement.where = where;
    statement.name = head;
    statement.kind = is_directive_name(head) ? StatementKind::Directive : StatementKind::Instruction;

    for (std::size_t i = 1; i < fields.size(); ++i) {
        Operand operand;
        if (!parse_operand(fields[i], where, operand)) {
            return;
        }
        statement.operands.push_back(std::move(operand));
    }

    // `include` is resolved here rather than in a pass, because it assembles the included file's
    // text at the point of the directive, as though its lines had been written there.
    if (statement.kind == StatementKind::Directive && head == "include") {
        if (statement.operands.size() != 1 ||
            statement.operands[0].kind != OperandKind::StringText) {
            diags_.error(where, "include takes exactly one string literal naming a path");
            return;
        }
        // A relative path resolves against the directory of the file containing the directive,
        // never against the working directory, so a source tree moves without breaking.
        std::filesystem::path target(statement.operands[0].string_value);
        if (target.is_relative()) {
            target = std::filesystem::path(dir) / target;
        }
        std::error_code ec;
        std::filesystem::path canonical = std::filesystem::weakly_canonical(target, ec);
        // generic_string, not string: this value is both the cycle-detection key and the file
        // name every diagnostic from the included text prints. Taking the host's native
        // separator here would make one diagnostic chain mix slashes, and, worse, it would make
        // this key differ from the one assemble_file computes for the top-level file on a host
        // whose native separator is a backslash, so a file that includes itself would not be
        // recognized as the cycle it is. Both sites normalize the same way for that reason.
        const std::string key = ec ? target.generic_string() : canonical.generic_string();

        if (include_stack_.count(key) != 0) {
            std::ostringstream cycle;
            cycle << "include cycle: ";
            for (const std::string& entry : include_order_) {
                cycle << entry << " -> ";
            }
            cycle << key;
            diags_.error(where, cycle.str());
            return;
        }

        std::string included_text;
        if (!read_source(key, included_text, where)) {
            return;
        }
        include_stack_.insert(key);
        include_order_.push_back(key);
        auto parent = std::make_shared<SourceLoc>(where);
        parent->included_from = included_from;
        parse_text(included_text, key,
                   std::filesystem::path(key).parent_path().string(), parent);
        include_order_.pop_back();
        include_stack_.erase(key);
        return;
    }

    statements_.push_back(std::move(statement));
}

bool Assembler::assemble_text(const std::string& text, const std::string& name,
                              const std::string& base_path) {
    base_path_ = base_path;
    parse_text(text, name, base_path, nullptr);
    pass_one();
    if (!diags_.any()) {
        pass_two();
    }
    return !diags_.any();
}

bool Assembler::assemble_file(const std::string& path) {
    SourceLoc origin{path, 0, nullptr};
    std::string text;
    if (!read_source(path, text, origin)) {
        return false;
    }
    const std::string dir = std::filesystem::path(path).parent_path().string();
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    // Normalized the same way the include directive normalizes, and for the two reasons stated
    // there: one diagnostic chain reads with one separator, and a file that includes itself is
    // recognized as a cycle rather than as two different files that happen to have the same
    // contents.
    const std::string key = ec ? path : canonical.generic_string();
    include_stack_.insert(key);
    return assemble_text(text, key, dir);
}

// ---------------------------------------------------------------------------------------
// Emission helpers
// ---------------------------------------------------------------------------------------

std::uint64_t Assembler::current_address() const { return address_; }

void Assembler::emit_byte(std::uint8_t value) {
    if (second_pass_) {
        if (mode_ == PlacementMode::Sectioned) {
            std::vector<std::uint8_t>& target = sections_[current_section_];
            const std::uint64_t index = address_;
            if (target.size() < index) {
                target.resize(static_cast<std::size_t>(index), 0);
            }
            if (target.size() == index) {
                target.push_back(value);
            } else {
                target[static_cast<std::size_t>(index)] = value;
            }
        } else {
            const std::uint64_t index = address_ - flat_base_;
            if (flat_image_.size() < index) {
                flat_image_.resize(static_cast<std::size_t>(index), 0);
            }
            if (flat_image_.size() == index) {
                flat_image_.push_back(value);
            } else {
                flat_image_[static_cast<std::size_t>(index)] = value;
            }
        }
    }
    ++address_;
}

void Assembler::emit_bytes(const std::uint8_t* data, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        emit_byte(data[i]);
    }
}

void Assembler::emit_immediate(std::uint64_t value, unsigned bytes) {
    // Every immediate occupies a whole number of bytes and is stored little-endian.
    for (unsigned i = 0; i < bytes; ++i) {
        emit_byte(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
    }
}

void Assembler::reserve_space(std::uint64_t count) { address_ += count; }

const std::vector<std::uint8_t>& Assembler::section_bytes(std::uint8_t section) const {
    return sections_[section];
}

std::uint64_t Assembler::section_size(std::uint8_t section) const {
    return section_sizes_[section];
}

// ---------------------------------------------------------------------------------------
// Pass 1: addresses
// ---------------------------------------------------------------------------------------

void Assembler::pass_one() {
    second_pass_ = false;
    std::array<std::uint64_t, 5> counters{};
    address_ = 0;
    current_section_ = 0;
    bool flat_base_pending = true;

    for (Statement& statement : statements_) {
        statement.section = current_section_;

        if (statement.kind == StatementKind::Label) {
            statement.address = address_;
            Symbol& symbol = symbols_[statement.name];
            if (symbol.defined) {
                diags_.error(statement.where,
                             "'" + statement.name + "' is already defined");
                continue;
            }
            symbol.name = statement.name;
            symbol.value = address_;
            symbol.section = current_section_;
            symbol.defined = true;
            symbol.where = statement.where;
            continue;
        }

        if (statement.kind == StatementKind::Directive) {
            const std::string& name = statement.name;

            if (name == "section") {
                if (mode_ == PlacementMode::Flat) {
                    diags_.error(statement.where,
                                 "a module cannot mix origin and section; both answer the same "
                                 "placement question");
                    continue;
                }
                if (statement.operands.size() != 1 ||
                    statement.operands[0].kind != OperandKind::SectionKind) {
                    diags_.error(statement.where,
                                 "section takes one of code, rodata, data or bss");
                    continue;
                }
                mode_ = PlacementMode::Sectioned;
                counters[current_section_] = address_;
                current_section_ = statement.operands[0].section_kind;
                address_ = counters[current_section_];
                statement.section = current_section_;
                continue;
            }

            if (name == "origin") {
                if (mode_ == PlacementMode::Sectioned) {
                    diags_.error(statement.where,
                                 "a module cannot mix origin and section; both answer the same "
                                 "placement question");
                    continue;
                }
                mode_ = PlacementMode::Flat;
                ExprValue value;
                if (statement.operands.size() != 1 ||
                    statement.operands[0].kind != OperandKind::Expression ||
                    !evaluate(statement.operands[0].expression_text, statement.where, value)) {
                    if (statement.operands.size() == 1 &&
                        statement.operands[0].kind == OperandKind::Expression) {
                        continue;  // evaluate already reported
                    }
                    diags_.error(statement.where, "origin takes one constant expression");
                    continue;
                }
                if (value.is_relocatable()) {
                    diags_.error(statement.where, "origin takes a constant expression");
                    continue;
                }
                address_ = value.constant;
                if (flat_base_pending) {
                    flat_base_ = address_;
                    flat_base_set_ = true;
                    flat_base_pending = false;
                } else if (address_ < flat_base_) {
                    diags_.error(statement.where,
                                 "origin moves below the start of the image");
                    continue;
                }
                statement.address = address_;
                continue;
            }

            statement.address = address_;
            if (mode_ == PlacementMode::Undecided && name != "constant" && name != "global" &&
                name != "extern") {
                mode_ = PlacementMode::Flat;
            }
            if (flat_base_pending && mode_ == PlacementMode::Flat) {
                flat_base_ = address_;
                flat_base_set_ = true;
                flat_base_pending = false;
            }

            // A `constant` binds before it is used, so it is evaluated on this walk rather than
            // waiting for pass 2. That restriction costs nothing in practice and it makes a
            // cyclic definition impossible to write.
            if (name == "constant") {
                if (statement.operands.size() != 2) {
                    diags_.error(statement.where,
                                 "constant takes an identifier and a constant expression");
                    continue;
                }
                const std::string& symbol_name = statement.operands[0].text;
                if (statement.operands[0].kind == OperandKind::Register ||
                    is_reserved_word(symbol_name)) {
                    diags_.error(statement.where,
                                 "'" + symbol_name +
                                     "' is a reserved word and cannot be defined as a constant");
                    continue;
                }
                if (!is_identifier(symbol_name)) {
                    diags_.error(statement.where, "'" + symbol_name + "' is not an identifier");
                    continue;
                }
                if (symbols_.count(symbol_name) != 0 && symbols_[symbol_name].defined) {
                    diags_.error(statement.where,
                                 "'" + symbol_name +
                                     "' is already defined; a second value gets a second name");
                    continue;
                }
                ExprValue value;
                if (statement.operands[1].kind != OperandKind::Expression) {
                    diags_.error(statement.where, "constant takes a constant expression");
                    continue;
                }
                if (!evaluate(statement.operands[1].expression_text, statement.where, value)) {
                    continue;
                }
                if (value.is_relocatable()) {
                    diags_.error(statement.where,
                                 "constant takes a constant expression, and '" +
                                     statement.operands[1].text + "' is relocatable");
                    continue;
                }
                Symbol& symbol = symbols_[symbol_name];
                symbol.name = symbol_name;
                symbol.value = value.constant;
                symbol.defined = true;
                symbol.is_constant = true;
                symbol.section = maize::obj::SEC_NULL;
                symbol.where = statement.where;
                continue;
            }

            if (name == "global" || name == "extern") {
                if (statement.operands.size() != 1) {
                    diags_.error(statement.where, name + " takes one identifier");
                    continue;
                }
                const std::string& symbol_name = statement.operands[0].text;
                if (!is_identifier(symbol_name) || is_reserved_word(symbol_name)) {
                    diags_.error(statement.where, "'" + symbol_name + "' is not a symbol name");
                    continue;
                }
                Symbol& symbol = symbols_[symbol_name];
                symbol.name = symbol_name;
                if (name == "global") {
                    symbol.exported = true;
                } else {
                    symbol.external = true;
                    symbol.section = maize::obj::SEC_NULL;
                }
                continue;
            }

            // The remaining directives all advance the address by an amount fixed by their
            // operands' syntax, so pass 1 computes the same length pass 2 emits.
            std::uint64_t size = 0;
            if (!directive_size(statement, size)) {
                continue;
            }
            address_ += size;
            continue;
        }

        // An instruction. Its length is a pure function of the mnemonic and the operand syntax
        // alone, so the opcode is resolved here and the address never has to be revised.
        statement.address = address_;
        if (mode_ == PlacementMode::Undecided) {
            mode_ = PlacementMode::Flat;
        }
        if (flat_base_pending && mode_ == PlacementMode::Flat) {
            flat_base_ = address_;
            flat_base_set_ = true;
            flat_base_pending = false;
        }
        if (mode_ == PlacementMode::Sectioned && current_section_ != maize::obj::SEC_CODE) {
            diags_.error(statement.where, "instructions are legal in the code section only");
            continue;
        }
        std::uint8_t opcode = 0;
        if (!resolve_opcode(statement, opcode)) {
            continue;
        }
        statement.opcode = opcode;
        statement.length = kOpcodeTable[opcode].length;
        address_ += statement.length;
    }

    counters[current_section_] = address_;
    for (int i = 0; i < 5; ++i) {
        section_sizes_[static_cast<std::size_t>(i)] = counters[static_cast<std::size_t>(i)];
    }
    if (mode_ == PlacementMode::Undecided) {
        mode_ = PlacementMode::Flat;
    }
    if (flat_base_pending) {
        flat_base_ = 0;
    }
}

// ---------------------------------------------------------------------------------------
// Pass 2: bytes
// ---------------------------------------------------------------------------------------

void Assembler::pass_two() {
    second_pass_ = true;
    std::array<std::uint64_t, 5> counters{};
    address_ = mode_ == PlacementMode::Flat ? flat_base_ : 0;
    current_section_ = 0;

    for (Statement& statement : statements_) {
        switch (statement.kind) {
            case StatementKind::Label:
                break;
            case StatementKind::Directive:
                if (statement.name == "section") {
                    counters[current_section_] = address_;
                    current_section_ = statement.section;
                    address_ = counters[current_section_];
                } else if (statement.name == "origin") {
                    address_ = statement.address;
                } else {
                    emit_directive(statement);
                }
                break;
            case StatementKind::Instruction:
                if (statement.length != 0) {
                    encode_instruction(statement);
                }
                break;
        }
    }
}

// ---------------------------------------------------------------------------------------
// Mnemonic selection
// ---------------------------------------------------------------------------------------

bool Assembler::resolve_opcode(Statement& statement, std::uint8_t& out) {
    const auto& index = mnemonic_index();
    const auto it = index.find(statement.name);
    if (it == index.end()) {
        diags_.error(statement.where, "'" + statement.name +
                                          "' is not a mnemonic or a directive");
        return false;
    }
    const std::vector<const MnemonicEntry*>& candidates = it->second;
    if (candidates.size() == 1) {
        // The bare `move` is register-to-register and nothing else, so an immediate written
        // here is not a narrower encoding chosen by inference; it is a missing width.
        if (statement.name == "move" && !statement.operands.empty() &&
            statement.operands[0].kind == OperandKind::Expression) {
            diags_.error(statement.where,
                         "an immediate move names its width: write move.zb, move.sb, move.zq, "
                         "move.sq, move.zh, move.sh or move.w rather than a bare move");
            return false;
        }
        out = candidates[0]->opcode;
        return true;
    }

    // Two candidates: the operand syntax picks which encoding is emitted, and that is the whole
    // of the sanctioned mnemonic selection. Selection never changes the operation, never
    // changes the number of instructions, and never changes what the source says.
    const auto has = [&](Select select) {
        return std::any_of(candidates.begin(), candidates.end(),
                           [&](const MnemonicEntry* e) { return e->select == select; });
    };
    Select wanted = Select::None;

    if (has(Select::Bare) && has(Select::Displaced)) {
        const auto memory = std::find_if(
            statement.operands.begin(), statement.operands.end(),
            [](const Operand& o) { return o.kind == OperandKind::Memory; });
        if (memory == statement.operands.end()) {
            diags_.error(statement.where, "'" + statement.name +
                                              "' takes a memory operand written @rb or @rb+$disp");
            return false;
        }
        wanted = memory->has_displacement ? Select::Displaced : Select::Bare;
    } else if (has(Select::RegForm) && has(Select::ImmForm)) {
        if (statement.operands.size() < 2) {
            diags_.error(statement.where, "'" + statement.name + "' takes three operands");
            return false;
        }
        wanted = statement.operands[1].kind == OperandKind::Expression ? Select::ImmForm
                                                                       : Select::RegForm;
    } else if (has(Select::RegTarget) && has(Select::DispTarget)) {
        if (statement.operands.size() != 1) {
            diags_.error(statement.where, "'" + statement.name + "' takes one operand");
            return false;
        }
        wanted = statement.operands[0].kind == OperandKind::Register ? Select::RegTarget
                                                                     : Select::DispTarget;
    } else {
        diags_.error(statement.where,
                     "internal: '" + statement.name + "' has siblings with no selection rule");
        return false;
    }

    for (const MnemonicEntry* entry : candidates) {
        if (entry->select == wanted) {
            out = entry->opcode;
            return true;
        }
    }
    diags_.error(statement.where, "no encoding of '" + statement.name + "' takes these operands");
    return false;
}

// ---------------------------------------------------------------------------------------
// Directive sizes
// ---------------------------------------------------------------------------------------

namespace {

// How many bytes one operand of a data directive emits, or zero when the directive is not one
// of the four width directives.
unsigned data_directive_width(const std::string& name) {
    if (name == "data_byte") return 1;
    if (name == "data_quarter_word") return 2;
    if (name == "data_half_word") return 4;
    if (name == "data_word") return 8;
    return 0;
}

}  // namespace

bool Assembler::directive_size(Statement& statement, std::uint64_t& out) {
    const std::string& name = statement.name;
    out = 0;

    const bool in_bss =
        mode_ == PlacementMode::Sectioned && current_section_ == maize::obj::SEC_BSS;
    if (in_bss && name != "reserve" && name != "align" && name != "constant" &&
        name != "global" && name != "extern") {
        diags_.error(statement.where,
                     "the bss section holds no emitted bytes, so reserve and align are the only "
                     "directives legal inside it");
        return false;
    }

    if (const unsigned width = data_directive_width(name); width != 0) {
        if (statement.operands.empty()) {
            diags_.error(statement.where, name + " takes one or more expressions");
            return false;
        }
        out = static_cast<std::uint64_t>(statement.operands.size()) * width;
        return true;
    }

    if (name == "data_string" || name == "data_string_zero") {
        if (statement.operands.size() != 1 ||
            statement.operands[0].kind != OperandKind::StringText) {
            diags_.error(statement.where, name + " takes exactly one string literal");
            return false;
        }
        out = statement.operands[0].string_value.size() + (name == "data_string_zero" ? 1 : 0);
        return true;
    }

    if (name == "data_fill" || name == "reserve" || name == "align") {
        if (statement.operands.empty() ||
            statement.operands[0].kind != OperandKind::Expression) {
            diags_.error(statement.where, name + " takes a constant expression");
            return false;
        }
        ExprValue count;
        if (!evaluate(statement.operands[0].expression_text, statement.where, count)) {
            return false;
        }
        if (count.is_relocatable()) {
            diags_.error(statement.where, name + " takes a constant expression");
            return false;
        }
        if (name == "align") {
            if (statement.operands.size() != 1) {
                diags_.error(statement.where, "align takes one constant expression");
                return false;
            }
            const std::uint64_t alignment = count.constant;
            if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
                diags_.error(statement.where,
                             "an alignment is a power of two and is never zero");
                return false;
            }
            const std::uint64_t remainder = address_ % alignment;
            out = remainder == 0 ? 0 : alignment - remainder;
            return true;
        }
        if (name == "reserve") {
            if (statement.operands.size() != 1) {
                diags_.error(statement.where, "reserve takes one constant expression");
                return false;
            }
            if (mode_ == PlacementMode::Sectioned && current_section_ != maize::obj::SEC_BSS) {
                diags_.error(statement.where,
                             "reserve names storage in a bss section; a section that carries "
                             "bytes cannot use it");
                return false;
            }
            out = count.constant;
            return true;
        }
        // data_fill takes a count expression and a value expression.
        if (statement.operands.size() != 2) {
            diags_.error(statement.where, "data_fill takes a count and a value");
            return false;
        }
        out = count.constant;
        return true;
    }

    diags_.error(statement.where, "'" + name + "' is not a directive");
    return false;
}

// ---------------------------------------------------------------------------------------
// Relocations
// ---------------------------------------------------------------------------------------

void Assembler::add_relocation(std::uint64_t offset, const std::string& symbol, std::uint8_t type,
                               std::int64_t addend, const SourceLoc& where) {
    if (mode_ != PlacementMode::Sectioned) {
        // A flat image is loaded as it stands, so there is no linker to resolve anything and no
        // honest way to leave a placeholder behind.
        diags_.error(where, "'" + symbol +
                                "' needs a relocation, which only a section-mode module can "
                                "carry; a flat image is loaded exactly as it is assembled");
        return;
    }
    Relocation relocation;
    relocation.section = current_section_;
    relocation.offset = offset;
    relocation.symbol = symbol;
    relocation.type = type;
    relocation.addend = addend;
    relocations_.push_back(std::move(relocation));
}

// ---------------------------------------------------------------------------------------
// Instruction encoding
// ---------------------------------------------------------------------------------------

void Assembler::encode_instruction(Statement& statement) {
    const std::uint8_t opcode = statement.opcode;
    const OpcodeInfo& info = kOpcodeTable[opcode];
    const ShapeInfo shape = shape_info(info.shape);
    const Pattern pattern = pattern_for(opcode);

    if (statement.operands.size() != pattern.count) {
        std::ostringstream message;
        message << "'" << statement.name << "' takes " << static_cast<int>(pattern.count)
                << " operand" << (pattern.count == 1 ? "" : "s") << ", not "
                << statement.operands.size();
        diags_.error(statement.where, message.str());
        return;
    }

    std::vector<std::uint8_t> operand_bytes;
    struct PendingImmediate {
        std::uint64_t value = 0;
        unsigned bytes = 0;
    };
    std::vector<PendingImmediate> immediates;

    // The offset of the next immediate field, needed for a relocation record. Component order
    // is fixed, so it is the opcode byte plus every operand byte plus every earlier immediate.
    const auto next_immediate_offset = [&]() {
        std::uint64_t offset = statement.address + 1 + shape.operands;
        for (const PendingImmediate& earlier : immediates) {
            offset += earlier.bytes;
        }
        return offset;
    };

    const auto immediate_width = [&](std::size_t which) -> unsigned {
        return which < shape.immediates ? shape.immediate_bytes[which] : 0;
    };

    for (std::uint8_t i = 0; i < pattern.count; ++i) {
        const Operand& operand = statement.operands[i];
        const Syn syn = pattern.items[i];

        switch (syn) {
            case Syn::Reg: {
                const Slot slot = info.slots[operand_bytes.size()];
                if (slot == Slot::Plain) {
                    if (operand.kind != OperandKind::Register) {
                        if (operand.kind == OperandKind::Slice) {
                            diags_.error(statement.where,
                                         "'" + operand.text +
                                             "' is a slice, and this operand slot names a whole "
                                             "register");
                        } else {
                            diags_.error(statement.where,
                                         "'" + operand.text + "' is not a register");
                        }
                        return;
                    }
                    operand_bytes.push_back(operand.reg);
                    break;
                }
                // A sliced slot is always written sliced. An extract source and an insert
                // destination take no shorthand: extract.zb r3 r7 is a diagnostic rather than a
                // spelling of byte 0.
                if (operand.kind != OperandKind::Slice) {
                    diags_.error(statement.where,
                                 "'" + operand.text +
                                     "' names a whole register, and this operand slot is sliced; "
                                     "write the element, as in r3.b5");
                    return;
                }
                const char* expected = "";
                unsigned limit = 0;
                SliceWidth wanted = SliceWidth::Byte;
                switch (slot) {
                    case Slot::ByteSliced: expected = "b"; limit = 7; wanted = SliceWidth::Byte; break;
                    case Slot::QuarterSliced: expected = "q"; limit = 3; wanted = SliceWidth::Quarter; break;
                    case Slot::HalfSliced: expected = "h"; limit = 1; wanted = SliceWidth::Half; break;
                    default: break;
                }
                if (operand.slice_width != wanted) {
                    diags_.error(statement.where, "'" + operand.text + "' is sliced at the wrong "
                                                  "width; this slot takes ." +
                                                      std::string(expected));
                    return;
                }
                if (operand.slice_index > limit) {
                    std::ostringstream message;
                    message << "'" << operand.text << "' names element "
                            << static_cast<int>(operand.slice_index) << ", and this slot admits 0 through "
                            << limit;
                    diags_.error(statement.where, message.str());
                    return;
                }
                operand_bytes.push_back(
                    static_cast<std::uint8_t>(operand.reg | (operand.slice_index << 5)));
                break;
            }

            case Syn::MemBare:
            case Syn::MemDisp: {
                if (operand.kind != OperandKind::Memory) {
                    diags_.error(statement.where,
                                 "'" + operand.text +
                                     "' is not a memory operand; write @rb or @rb+$disp");
                    return;
                }
                if (syn == Syn::MemBare && operand.has_displacement) {
                    diags_.error(statement.where,
                                 "'" + operand.text + "' carries a displacement this form has no "
                                                      "field for");
                    return;
                }
                operand_bytes.push_back(operand.reg);
                if (syn == Syn::MemDisp) {
                    const unsigned bytes = immediate_width(immediates.size());
                    ExprValue value;
                    if (!evaluate(operand.displacement_text, statement.where, value)) {
                        return;
                    }
                    if (value.is_relocatable()) {
                        diags_.error(statement.where,
                                     "a memory displacement takes a constant expression");
                        return;
                    }
                    std::uint64_t displacement = value.constant;
                    if (operand.displacement_negated) {
                        displacement =
                            static_cast<std::uint64_t>(-static_cast<std::int64_t>(displacement));
                    }
                    if (!fits(displacement, bytes * 8)) {
                        diags_.error(statement.where,
                                     "the displacement in '" + operand.text +
                                         "' does not fit the " + std::to_string(bytes * 8) +
                                         "-bit field, and the assembler never truncates one");
                        return;
                    }
                    immediates.push_back({displacement, bytes});
                }
                break;
            }

            case Syn::Imm: {
                if (operand.kind != OperandKind::Expression) {
                    diags_.error(statement.where,
                                 "'" + operand.text +
                                     "' is not an expression; this operand is an immediate");
                    return;
                }
                const unsigned bytes = immediate_width(immediates.size());
                ExprValue value;
                if (!evaluate(operand.expression_text, statement.where, value)) {
                    return;
                }
                if (value.is_relocatable()) {
                    // Exactly one instruction immediate outside the target slots accepts a
                    // relocatable expression, and it is move.w's, because absolute relocations
                    // exist at 32 and 64 bits and nowhere narrower.
                    diags_.error(statement.where,
                                 "'" + operand.text +
                                     "' is relocatable, and only move.w's immediate and a target "
                                     "slot accept one");
                    return;
                }
                if (!fits(value.constant, bytes * 8)) {
                    diags_.error(statement.where,
                                 "'" + operand.text + "' does not fit the " +
                                     std::to_string(bytes * 8) +
                                     "-bit field, and the assembler never truncates a literal to "
                                     "make it fit");
                    return;
                }
                immediates.push_back({value.constant, bytes});
                break;
            }

            case Syn::ImmAbs: {
                if (operand.kind != OperandKind::Expression) {
                    diags_.error(statement.where,
                                 "'" + operand.text + "' is not an expression; move.w takes a "
                                                      "64-bit immediate");
                    return;
                }
                const unsigned bytes = immediate_width(immediates.size());
                ExprValue value;
                if (!evaluate(operand.expression_text, statement.where, value)) {
                    return;
                }
                if (value.is_relocatable()) {
                    add_relocation(next_immediate_offset(), value.symbol,
                                   maize::obj::R_MAIZE_ABS64,
                                   static_cast<std::int64_t>(value.constant), statement.where);
                    immediates.push_back({0, bytes});
                    break;
                }
                immediates.push_back({value.constant, bytes});
                break;
            }

            case Syn::Target: {
                if (operand.kind != OperandKind::Expression) {
                    diags_.error(statement.where,
                                 "'" + operand.text + "' is not a target");
                    return;
                }
                const unsigned bytes = immediate_width(immediates.size());
                if (target_is_literal_displacement(operand.expression_text)) {
                    // A numeric literal in a target slot IS the displacement and is emitted
                    // unchanged, which is how a program written against a fixed layout, or a
                    // test probing a particular encoding, says so.
                    ExprValue value;
                    if (!evaluate(operand.expression_text, statement.where, value)) {
                        return;
                    }
                    if (!fits(value.constant, bytes * 8)) {
                        diags_.error(statement.where,
                                     "'" + operand.text + "' does not fit the " +
                                         std::to_string(bytes * 8) + "-bit displacement field");
                        return;
                    }
                    immediates.push_back({value.constant, bytes});
                    break;
                }
                ExprValue value;
                if (!evaluate(operand.expression_text, statement.where, value)) {
                    return;
                }
                if (value.is_relocatable()) {
                    const auto symbol = symbols_.find(value.symbol);
                    const bool same_section = symbol != symbols_.end() && symbol->second.defined &&
                                              symbol->second.section == current_section_;
                    if (!same_section) {
                        // A target that is extern, or that lives in another section, emits a
                        // placeholder of zero and a program-counter-relative relocation.
                        add_relocation(next_immediate_offset(), value.symbol,
                                       maize::obj::R_MAIZE_REL32,
                                       static_cast<std::int64_t>(value.constant), statement.where);
                        immediates.push_back({0, bytes});
                        break;
                    }
                    value.constant += symbol->second.value;
                    value.symbol.clear();
                }
                // Source is written in addresses and the assembler does the subtraction, from
                // the address of the instruction that follows this one.
                const std::int64_t displacement =
                    static_cast<std::int64_t>(value.constant) -
                    static_cast<std::int64_t>(statement.address + statement.length);
                if (displacement < -2147483648LL || displacement > 2147483647LL) {
                    diags_.error(statement.where,
                                 "the displacement to '" + operand.text +
                                     "' does not fit a signed 32-bit field, and the assembler "
                                     "does not rewrite one instruction into several");
                    return;
                }
                immediates.push_back({static_cast<std::uint64_t>(displacement), bytes});
                break;
            }
        }
    }

    if (operand_bytes.size() != shape.operands || immediates.size() != shape.immediates) {
        diags_.error(statement.where,
                     "internal: the assembly form of '" + statement.name +
                         "' does not match the shape its opcode declares");
        return;
    }

    emit_byte(opcode);
    for (const std::uint8_t byte : operand_bytes) {
        emit_byte(byte);
    }
    for (const PendingImmediate& immediate : immediates) {
        emit_immediate(immediate.value, immediate.bytes);
    }
}

// ---------------------------------------------------------------------------------------
// Directive emission
// ---------------------------------------------------------------------------------------

void Assembler::emit_directive(Statement& statement) {
    const std::string& name = statement.name;

    if (name == "constant" || name == "global" || name == "extern") {
        return;  // bindings and linkage, settled in pass 1 and emitting nothing
    }

    if (name == "align") {
        ExprValue alignment;
        if (!evaluate(statement.operands[0].expression_text, statement.where, alignment)) {
            return;
        }
        const std::uint64_t remainder = address_ % alignment.constant;
        const std::uint64_t padding = remainder == 0 ? 0 : alignment.constant - remainder;
        // Padding in the code section is a run of nop, which is exact because nop is one byte.
        // Padding in data and rodata is zero bytes. In bss align emits nothing and only
        // advances the address, which keeps the section true to holding no emitted bytes. A
        // flat module declares no section and is a directly loadable image, so it pads the way
        // code does.
        const bool code_like = mode_ == PlacementMode::Flat ||
                               current_section_ == maize::obj::SEC_CODE;
        if (mode_ == PlacementMode::Sectioned && current_section_ == maize::obj::SEC_BSS) {
            reserve_space(padding);
            return;
        }
        for (std::uint64_t i = 0; i < padding; ++i) {
            emit_byte(code_like ? op::kNop : 0);
        }
        return;
    }

    if (name == "reserve") {
        ExprValue count;
        if (!evaluate(statement.operands[0].expression_text, statement.where, count)) {
            return;
        }
        reserve_space(count.constant);
        return;
    }

    if (name == "data_string" || name == "data_string_zero") {
        const std::string& text = statement.operands[0].string_value;
        emit_bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
        if (name == "data_string_zero") {
            emit_byte(0);
        }
        return;
    }

    if (name == "data_fill") {
        ExprValue count;
        ExprValue value;
        if (!evaluate(statement.operands[0].expression_text, statement.where, count) ||
            !evaluate(statement.operands[1].expression_text, statement.where, value)) {
            return;
        }
        if (value.is_relocatable() || !fits(value.constant, 8)) {
            diags_.error(statement.where,
                         "a data_fill value accepts the byte range, -128 through 255");
            return;
        }
        for (std::uint64_t i = 0; i < count.constant; ++i) {
            emit_byte(static_cast<std::uint8_t>(value.constant & 0xFF));
        }
        return;
    }

    const unsigned width = data_directive_width(name);
    if (width == 0) {
        diags_.error(statement.where, "'" + name + "' is not a directive");
        return;
    }
    for (const Operand& operand : statement.operands) {
        if (operand.kind != OperandKind::Expression) {
            diags_.error(statement.where, "'" + operand.text + "' is not an expression");
            return;
        }
        ExprValue value;
        if (!evaluate(operand.expression_text, statement.where, value)) {
            return;
        }
        if (value.is_relocatable()) {
            // A relocatable expression is accepted by data_half_word and data_word and emits a
            // placeholder plus an absolute relocation of that width. The narrower two take
            // constant expressions only, since no relocation is defined at those widths.
            if (width != 4 && width != 8) {
                diags_.error(statement.where,
                             "'" + operand.text + "' is relocatable, and no relocation is defined "
                                                  "at " + std::to_string(width * 8) + " bits");
                return;
            }
            add_relocation(address_, value.symbol,
                           width == 4 ? maize::obj::R_MAIZE_ABS32 : maize::obj::R_MAIZE_ABS64,
                           static_cast<std::int64_t>(value.constant), statement.where);
            emit_immediate(0, width);
            continue;
        }
        if (!fits(value.constant, width * 8)) {
            diags_.error(statement.where,
                         "'" + operand.text + "' does not fit the " +
                             std::to_string(width * 8) +
                             "-bit field this directive emits, and the assembler never "
                             "truncates one");
            return;
        }
        emit_immediate(value.constant, width);
    }
}

}  // namespace maize::v2::asmr
