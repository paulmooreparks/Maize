// mzasm.h (maize-422): the shared types of the Maize v2 assembler.
//
// docs/spec-v2/assembler.md is normative and this header does not restate its grammar. What it
// fixes is the shape of the implementation: the source location a diagnostic reports, the
// operand forms the parser produces, the symbol table, and the two-pass structure.
//
// Two passes over one parse. The whole module is parsed once into a statement list, pass 1
// assigns every label an address, and pass 2 evaluates expressions and emits bytes. That split
// is safe, and it is safe for a reason the encoding chapter states rather than one this
// assembler arranges: instruction length is a pure function of the mnemonic and the operand
// syntax alone (instruction-encoding.md invariants 2 and 3), so no forward reference to an
// as-yet-unknown label can change a length and no address assigned in pass 1 is ever revised.
// `constant` is the exception that proves it: assembler.md requires a constant to be defined
// before it is used, so constants resolve inline during the single left-to-right walk and never
// wait for pass 2.

#ifndef MAIZE_V2_MZASM_H
#define MAIZE_V2_MZASM_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mnemonic_v2.h"
#include "opcode_v2.h"

namespace maize::v2::asmr {

// ---------------------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------------------

// Where a token came from. `include` stacking means a location can have a parent: assembler.md
// requires the diagnostic for an error inside an included file to name the included file and
// its line, then the including file and its line.
struct SourceLoc {
    std::string file;
    int line = 0;
    std::shared_ptr<SourceLoc> included_from;
};

struct Diagnostic {
    SourceLoc where;
    std::string message;
};

// Diagnostics accumulate rather than stopping the run: assembler.md requires the assembler to
// continue past one in order to report further ones in the same invocation.
class Diagnostics {
  public:
    void error(const SourceLoc& where, const std::string& message);
    bool any() const { return !entries_.empty(); }
    std::size_t count() const { return entries_.size(); }
    const std::vector<Diagnostic>& entries() const { return entries_; }

    // "mzasm: <file>:<line>: error: <msg>", one line per diagnostic, plus one continuation
    // line per enclosing include (D-5, and assembler.md's Inclusion section).
    std::string format() const;

  private:
    std::vector<Diagnostic> entries_;
};

// ---------------------------------------------------------------------------------------
// Expression values
// ---------------------------------------------------------------------------------------

// The value of an evaluated expression. Only four relocatable forms exist (assembler.md
// "Expressions"): a symbol alone, a symbol plus a constant, a symbol minus a constant, and one
// symbol minus another in the same section of the same module, which folds to a constant. That
// is why one symbol name plus one 64-bit addend is the whole of the representation: nothing
// else is expressible, so nothing else needs a field.
struct ExprValue {
    std::uint64_t constant = 0;  // the whole value when symbol is empty, the addend when it is not
    std::string symbol;          // empty for a constant expression
    bool is_relocatable() const { return !symbol.empty(); }
};

// ---------------------------------------------------------------------------------------
// Parsed operands
// ---------------------------------------------------------------------------------------

enum class OperandKind : std::uint8_t {
    Register,     // r5, sp, a0
    Slice,        // r3.b5, r3.q2, r3.h1
    Memory,       // @r9, @sp+slot_a, @r9-#4
    Expression,   // any expression, literal or symbolic
    StringText,   // a string literal, for data_string and include
    SectionKind,  // code, rodata, data, bss
};

// The width letter of a slice, which fixes which slot class the operand may land in.
enum class SliceWidth : std::uint8_t { Byte, Quarter, Half };

struct Operand {
    OperandKind kind = OperandKind::Expression;
    std::string text;  // the operand as written, for diagnostics

    std::uint8_t reg = 0;         // Register, Slice, Memory: the register number
    SliceWidth slice_width = SliceWidth::Byte;
    std::uint8_t slice_index = 0;

    bool has_displacement = false;  // Memory: whether a +/- displacement was written
    bool displacement_negated = false;
    std::string displacement_text;  // Memory: the displacement expression, unevaluated

    std::string expression_text;  // Expression: the expression, unevaluated
    std::string string_value;     // StringText: the decoded bytes
    std::uint8_t section_kind = 0;
};

// ---------------------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------------------

enum class StatementKind : std::uint8_t { Label, Instruction, Directive };

struct Statement {
    StatementKind kind = StatementKind::Instruction;
    SourceLoc where;
    std::string name;  // the label name, the mnemonic, or the directive name
    std::vector<Operand> operands;

    std::uint64_t address = 0;  // assigned in pass 1
    std::uint8_t section = 0;   // the section open at this statement
    std::uint8_t opcode = 0;    // resolved in pass 1 for an instruction
    std::uint8_t length = 0;    // resolved in pass 1
};

// ---------------------------------------------------------------------------------------
// Symbols
// ---------------------------------------------------------------------------------------

struct Symbol {
    std::string name;
    std::uint64_t value = 0;
    std::uint8_t section = 0;
    bool defined = false;
    bool exported = false;   // named by `global`
    bool external = false;   // named by `extern`
    bool is_constant = false;  // bound by `constant` rather than by a label
    SourceLoc where;
};

// ---------------------------------------------------------------------------------------
// Relocations
// ---------------------------------------------------------------------------------------

struct Relocation {
    std::uint8_t section = 0;
    std::uint64_t offset = 0;  // section-relative offset of the field being patched
    std::string symbol;
    std::uint8_t type = 0;      // maize::obj::R_MAIZE_*
    std::int64_t addend = 0;
};

// ---------------------------------------------------------------------------------------
// Reserved words
// ---------------------------------------------------------------------------------------

// Every register name, every calling-convention alias, `here`, and every directive name.
// assembler.md makes these reserved so that the grammar's `register` and `expression`
// alternatives have exactly one derivation per token: a token that spells a register name is a
// register and is never a symbol, which is what keeps `jump a0` from having two readings that
// emit different bytes and shift every address after them.
bool is_register_name(const std::string& text, std::uint8_t& number);
bool is_reserved_word(const std::string& text);
const std::vector<std::string>& all_reserved_words();

// ---------------------------------------------------------------------------------------
// Field-fit, the dual-reading rule
// ---------------------------------------------------------------------------------------

// A field of N bits accepts any value representable in N bits under a signed reading OR under
// an unsigned reading, so its acceptance range runs from -2^(N-1) through 2^N - 1. The bits
// emitted are the low N bits of the two's-complement value, and those bits are identical under
// both readings, so the dual acceptance never leaves the encoded bytes in doubt. A value
// outside the range is a diagnostic and is never truncated to fit.
bool fits(std::uint64_t value, unsigned width_bits);

// ---------------------------------------------------------------------------------------
// The assembler
// ---------------------------------------------------------------------------------------

// How the module places its bytes. A module that declares a `section` is relocatable; a module
// that does not is flat and `origin` sets its address counter. Mixing the two is a diagnostic,
// because both answer the same question and a module using both leaves the answer to whichever
// came last.
enum class PlacementMode : std::uint8_t { Undecided, Flat, Sectioned };

class Assembler {
  public:
    Diagnostics& diagnostics() { return diags_; }
    const Diagnostics& diagnostics() const { return diags_; }

    // Read `path`, assemble it, and leave the result in this object. Returns false if any
    // diagnostic was reported.
    bool assemble_file(const std::string& path);

    // Assemble `text` as though it had been read from `name`, resolving relative include paths
    // against `base_path`.
    bool assemble_text(const std::string& text, const std::string& name,
                       const std::string& base_path);

    PlacementMode mode() const { return mode_; }

    // Flat mode: the image, and the address its first byte occupies.
    const std::vector<std::uint8_t>& flat_image() const { return flat_image_; }
    std::uint64_t flat_base() const { return flat_base_; }

    // Section mode: per-section bytes, the symbol table, and the relocations.
    const std::vector<std::uint8_t>& section_bytes(std::uint8_t section) const;
    std::uint64_t section_size(std::uint8_t section) const;
    const std::map<std::string, Symbol>& symbols() const { return symbols_; }
    const std::vector<Relocation>& relocations() const { return relocations_; }

    // Serialize the section-mode result as a v2 .mzo object (D-3, D-10).
    std::vector<std::uint8_t> serialize_object() const;

  private:
    // --- reading and parsing ---
    bool read_source(const std::string& path, std::string& out, const SourceLoc& referenced_from);
    void parse_text(const std::string& text, const std::string& file_name,
                    const std::string& dir, std::shared_ptr<SourceLoc> included_from);
    void parse_line(const std::string& line, const SourceLoc& where, const std::string& dir,
                    std::shared_ptr<SourceLoc> included_from);
    bool parse_operand(const std::string& field, const SourceLoc& where, Operand& out);

    // --- the two passes ---
    void pass_one();
    void pass_two();

    // --- expression evaluation ---
    bool evaluate(const std::string& text, const SourceLoc& where, ExprValue& out);

    // --- emission helpers ---
    void emit_byte(std::uint8_t value);
    void emit_bytes(const std::uint8_t* data, std::size_t count);
    void emit_immediate(std::uint64_t value, unsigned bytes);
    void reserve_space(std::uint64_t count);
    std::uint64_t current_address() const;

    // Which of a mnemonic's encodings this statement selected, and how many bytes the result
    // occupies. Both are settled in pass 1, because both are pure functions of the mnemonic and
    // the operand syntax, which is exactly what lets pass 1 assign every address.
    bool resolve_opcode(Statement& statement, std::uint8_t& out);
    bool directive_size(Statement& statement, std::uint64_t& out);

    void encode_instruction(Statement& statement);
    void emit_directive(Statement& statement);
    void add_relocation(std::uint64_t offset, const std::string& symbol, std::uint8_t type,
                        std::int64_t addend, const SourceLoc& where);

    Diagnostics diags_;
    PlacementMode mode_ = PlacementMode::Undecided;

    std::vector<Statement> statements_;
    std::map<std::string, Symbol> symbols_;
    std::vector<Relocation> relocations_;
    std::set<std::string> include_stack_;
    std::vector<std::string> include_order_;
    std::string base_path_;

    // Flat mode state.
    std::vector<std::uint8_t> flat_image_;
    std::uint64_t flat_base_ = 0;
    bool flat_base_set_ = false;

    // Section mode state. Indexed by maize::obj::section_kind, so index 0 is unused.
    std::array<std::vector<std::uint8_t>, 5> sections_{};
    std::array<std::uint64_t, 5> section_sizes_{};
    std::uint8_t current_section_ = 0;

    // Pass state.
    std::uint64_t address_ = 0;
    bool second_pass_ = false;
};

}  // namespace maize::v2::asmr

#endif  // MAIZE_V2_MZASM_H
