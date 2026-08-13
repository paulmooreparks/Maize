// mzasm_lexer.cpp (maize-422): reserved words, the field-fit rule, and the expression language.
//
// assembler.md's "Expressions" section fixes both halves of what this file does. An expression
// contains no whitespace, because whitespace ends an operand, and that single rule is what lets
// operands stay comma-free with no ambiguity about where one ends and the next begins. So the
// line splitter hands each operand across as one solid field and the expression parser here
// never has to consider whitespace at all.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

#include "mzasm.h"

namespace maize::v2::asmr {

// ---------------------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------------------

void Diagnostics::error(const SourceLoc& where, const std::string& message) {
    entries_.push_back(Diagnostic{where, message});
}

std::string Diagnostics::format() const {
    std::ostringstream out;
    for (const Diagnostic& d : entries_) {
        out << "mzasm: " << d.where.file << ":" << d.where.line << ": error: " << d.message
            << "\n";
        // assembler.md's Inclusion section: an error inside an included file names the included
        // file and its line, then the including file and its line, so a reader can walk back to
        // the directive that pulled it in.
        for (const SourceLoc* parent = d.where.included_from.get(); parent != nullptr;
             parent = parent->included_from.get()) {
            out << "mzasm: " << parent->file << ":" << parent->line
                << ": note: included from here\n";
        }
    }
    return out.str();
}

// ---------------------------------------------------------------------------------------
// Reserved words
// ---------------------------------------------------------------------------------------

namespace {

// abi.md "Register roles" in full. The three architectural aliases (zero, ra, sp) are in this
// table too, so one lookup answers for every spelling a register has.
struct RegisterAlias {
    const char* name;
    std::uint8_t number;
};

// One entry per register, in register-number order. The three architectural aliases sit here
// too: `zero` is r0, `sp` is r30 and `ra` is r31, so a single lookup answers for every spelling
// a register has and no second table can disagree with this one.
constexpr std::array<RegisterAlias, 32> kRegisterAliases = {{
    {"zero", 0}, {"tp", 1},  {"a0", 2},  {"a1", 3},  {"a2", 4},  {"a3", 5},  {"a4", 6},
    {"a5", 7},   {"a6", 8},  {"a7", 9},  {"t0", 10}, {"t1", 11}, {"t2", 12}, {"t3", 13},
    {"t4", 14},  {"t5", 15}, {"t6", 16}, {"t7", 17}, {"t8", 18}, {"t9", 19}, {"s0", 20},
    {"s1", 21},  {"s2", 22}, {"s3", 23}, {"s4", 24}, {"s5", 25}, {"s6", 26}, {"s7", 27},
    {"s8", 28},  {"fp", 29}, {"sp", 30}, {"ra", 31},
}};

constexpr std::array<const char*, 15> kDirectiveNames = {{
    "section", "origin", "align", "data_byte", "data_quarter_word", "data_half_word",
    "data_word", "data_string", "data_string_zero", "data_fill", "reserve", "constant",
    "global", "extern", "include",
}};

}  // namespace

bool is_register_name(const std::string& text, std::uint8_t& number) {
    // The canonical spelling rN carries no leading zero, so r5 is the register and r05 is a
    // diagnostic. That keeps a register to a single spelling in source, in a listing, and in a
    // text diff.
    if (text.size() >= 2 && text[0] == 'r') {
        const std::string digits = text.substr(1);
        const bool all_digits =
            !digits.empty() && std::all_of(digits.begin(), digits.end(),
                                           [](unsigned char c) { return std::isdigit(c) != 0; });
        if (all_digits && (digits.size() == 1 || digits[0] != '0')) {
            const long value = std::stol(digits);
            if (value >= 0 && value <= 31) {
                number = static_cast<std::uint8_t>(value);
                return true;
            }
        }
    }
    for (const RegisterAlias& alias : kRegisterAliases) {
        if (alias.name[0] != '\0' && text == alias.name) {
            number = alias.number;
            return true;
        }
    }
    return false;
}

const std::vector<std::string>& all_reserved_words() {
    static const std::vector<std::string> words = [] {
        std::vector<std::string> result;
        for (int i = 0; i <= 31; ++i) {
            result.push_back("r" + std::to_string(i));
        }
        for (const RegisterAlias& alias : kRegisterAliases) {
            if (alias.name[0] != '\0') {
                result.emplace_back(alias.name);
            }
        }
        result.emplace_back("here");
        for (const char* directive : kDirectiveNames) {
            result.emplace_back(directive);
        }
        return result;
    }();
    return words;
}

bool is_reserved_word(const std::string& text) {
    std::uint8_t ignored = 0;
    if (is_register_name(text, ignored)) {
        return true;
    }
    if (text == "here") {
        return true;
    }
    for (const char* directive : kDirectiveNames) {
        if (text == directive) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// Field fit
// ---------------------------------------------------------------------------------------

bool fits(std::uint64_t value, unsigned width_bits) {
    if (width_bits >= 64) {
        // A 64-bit field accepts every value an expression can compute, since expression
        // arithmetic is itself 64-bit. There is no value outside the range, so there is no
        // boundary to test and none is claimed.
        return true;
    }
    const std::uint64_t unsigned_max = (std::uint64_t{1} << width_bits) - 1;
    if (value <= unsigned_max) {
        return true;
    }
    // The signed reading. -2^(N-1) through -1 occupy the top of the 64-bit two's-complement
    // space, so the test is against the sign-extended low N bits rather than against a
    // magnitude.
    const std::uint64_t signed_min = ~unsigned_max | (std::uint64_t{1} << (width_bits - 1));
    return value >= signed_min;
}

// ---------------------------------------------------------------------------------------
// The expression parser
// ---------------------------------------------------------------------------------------

namespace {

// A recursive-descent parser over one solid operand field. The precedence table is
// assembler.md's, lowest binding first: | then ^ then & then the shifts then + - then * / then
// the unary operators then grouping.
class ExprParser {
  public:
    ExprParser(const std::string& text, const SourceLoc& where, Diagnostics& diags)
        : text_(text), where_(where), diags_(diags) {}

    bool parse(ExprValue& out) {
        if (!parse_or(out)) {
            return false;
        }
        if (position_ != text_.size()) {
            fail("unexpected '" + text_.substr(position_) + "' in expression");
            return false;
        }
        return true;
    }

    // Set when a leaf named a symbol the module has not defined and has not declared extern.
    bool saw_undeclared_symbol() const { return undeclared_; }

  private:
    void fail(const std::string& message) {
        if (!failed_) {
            diags_.error(where_, message);
            failed_ = true;
        }
    }

    bool at_end() const { return position_ >= text_.size(); }
    char peek(std::size_t ahead = 0) const {
        return position_ + ahead < text_.size() ? text_[position_ + ahead] : '\0';
    }

    bool consume(const char* op) {
        const std::size_t length = std::string(op).size();
        if (text_.compare(position_, length, op) == 0) {
            position_ += length;
            return true;
        }
        return false;
    }

    // A symbol survives only through addition and subtraction, and only in the four forms
    // assembler.md enumerates. Every other operator applied to an unresolved symbol fails here
    // rather than producing a relocation no object format can express.
    bool require_constant(const ExprValue& value, const char* operation) {
        if (value.is_relocatable()) {
            fail(std::string("a symbol cannot be an operand of ") + operation +
                 " (only a symbol alone, a symbol plus or minus a constant, and one symbol "
                 "minus another in the same section are relocatable)");
            return false;
        }
        return true;
    }

    bool parse_or(ExprValue& out) {
        if (!parse_xor(out)) return false;
        while (!at_end() && peek() == '|') {
            ++position_;
            ExprValue rhs;
            if (!parse_xor(rhs)) return false;
            if (!require_constant(out, "|") || !require_constant(rhs, "|")) return false;
            out.constant |= rhs.constant;
        }
        return true;
    }

    bool parse_xor(ExprValue& out) {
        if (!parse_and(out)) return false;
        while (!at_end() && peek() == '^') {
            ++position_;
            ExprValue rhs;
            if (!parse_and(rhs)) return false;
            if (!require_constant(out, "^") || !require_constant(rhs, "^")) return false;
            out.constant ^= rhs.constant;
        }
        return true;
    }

    bool parse_and(ExprValue& out) {
        if (!parse_shift(out)) return false;
        while (!at_end() && peek() == '&') {
            ++position_;
            ExprValue rhs;
            if (!parse_shift(rhs)) return false;
            if (!require_constant(out, "&") || !require_constant(rhs, "&")) return false;
            out.constant &= rhs.constant;
        }
        return true;
    }

    bool parse_shift(ExprValue& out) {
        if (!parse_additive(out)) return false;
        while (!at_end()) {
            const bool left = text_.compare(position_, 2, "<<") == 0;
            const bool right = text_.compare(position_, 2, ">>") == 0;
            if (!left && !right) break;
            position_ += 2;
            ExprValue rhs;
            if (!parse_additive(rhs)) return false;
            const char* name = left ? "<<" : ">>";
            if (!require_constant(out, name) || !require_constant(rhs, name)) return false;
            const unsigned amount = static_cast<unsigned>(rhs.constant & 63);
            if (left) {
                out.constant <<= amount;
            } else {
                // Arithmetic right shift, per the operator table.
                out.constant = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(out.constant) >> amount);
            }
        }
        return true;
    }

    bool parse_additive(ExprValue& out) {
        if (!parse_multiplicative(out)) return false;
        while (!at_end() && (peek() == '+' || peek() == '-')) {
            const bool subtract = peek() == '-';
            ++position_;
            ExprValue rhs;
            if (!parse_multiplicative(rhs)) return false;
            if (!combine_additive(out, rhs, subtract)) return false;
        }
        return true;
    }

    // The one place a symbol may take part in arithmetic, and the place the four relocatable
    // forms are enforced.
    bool combine_additive(ExprValue& out, const ExprValue& rhs, bool subtract) {
        if (!out.is_relocatable() && !rhs.is_relocatable()) {
            out.constant = subtract ? out.constant - rhs.constant : out.constant + rhs.constant;
            return true;
        }
        if (out.is_relocatable() && !rhs.is_relocatable()) {
            out.constant = subtract ? out.constant - rhs.constant : out.constant + rhs.constant;
            return true;
        }
        if (!out.is_relocatable() && rhs.is_relocatable()) {
            if (subtract) {
                fail("a constant minus a symbol is not a relocatable form");
                return false;
            }
            out.symbol = rhs.symbol;
            out.constant += rhs.constant;
            return true;
        }
        // Both are symbolic: legal only as one symbol minus another defined in the same section
        // of the same module, which folds to a constant.
        if (!subtract) {
            fail("two symbols cannot be added");
            return false;
        }
        return fold_symbol_difference(out, rhs);
    }

    bool fold_symbol_difference(ExprValue& out, const ExprValue& rhs);

    bool parse_multiplicative(ExprValue& out) {
        if (!parse_unary(out)) return false;
        while (!at_end() && (peek() == '*' || peek() == '/')) {
            const bool divide = peek() == '/';
            ++position_;
            ExprValue rhs;
            if (!parse_unary(rhs)) return false;
            const char* name = divide ? "/" : "*";
            if (!require_constant(out, name) || !require_constant(rhs, name)) return false;
            if (divide) {
                if (rhs.constant == 0) {
                    fail("division by zero in expression");
                    return false;
                }
                const std::uint64_t signed_min = std::uint64_t{1} << 63;
                if (out.constant == signed_min && rhs.constant == ~std::uint64_t{0}) {
                    // -2^63 / -1 is the one quotient truncating signed division cannot
                    // represent. Expression arithmetic is 64-bit two's complement and wraps on
                    // overflow (assembler.md, "Expressions"), so the value wraps back to -2^63.
                    // Computing it in the signed domain is undefined instead, and on common
                    // hardware it faults rather than wrapping.
                    out.constant = signed_min;
                } else {
                    out.constant =
                        static_cast<std::uint64_t>(static_cast<std::int64_t>(out.constant) /
                                                   static_cast<std::int64_t>(rhs.constant));
                }
            } else {
                out.constant *= rhs.constant;
            }
        }
        return true;
    }

    bool parse_unary(ExprValue& out) {
        if (!at_end() && peek() == '-') {
            ++position_;
            if (!parse_unary(out)) return false;
            if (!require_constant(out, "unary -")) return false;
            // Unary negation stays in the unsigned domain for the reason the literal scanner's
            // own negation does: routing the value through std::int64_t is undefined behaviour at
            // exactly 2^63, and `-$8000000000000000` reaches this line where
            // `$-8000000000000000` reaches that one.
            out.constant = std::uint64_t{0} - out.constant;
            return true;
        }
        if (!at_end() && peek() == '~') {
            ++position_;
            if (!parse_unary(out)) return false;
            if (!require_constant(out, "unary ~")) return false;
            out.constant = ~out.constant;
            return true;
        }
        return parse_primary(out);
    }

    bool parse_primary(ExprValue& out);
    bool parse_number(ExprValue& out);
    bool parse_character(ExprValue& out);

    const std::string& text_;
    const SourceLoc& where_;
    Diagnostics& diags_;
    std::size_t position_ = 0;
    bool failed_ = false;
    bool undeclared_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------------------
// Escapes, shared by character and string literals
// ---------------------------------------------------------------------------------------

// The eight escapes assembler.md admits and no others: an unrecognized escape is a diagnostic
// rather than the escaped character, so a typo cannot quietly become data.
bool decode_escape(const std::string& text, std::size_t& position, std::uint8_t& out) {
    if (position >= text.size() || text[position] != '\\') {
        return false;
    }
    ++position;
    if (position >= text.size()) {
        return false;
    }
    const char c = text[position++];
    switch (c) {
        case '\\': out = '\\'; return true;
        case '"': out = '"'; return true;
        case '\'': out = '\''; return true;
        case 'n': out = '\n'; return true;
        case 'r': out = '\r'; return true;
        case 't': out = '\t'; return true;
        case '0': out = 0; return true;
        case 'x': {
            if (position + 1 >= text.size()) {
                return false;
            }
            const auto hex_digit = [](char digit, int& value) {
                if (digit >= '0' && digit <= '9') { value = digit - '0'; return true; }
                if (digit >= 'a' && digit <= 'f') { value = digit - 'a' + 10; return true; }
                if (digit >= 'A' && digit <= 'F') { value = digit - 'A' + 10; return true; }
                return false;
            };
            int high = 0;
            int low = 0;
            if (!hex_digit(text[position], high) || !hex_digit(text[position + 1], low)) {
                return false;
            }
            position += 2;
            out = static_cast<std::uint8_t>((high << 4) | low);
            return true;
        }
        default: return false;
    }
}

namespace {

bool ExprParser::parse_character(ExprValue& out) {
    // A character literal names its value by identity rather than by digits in an unstated
    // base, so it does not violate the mandatory-base rule and needs no marker.
    ++position_;  // the opening quote
    std::uint8_t value = 0;
    if (peek() == '\\') {
        if (!decode_escape(text_, position_, value)) {
            fail("unrecognized escape in character literal");
            return false;
        }
    } else if (!at_end()) {
        const unsigned char c = static_cast<unsigned char>(text_[position_++]);
        if (c > 255) {
            fail("a character literal above code point 255 is not representable");
            return false;
        }
        value = static_cast<std::uint8_t>(c);
    } else {
        fail("unterminated character literal");
        return false;
    }
    if (at_end() || peek() != '\'') {
        fail("a character literal holds exactly one character");
        return false;
    }
    ++position_;
    out.constant = value;
    return true;
}

bool ExprParser::parse_number(ExprValue& out) {
    const char marker = text_[position_++];
    int base = 10;
    const char* base_name = "decimal";
    if (marker == '$') {
        base = 16;
        base_name = "hexadecimal";
    } else if (marker == '%') {
        base = 2;
        base_name = "binary";
    }

    bool negative = false;
    if (!at_end() && (peek() == '-' || peek() == '+')) {
        negative = peek() == '-';
        ++position_;
    }

    // Two characters group digits, the comma and the back-tick, exactly the two v1 accepted.
    // Either is legal between two digits, carries no value, and is stripped before conversion;
    // one immediately after the marker, after the sign, or at the end of the literal is a
    // diagnostic, so a separator can never stand in for a missing digit.
    std::string digits;
    bool previous_was_separator = true;  // true at the start, so a leading separator fails
    bool saw_digit = false;
    while (!at_end()) {
        const char c = peek();
        if (c == ',' || c == '`') {
            if (previous_was_separator) {
                fail("a digit separator must sit between two digits");
                return false;
            }
            previous_was_separator = true;
            ++position_;
            continue;
        }
        int digit_value = 0;
        if (c >= '0' && c <= '9') {
            digit_value = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit_value = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit_value = c - 'A' + 10;
        } else {
            break;
        }
        if (digit_value >= base) {
            break;
        }
        digits.push_back(c);
        saw_digit = true;
        previous_was_separator = false;
        ++position_;
    }

    if (!saw_digit) {
        fail(std::string("a ") + base_name + " literal needs at least one digit");
        return false;
    }
    if (previous_was_separator) {
        fail("a digit separator must sit between two digits");
        return false;
    }

    std::uint64_t value = 0;
    for (const char c : digits) {
        int digit_value = 0;
        if (c >= '0' && c <= '9') {
            digit_value = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit_value = c - 'a' + 10;
        } else {
            digit_value = c - 'A' + 10;
        }
        // Expression arithmetic is 64-bit two's complement and wraps on overflow, so a literal
        // wider than 64 bits wraps here exactly as arithmetic would.
        value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit_value);
    }
    // The negation stays in the unsigned domain for the same reason the accumulation above does.
    // Negating in std::uint64_t yields the two's-complement bit pattern for every input and is
    // defined for all of them, where routing the value through std::int64_t first is undefined
    // behaviour at exactly 2^63, which is the one literal naming the most negative word the
    // machine's own registers hold.
    out.constant = negative ? std::uint64_t{0} - value : value;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Symbol leaves
// ---------------------------------------------------------------------------------------

// Defined out of the anonymous namespace's class body so it can reach the Assembler's private
// symbol table through the friendship the header does not need to declare: the parser asks the
// owner through a public-in-this-translation-unit shim below.
namespace {

struct SymbolQuery {
    bool known = false;
    bool defined = false;
    bool is_constant = false;
    std::uint64_t value = 0;
    std::uint8_t section = 0;
};

}  // namespace

// The parser needs three things from the assembler for a symbol leaf: whether the name is
// known, whether its value is settled, and which section it lives in. Rather than widen the
// header's surface for the parser's benefit, the assembler installs a lookup callback for the
// duration of one evaluation.
namespace {

using SymbolLookup = bool (*)(void* context, const std::string& name, SymbolQuery& out);

struct LookupBinding {
    SymbolLookup lookup = nullptr;
    void* context = nullptr;
    std::uint64_t here = 0;
};

thread_local LookupBinding g_binding;

bool ExprParser::parse_primary(ExprValue& out) {
    if (at_end()) {
        fail("an expression is missing");
        return false;
    }
    const char c = peek();
    if (c == '(') {
        ++position_;
        if (!parse_or(out)) return false;
        if (at_end() || peek() != ')') {
            fail("a '(' in this expression has no matching ')'");
            return false;
        }
        ++position_;
        return true;
    }
    if (c == '#' || c == '$' || c == '%') {
        return parse_number(out);
    }
    if (c == '\'') {
        return parse_character(out);
    }
    if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
        // The poka-yoke stance, stated plainly: a base is never inferred by the lexer and never
        // by a reader either, so a token beginning with a digit is a syntax error rather than a
        // number in whichever base the reader assumed.
        fail("a numeric literal names its base: write #" + text_.substr(position_) + " for decimal, $" +
             text_.substr(position_) + " for hexadecimal, or %" + text_.substr(position_) +
             " for binary");
        return false;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_') {
        std::size_t start = position_;
        while (!at_end() && (std::isalnum(static_cast<unsigned char>(peek())) != 0 ||
                             peek() == '_')) {
            ++position_;
        }
        const std::string name = text_.substr(start, position_ - start);

        if (name == "here") {
            out.constant = g_binding.here;
            return true;
        }
        std::uint8_t register_number = 0;
        if (is_register_name(name, register_number)) {
            fail("'" + name + "' is a register name and cannot appear in an expression");
            return false;
        }

        SymbolQuery query;
        if (g_binding.lookup != nullptr) {
            g_binding.lookup(g_binding.context, name, query);
        }
        if (query.known && query.is_constant) {
            out.constant = query.value;
            return true;
        }
        if (!query.known) {
            undeclared_ = true;
            fail("undefined symbol '" + name +
                 "' (define it, or declare it extern if another module defines it)");
            return false;
        }
        out.symbol = name;
        out.constant = 0;
        return true;
    }
    fail("'" + std::string(1, c) + "' does not begin an expression");
    return false;
}

bool ExprParser::fold_symbol_difference(ExprValue& out, const ExprValue& rhs) {
    SymbolQuery left;
    SymbolQuery right;
    if (g_binding.lookup == nullptr) {
        fail("two symbols cannot be subtracted here");
        return false;
    }
    g_binding.lookup(g_binding.context, out.symbol, left);
    g_binding.lookup(g_binding.context, rhs.symbol, right);
    if (!left.defined || !right.defined) {
        fail("one symbol minus another is relocatable only when this module defines both");
        return false;
    }
    if (left.section != right.section) {
        fail("one symbol minus another is relocatable only when both live in the same section");
        return false;
    }
    const std::uint64_t difference = left.value - right.value;
    out.symbol.clear();
    out.constant = out.constant - rhs.constant + difference;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// The assembler's entry point into all of the above
// ---------------------------------------------------------------------------------------

namespace {

bool lookup_in_assembler(void* context, const std::string& name, SymbolQuery& out);

}  // namespace

bool Assembler::evaluate(const std::string& text, const SourceLoc& where, ExprValue& out) {
    g_binding.lookup = &lookup_in_assembler;
    g_binding.context = this;
    g_binding.here = current_address();
    ExprParser parser(text, where, diags_);
    const bool ok = parser.parse(out);
    g_binding.lookup = nullptr;
    g_binding.context = nullptr;
    return ok;
}

namespace {

bool lookup_in_assembler(void* context, const std::string& name, SymbolQuery& out) {
    Assembler* assembler = static_cast<Assembler*>(context);
    const std::map<std::string, Symbol>& table = assembler->symbols();
    const auto it = table.find(name);
    if (it == table.end()) {
        out.known = false;
        return false;
    }
    out.known = true;
    out.defined = it->second.defined;
    out.is_constant = it->second.is_constant;
    out.value = it->second.value;
    out.section = it->second.section;
    return true;
}

}  // namespace

}  // namespace maize::v2::asmr
