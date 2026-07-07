#include <filesystem>
#include <unordered_map>
#include <iostream>
#include <sstream>
#include <list>
#include <algorithm>
#include <utility>
#include <stdexcept>
#include <string>
#include "maize.h"
#include <fstream>

using namespace maize;

namespace {
    enum class parser_state : int {
        whitespace,
        newline,
        code_block,
        comment,
        keyword,
        opcode,
        label_declaration,
        label_instance,
        address,
        hex_literal,
        dec_literal,
        bin_literal,
        operand1,
        operand2,
        register_id,
        sub_register_id
    };

    enum class label_state : int {
        start,
        name,
        value,
        end
    };

    enum class opcode_state : int {
        start,
        operand1,
        operand2,
        operand3,
        end
    };

    enum class literal_state : int {
        start,
        type,
        value,
        end
    };

    enum class string_state : int {
        start,
        read,
        escape,
        end
    };

    enum special_chars : char {
        hex = '$',
        dec = '#',
        bin = '%',
        label_end = ':',
        comment_start = ';',
        address = '@',
        reg_sep = '.',
        quote = '"',
        escape = '\\',
        neg = '-',
        pos = '+'
    };

    std::string base_path {};

    /* --check mode (maize-46): run the full tokenize+compile pipeline with zero
       filesystem effects. Editors probe assembly validity on save; a probe must
       neither write a .bin nor delete a previously-good one. */
    bool check_only {false};

    /* --stdin mode (maize-49): check a buffer piped over standard input, so an
       editor can validate unsaved text. Requires --check and --base-path; the
       diagnostic lines name source_name (--source-name) for buffer errors while
       INCLUDEd files keep reporting their real paths. */
    bool stdin_mode {false};
    std::string source_name {"<stdin>"};

    std::string current_token {};
    maize::u_hword current_address {};
    std::unordered_map<std::string, maize::u_hword> labels {};
    std::unordered_map<maize::u_hword, std::string> fixups {};
    std::map<maize::u_word, std::vector<u_byte>> memory_map {};

    /* Diagnostic state (maize-13). current_file / current_line track the source
       position of the byte stream; token_line remembers the line a token began on
       so a diagnostic cites where the token started rather than where it ended. */
    std::string current_file {};
    int current_line {1};
    int token_line {1};

    struct src_loc {
        std::string file {};
        int line {0};
    };

    /* Where each label was first really declared, and first referenced. Used to
       cite both sites in a redeclaration error and the reference site in an
       undefined-label error. */
    std::unordered_map<std::string, src_loc> label_decl_loc {};
    std::unordered_map<std::string, src_loc> label_ref_loc {};
    src_loc current_ref_loc {};

    template <typename T> struct expression;

    template <typename T> struct expression {
    private: 
        expression() { }

    public:
        typedef std::list<expression<T>> expression_list;

        expression(T token) : key(token) {
        }

        T key;
        expression_list value {};
        std::string loc_file {};
        int loc_line {0};

        expression<T>& add(T token) {
            value.push_back(expression<T>(token));
            expression<T> &node = value.back();
            node.loc_file = current_file;
            node.loc_line = token_line;
            return node;
        }
    };

    typedef expression<std::string> token_tree;

    typedef parser_state(*tokenizer_fn)(std::istream &fin, token_tree &tree, char c);
    typedef void (*compiler_fn)(token_tree &tree, std::string &opcode_str);

    struct keyword_data {
        keyword_data() : tokenizer(0), compiler(0) { }
        keyword_data(tokenizer_fn tokenizer_init, compiler_fn compiler_init) :
            tokenizer(tokenizer_init),
            compiler(compiler_init) 
        { }

        tokenizer_fn tokenizer;
        compiler_fn compiler;
    };

    struct opcode_data : public keyword_data {
        opcode_data() : opcode(0), keyword_data() { }
        opcode_data(u_byte opcode_init, tokenizer_fn tokenizer_init, compiler_fn compiler_init) : 
            opcode(opcode_init),
            keyword_data(tokenizer_init, compiler_init)
        { }

        u_byte opcode;
    };

    void assemble(std::string file_path);
    void tokenize(std::istream &fin, token_tree &tree);
    void compile(token_tree &tree);

    literal_state parse_literal(std::istream &fin, token_tree& tree, char c);

    parser_state process_char_stream(parser_state state, std::istream &fin, token_tree &tree, char c);

    parser_state parse_keyword(std::istream &fin, token_tree &tree, char c);
    parser_state parse_code_block(std::istream &fin, token_tree &tree, char c);
    parser_state process_keyword(parser_state state, std::istream &fin, token_tree &tree, char c);
    parser_state parse_opcode(std::istream &fin, token_tree &tree, char c);

    parser_state address_tokenizer(std::istream &fin, token_tree &tree, char c);
    parser_state string_tokenizer(std::istream &fin, token_tree &tree, char c);
    parser_state label_tokenizer(std::istream &fin, token_tree &tree, char c);
    parser_state data_tokenizer(std::istream &fin, token_tree &tree, char c);
    parser_state include_tokenizer(std::istream &fin, token_tree &tree, char c);
    parser_state opcode_0param_tokenizer(std::istream &fin, token_tree &tree, char c);
    parser_state opcode_1param_tokenizer(std::istream &fin, token_tree &tree, char c);
    parser_state opcode_2param_tokenizer(std::istream &fin, token_tree &tree, char c);
    parser_state opcode_3param_tokenizer(std::istream &fin, token_tree &tree, char c);

    void address_compiler(token_tree &tree, std::string &opcode_str);
    void string_compiler(token_tree &tree, std::string &opcode_str);
    void label_compiler(token_tree &tree, std::string &opcode_str);
    void data_compiler(token_tree &tree, std::string &opcode_str);
    void code_compiler(token_tree &tree, std::string &opcode_str);

    void no_operand_compiler(token_tree &tree, std::string &opcode_str);
    void regimm_reg_compiler(token_tree &tree, std::string &opcode_str);
    void regimm_regaddr_compiler(token_tree &tree, std::string &opcode_str);
    void reg_compiler(token_tree &tree, std::string &opcode_str);
    void regimm_imm_compiler(token_tree &tree, std::string &opcode_str);
    void regimm_compiler(token_tree &tree, std::string &opcode_str);
    void regimm_regreg_compiler(token_tree &tree, std::string &opcode_str);

    std::unordered_map<std::string, keyword_data> keywords {
        { "ADDRESS", {address_tokenizer, address_compiler}},
        { "LABEL", {label_tokenizer, label_compiler}},
        { "DATA", {data_tokenizer, data_compiler}},
        { "STRING", {string_tokenizer, string_compiler}},
        { "INCLUDE", {include_tokenizer, nullptr}},
        { "CODE", {nullptr, code_compiler}}
    };

    std::unordered_map<std::string, opcode_data> opcodes {
        { "HALT",   {cpu::instr::halt_opcode   , opcode_0param_tokenizer, no_operand_compiler}},
        { "LD",     {cpu::instr::ld_opcode     , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "CP",     {cpu::instr::ld_opcode     , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "ST",     {cpu::instr::st_opcode     , opcode_2param_tokenizer, regimm_regaddr_compiler}},
        { "ADD",    {cpu::instr::add_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "SUB",    {cpu::instr::sub_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "ADC",    {cpu::instr::adc_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "SBB",    {cpu::instr::sbb_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "MUL",    {cpu::instr::mul_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "DIV",    {cpu::instr::div_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "MOD",    {cpu::instr::mod_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "UDIV",   {cpu::instr::udiv_opcode   , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "UMOD",   {cpu::instr::umod_opcode   , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "AND",    {cpu::instr::and_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "OR",     {cpu::instr::or_opcode     , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "NOR",    {cpu::instr::nor_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "NAND",   {cpu::instr::nand_opcode   , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "XOR",    {cpu::instr::xor_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "SHL",    {cpu::instr::shl_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "SHR",    {cpu::instr::shr_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "CMP",    {cpu::instr::cmp_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "TEST",   {cpu::instr::test_opcode   , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "CMPXCHG",{cpu::instr::cmpxchg_opcode, opcode_3param_tokenizer, regimm_regreg_compiler}},
        { "LEA",    {cpu::instr::lea_opcode,     opcode_3param_tokenizer, regimm_regreg_compiler}},
        { "LDZ",    {cpu::instr::ldz_opcode,     opcode_2param_tokenizer, regimm_reg_compiler}},
        { "INC",    {cpu::instr::inc_opcode    , opcode_1param_tokenizer, reg_compiler}},
        { "DEC",    {cpu::instr::dec_opcode    , opcode_1param_tokenizer, reg_compiler}},
        { "NOT",    {cpu::instr::not_opcode    , opcode_1param_tokenizer, reg_compiler}},
        { "OUT",    {cpu::instr::out_opcode    , opcode_2param_tokenizer, regimm_imm_compiler}},
        { "JMP",    {cpu::instr::jmp_opcode    , opcode_1param_tokenizer, regimm_compiler}},
        { "JZ",     {cpu::instr::jz_opcode     , opcode_1param_tokenizer, regimm_compiler}},
        { "JNZ",    {cpu::instr::jnz_opcode    , opcode_1param_tokenizer, regimm_compiler}},
        { "JLT",    {cpu::instr::jlt_opcode    , opcode_1param_tokenizer, regimm_compiler}},
        { "JB",     {cpu::instr::jb_opcode     , opcode_1param_tokenizer, regimm_compiler}},
        { "JGT",    {cpu::instr::jgt_opcode    , opcode_1param_tokenizer, regimm_compiler}},
        { "JA",     {cpu::instr::ja_opcode     , opcode_1param_tokenizer, regimm_compiler}},
        { "JGE",    {cpu::instr::jge_opcode    , opcode_1param_tokenizer, regimm_compiler}},
        { "JLE",    {cpu::instr::jle_opcode    , opcode_1param_tokenizer, regimm_compiler}},
        { "JBE",    {cpu::instr::jbe_opcode    , opcode_1param_tokenizer, regimm_compiler}},
        { "JAE",    {cpu::instr::jae_opcode    , opcode_1param_tokenizer, regimm_compiler}},
        { "CALL",   {cpu::instr::call_opcode   , opcode_1param_tokenizer, regimm_compiler}},
        { "OUTR",   {cpu::instr::outr_opcode   , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "IN",     {cpu::instr::in_opcode     , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "PUSH",   {cpu::instr::push_opcode   , opcode_1param_tokenizer, regimm_compiler}},
        { "CLR",    {cpu::instr::clr_opcode    , opcode_1param_tokenizer, reg_compiler}},
        { "CMPIND", {cpu::instr::cmpind_opcode , opcode_2param_tokenizer, regimm_regaddr_compiler}},
        { "INT",    {cpu::instr::int_opcode    , opcode_1param_tokenizer, regimm_compiler}},
        { "TESTIND",{cpu::instr::testind_opcode, opcode_2param_tokenizer, regimm_regaddr_compiler}},
        { "POP",    {cpu::instr::pop_opcode    , opcode_1param_tokenizer, reg_compiler}},
        { "RET",    {cpu::instr::ret_opcode    , opcode_0param_tokenizer, no_operand_compiler}},
        { "IRET",   {cpu::instr::iret_opcode   , opcode_0param_tokenizer, no_operand_compiler}},
        { "SETINT", {cpu::instr::setint_opcode , opcode_0param_tokenizer, no_operand_compiler}},
        { "CLRINT", {cpu::instr::clrint_opcode , opcode_0param_tokenizer, no_operand_compiler}},
        { "SETCRY", {cpu::instr::setcry_opcode , opcode_0param_tokenizer, no_operand_compiler}},
        { "CLRCRY", {cpu::instr::clrcry_opcode , opcode_0param_tokenizer, no_operand_compiler}},
        { "SYS",    {cpu::instr::sys_opcode    , opcode_1param_tokenizer, regimm_compiler}},
        { "NOP",    {cpu::instr::nop_opcode    , opcode_0param_tokenizer, no_operand_compiler}},
        { "XCHG",   {cpu::instr::xchg_opcode   , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "BRK",    {cpu::instr::brk_opcode    , opcode_0param_tokenizer, no_operand_compiler}}
    };

    /* A fatal assembler diagnostic (maize-13). Carries an already-formatted
       "mazm: <file>:<line>: error: <msg>" string; main() catches it, prints to
       stderr, and exits nonzero. */
    struct asm_error : public std::runtime_error {
        asm_error(std::string const &msg) : std::runtime_error(msg) { }
    };

    [[noreturn]] void fatal(std::string const &file, int line, std::string const &msg) {
        std::ostringstream os;
        os << "mazm: " << file << ":" << line << ": error: " << msg;
        throw asm_error(os.str());
    }

    /* Read one byte without skipping whitespace (the project-wide idiom was
       "fin >> std::noskipws >> c"). Centralized so newline counting happens in
       exactly one place. Only '\n' advances the line counter, so both LF and
       CRLF files count one line per line. */
    std::istream& read_char(std::istream &fin, char &c) {
        fin >> std::noskipws >> c;

        if (fin && c == '\n') {
            ++current_line;
        }

        return fin;
    }

    /* Skip a leading UTF-8 BOM (EF BB BF) if present. Files without a BOM are
       left byte-for-byte untouched. */
    void skip_bom(std::fstream &fin) {
        if (!fin.good()) {
            return;
        }

        std::streampos start = fin.tellg();
        char b[3] = {0, 0, 0};
        fin.read(b, 3);
        std::streamsize got = fin.gcount();

        bool is_bom = got == 3
            && static_cast<unsigned char>(b[0]) == 0xEF
            && static_cast<unsigned char>(b[1]) == 0xBB
            && static_cast<unsigned char>(b[2]) == 0xBF;

        if (!is_bom) {
            fin.clear();
            fin.seekg(start);
        }
    }

    /* Append a byte to the token under construction, remembering the line the
       token started on the first time a character is added. */
    void tok_push(char c) {
        if (current_token.empty()) {
            token_line = current_line;
        }

        current_token.push_back(c);
    }

    void assemble(std::string file_path) {
        std::filesystem::path asm_path {std::filesystem::canonical(file_path)};

        std::filesystem::path bin_path {asm_path};
        std::filesystem::path ext {"bin"};
        bin_path.replace_extension(ext);

        /* Stale-binary rule (maize-13, AC9): remove any pre-existing output up
           front, so a failed assembly never leaves a previously-good .bin sitting
           at the target path looking like a fresh build for the now-broken source.
           In --check mode nothing is produced, so nothing may be destroyed either:
           a broken intermediate save must not cost the last-good binary. */
        if (!check_only) {
            std::error_code remove_ec;
            std::filesystem::remove(bin_path, remove_ec);
        }

        std::fstream fin(file_path, std::fstream::in);
        current_file = file_path;
        current_line = 1;
        token_line = 1;
        skip_bom(fin);

        token_tree tree {file_path};
        tokenize(fin, tree);
        compile(tree);

        if (check_only) {
            return;
        }

        /* write here */
        std::cout << "Output to " << bin_path.string() << std::endl;
        std::ofstream bin(bin_path, std::fstream::binary);

        maize::u_word last_block {cpu::mm.last_block()};
        maize::u_word end {last_block + cpu::mm.block_size};
        maize::u_word current_address {0};

        while (current_address < end) {
            char c = cpu::mm.read_byte(current_address);
            bin.write(&c, 1);
            ++current_address;
        }

        bin.close();
    }

    void tokenize(std::istream &fin, token_tree &tree) {
        parser_state state {parser_state::whitespace};
        char c {};

        while (read_char(fin, c)) {
            switch (state) {
            case parser_state::comment:
                switch (c) {
                case '\r':
                case '\n':
                    state = parser_state::newline;
                    continue;
                }
                break;

            case parser_state::keyword:
                state = process_keyword(state, fin, tree, c);
                continue;

            case parser_state::code_block:
                state = parse_code_block(fin, tree, c);
                continue;

            default:
                state = process_char_stream(state, fin, tree, c);
                continue;
            }
        }
    }

    void compile(token_tree &tree) {
        int i {0};

        for (auto &sub_tree : tree.value) {
            auto &key = sub_tree.key;
            compiler_fn compiler {0};

            if (keywords.contains(key)) {
                compiler = keywords[key].compiler;
            }
            else if (opcodes.contains(key)) {
                compiler = opcodes[key].compiler;
            }
            else {
                throw std::logic_error("keyword not found");
            }

            compiler(sub_tree, key);
        }

        for (auto &pair : fixups) {
            auto &label = pair.second;
            auto value = labels.contains(label)
                ? labels[label]
                : std::numeric_limits<u_hword>::max();

            /* Undefined label (maize-13): a reference that was never declared
               keeps the max() placeholder. Report it instead of writing the
               sentinel into the binary. */
            if (value == std::numeric_limits<u_hword>::max()) {
                src_loc loc = label_ref_loc.contains(label)
                    ? label_ref_loc[label]
                    : src_loc {current_file, 0};
                fatal(loc.file, loc.line, "undefined label '" + label + "'");
            }

            cpu::mm.write_hword(pair.first, value);
        }
    }

    literal_state parse_literal(std::istream &fin, token_tree &tree, char c) {
        literal_state state = literal_state::start;

        while (c >= 0) {
            switch (state) {
            case literal_state::start:
                if (isspace(static_cast<unsigned char>(c))) {
                    state = literal_state::start;
                }
                else if (c == ',' || c == '`') {
                    state = literal_state::value;
                }
                else {
                    tok_push(c);
                    state = literal_state::value;
                }
                break;

            case literal_state::type:
                break;

            case literal_state::value:
                if (c == ',' || c == '`') {
                    break;
                }

                if (isspace(static_cast<unsigned char>(c))) {
                    tree.add(current_token);
                    current_token.clear();
                    return literal_state::end;
                }
                else {
                    tok_push(c);
                    state = literal_state::value;
                }

                break;

            case literal_state::end:
                return state;
                break;

            default:
                break;
            }

            read_char(fin, c);
        }

        return literal_state::end;
    }

    parser_state process_char_stream(parser_state state, std::istream &fin, token_tree &tree, char c) {
        if (isspace(static_cast<unsigned char>(c))) {
            return parser_state::whitespace;
        }

        if (isalnum(static_cast<unsigned char>(c)) || c == ',' || c == '`') {
            state = parser_state::keyword;
        }
        else {
            switch (c) {
            case '\r':
            case '\n':
                return parser_state::newline;

            case special_chars::comment_start:
                return parser_state::comment;

            case special_chars::label_end:
                return parser_state::code_block;

            default:
                break;
            }
        }

        tok_push(c);
        return state;
    }

    parser_state process_keyword(parser_state state, std::istream &fin, token_tree &tree, char c) {
        /* TODO: Why am I processing this up here instead of in parse_keyword with the rest of the keywords?
        I didn't document this in the C# version, so I have no idea. */
        if (isspace(static_cast<unsigned char>(c))) {
            if (current_token == "INCLUDE") {
                std::string keyword = current_token;
                current_token.clear();
                // token_tree &sub_tree = tree.add(keyword);
                return keywords[keyword].tokenizer(fin, tree, c);
            }

            return parse_keyword(fin, tree, c);
        }

        return process_char_stream(state, fin, tree, c);
    }

    parser_state parse_keyword(std::istream &fin, token_tree &tree, char c) {
        std::string keyword = current_token;
        current_token.clear();

        if (keywords.contains(keyword)) {
            /* Keywords are fixed-case. */
            token_tree &sub_tree = tree.add(keyword);
            return keywords[keyword].tokenizer(fin, sub_tree, c);
        }

        /* Opcodes may be upper-case, lower-case, or mixed-case. Make everything upper-case for comparison. */
        std::transform(keyword.begin(), keyword.end(), keyword.begin(), ::toupper);
        
        if (opcodes.contains(keyword)) {
            token_tree &sub_tree = tree.add(keyword);
            return opcodes[keyword].tokenizer(fin, sub_tree, c);
        }

        /* Unknown keyword/opcode (maize-13): previously silently dropped. */
        if (!keyword.empty()) {
            fatal(current_file, token_line,
                "unknown keyword or opcode '" + keyword + "'");
        }

        return parser_state::whitespace;
    }

    parser_state parse_code_block(std::istream &fin, token_tree &tree, char c) {
        parser_state state {parser_state::whitespace};
        token_tree &sub_tree = tree.add("CODE");
        sub_tree.add(current_token);
        current_token.clear();

        do {
            switch (state) {
            case parser_state::comment:
                switch (c) {
                case '\r':
                case '\n':
                    state = parser_state::newline;
                    break;
                }

                break;

            case parser_state::keyword:
                state = process_keyword(state, fin, sub_tree, c);

                if (state == parser_state::code_block) {
                    return state;
                }

                break;

            case parser_state::code_block:
                return state;

            default:
                state = process_char_stream(state, fin, sub_tree, c);
                break;
            }

        } while (read_char(fin, c));

        return state;
    }

    parser_state parse_opcode(std::istream &fin, token_tree &tree, char c) {
        std::string keyword = current_token;
        current_token.clear();
        token_tree &sub_tree = tree.add(keyword);

        if (opcodes.contains(keyword)) {
            return opcodes[keyword].tokenizer(fin, sub_tree, c);
        }

        return parser_state::whitespace;
    }

    parser_state address_tokenizer(std::istream& fin, token_tree &tree, char c) {
        while (parse_literal(fin, tree, c) != literal_state::end) {
        }

        return parser_state::whitespace;
    }

    parser_state label_tokenizer(std::istream &fin, token_tree &tree, char c) {
        label_state state {label_state::start};

        while (fin.peek() >= 0) {
            read_char(fin, c);

            switch (state) {
            case label_state::start:
                if (isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
                    state = label_state::name;
                    tok_push(c);
                }

                break;

            case label_state::name:
                if (isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
                    state = label_state::name;
                    tok_push(c);
                }
                else if (isspace(static_cast<unsigned char>(c))) {
                    tree.add(current_token);
                    current_token.clear();
                    state = label_state::value;
                }

                break;

            case label_state::value:
                if (parse_literal(fin, tree, c) == literal_state::end) {
                    state = label_state::end;
                    return parser_state::whitespace;
                }

                break;
            }
        }

        return parser_state::label_declaration;
    }

    parser_state string_tokenizer(std::istream &fin, token_tree &tree, char c) {
        auto state = string_state::start;
        int string_open_line {current_line};

        while (fin.peek() >= 0) {
            read_char(fin, c);

            if (c == '\r' || c == '\n') {
                /* Unterminated string (maize-13): a newline arrived before the
                   closing quote. */
                if (state == string_state::read || state == string_state::escape) {
                    fatal(current_file, string_open_line,
                        "unterminated string literal starting at line "
                        + std::to_string(string_open_line));
                }

                return parser_state::newline;
            }

            switch (state) {
            case string_state::start:
                if (c == special_chars::comment_start) {
                    return parser_state::comment;
                }

                if (c == special_chars::quote) {
                    string_open_line = current_line;
                    state = string_state::read;
                }

                break;

            case string_state::read:
                if (c == special_chars::escape) {
                    tok_push(c);
                    state = string_state::escape;
                    break;
                }

                if (c == special_chars::quote) {
                    tree.add(current_token);
                    current_token.clear();
                    return parser_state::whitespace;
                }

                tok_push(c);

                break;

            case string_state::escape:
                tok_push(c);
                state = string_state::read;
                break;

            case string_state::end:
                break;

            default:
                break;
            }
        }

        /* Unterminated string (maize-13): EOF arrived before the closing quote. */
        if (state == string_state::read || state == string_state::escape) {
            fatal(current_file, string_open_line,
                "unterminated string literal starting at line "
                + std::to_string(string_open_line));
        }

        return parser_state::whitespace;
    }

    parser_state data_tokenizer(std::istream& fin, token_tree &tree, char c) {
        while (fin.peek() >= 0) {
            read_char(fin, c);

            if (c == '.' || c == '`') {
                continue;
            }

            if (isalnum(static_cast<unsigned char>(c)) || c == special_chars::bin || c == special_chars::dec || c == special_chars::hex) {
                tok_push(c);
            }
            else if ((isspace(static_cast<unsigned char>(c)) || c == '\r' || c == '\n' || c == special_chars::comment_start) && !current_token.empty()) {
                tree.add(current_token);
                current_token.clear();
            }

            if (c == '\r' || c == '\n') {
                return parser_state::newline;
            }

            if (c == special_chars::comment_start) {
                return parser_state::comment;
            }
        }

        return parser_state::whitespace;
    }

    parser_state include_tokenizer(std::istream& fin, token_tree &tree, char c) {
        std::string include_from_file {current_file};
        int include_from_line {current_line};

        token_tree string_tree {""};
        string_tokenizer(fin, string_tree, c);
        auto file_path = string_tree.value.begin()->key;
        std::filesystem::path include_path = base_path;
        include_path.append(file_path);

        std::fstream finclude(include_path.string(), std::fstream::in);

        /* Missing INCLUDE target (maize-13): an unopened stream would otherwise
           make tokenize's read loop immediately false, silently assembling as if
           the directive were absent. */
        if (!finclude.is_open()) {
            fatal(include_from_file, include_from_line,
                "cannot open include file '" + include_path.string()
                + "' (included from " + include_from_file + ":"
                + std::to_string(include_from_line) + ")");
        }

        /* Line tracking spans INCLUDE'd files independently: the included file
           reports its own name and its own 1-based line numbers. */
        std::string saved_file {current_file};
        int saved_line {current_line};
        int saved_token_line {token_line};

        current_file = include_path.string();
        current_line = 1;
        token_line = 1;
        skip_bom(finclude);

        tokenize(finclude, tree);
        finclude.close();

        current_file = saved_file;
        current_line = saved_line;
        token_line = saved_token_line;

        return parser_state::whitespace;
    }

    parser_state opcode_0param_tokenizer(std::istream &fin, token_tree &tree, char c) {
        return parser_state::whitespace;
    }

    parser_state opcode_1param_tokenizer(std::istream &fin, token_tree &tree, char c) {
        opcode_state state {opcode_state::start};
        int instr_line {current_line};

        while (fin.peek() >= 0) {
            read_char(fin, c);

            switch (state) {
            case opcode_state::start:
                if (isspace(static_cast<unsigned char>(c))) {
                    continue;
                }
                else if (c == ',' || c == '`') {
                    state = opcode_state::operand1;
                }
                else {
                    state = opcode_state::operand1;
                    tok_push(c);
                }

                break;

            case opcode_state::operand1:
                if (isspace(static_cast<unsigned char>(c))) {
                    tree.add(current_token);
                    current_token.clear();
                    return parser_state::whitespace;
                }
                else if (c == ',' || c == '`') {
                    break;
                }
                else {
                    tok_push(c);
                }

                break;
            }
        }

        /* EOF reached (maize-13). Flush a trailing operand not terminated by
           whitespace, then reject a truncated instruction. */
        if (!current_token.empty()) {
            tree.add(current_token);
            current_token.clear();
        }

        if (tree.value.size() < 1) {
            fatal(current_file, instr_line,
                "unexpected end of file: '" + tree.key + "' expects 1 operand(s), got "
                + std::to_string(tree.value.size()));
        }

        return parser_state::whitespace;
    }

    parser_state opcode_2param_tokenizer(std::istream &fin, token_tree &tree, char c) {
        opcode_state state {opcode_state::start};
        int instr_line {current_line};

        while (fin.peek() >= 0) {
            read_char(fin, c);

            switch (state) {
            case opcode_state::start:
                if (isspace(static_cast<unsigned char>(c))) {
                    continue;
                }
                else if (c == ',' || c == '`') {
                    state = opcode_state::operand1;
                }
                else {
                    state = opcode_state::operand1;
                    tok_push(c);
                }

                break;

            case opcode_state::operand1:
                if (isspace(static_cast<unsigned char>(c))) {
                    tree.add(current_token);
                    current_token.clear();
                    state = opcode_state::operand2;
                }
                else if (c == ',' || c == '`') {
                    break;
                }
                else {
                    tok_push(c);
                }

                break;

            case opcode_state::operand2:
                if (isspace(static_cast<unsigned char>(c))) {
                    tree.add(current_token);
                    current_token.clear();
                    return parser_state::whitespace;
                }
                else if (c == ',' || c == '`') {
                    break;
                }
                else {
                    tok_push(c);
                }

                break;
            }
        }

        /* EOF reached (maize-13). Flush a trailing operand not terminated by
           whitespace, then reject a truncated instruction. */
        if (!current_token.empty()) {
            tree.add(current_token);
            current_token.clear();
        }

        if (tree.value.size() < 2) {
            fatal(current_file, instr_line,
                "unexpected end of file: '" + tree.key + "' expects 2 operand(s), got "
                + std::to_string(tree.value.size()));
        }

        return parser_state::whitespace;
    }

    parser_state opcode_3param_tokenizer(std::istream &fin, token_tree &tree, char c) {
        opcode_state state {opcode_state::start};
        int instr_line {current_line};

        while (fin.peek() >= 0) {
            read_char(fin, c);

            switch (state) {
                case opcode_state::start:
                    if (isspace(static_cast<unsigned char>(c))) {
                        continue;
                    }
                    else if (c == ',' || c == '`') {
                        state = opcode_state::operand1;
                    }
                    else {
                        state = opcode_state::operand1;
                        tok_push(c);
                    }

                    break;

                case opcode_state::operand1:
                    if (isspace(static_cast<unsigned char>(c))) {
                        tree.add(current_token);
                        current_token.clear();
                        state = opcode_state::operand2;
                    }
                    else if (c == ',' || c == '`') {
                        break;
                    }
                    else {
                        tok_push(c);
                    }

                    break;

                case opcode_state::operand2:
                    if (isspace(static_cast<unsigned char>(c))) {
                        tree.add(current_token);
                        current_token.clear();
                        state = opcode_state::operand3;
                    }
                    else if (c == ',' || c == '`') {
                        break;
                    }
                    else {
                        tok_push(c);
                    }

                    break;

                case opcode_state::operand3:
                    if (isspace(static_cast<unsigned char>(c))) {
                        tree.add(current_token);
                        current_token.clear();
                        return parser_state::whitespace;
                    }
                    else if (c == ',' || c == '`') {
                        break;
                    }
                    else {
                        tok_push(c);
                    }

                    break;
            }
        }

        /* EOF reached (maize-13). Flush a trailing operand not terminated by
           whitespace, then reject a truncated instruction. */
        if (!current_token.empty()) {
            tree.add(current_token);
            current_token.clear();
        }

        if (tree.value.size() < 3) {
            fatal(current_file, instr_line,
                "unexpected end of file: '" + tree.key + "' expects 3 operand(s), got "
                + std::to_string(tree.value.size()));
        }

        return parser_state::whitespace;
    }

    uint64_t bin_cvt(std::string const &str) {
        uint64_t tmp {0};

        for (auto c : str) {
            tmp = tmp << 1;

            if (c == '1') {
                tmp |= 1;
            }
            else if (c == '0') {
                // nothing
            }
            else {
                std::cerr << "Error" << std::endl;
                return -1;
            }
        }

        return tmp;
    }


    maize::u_hword convert_label_string(std::string const &value) {
        maize::u_hword hvalue {0};
        char type = value[0];

        std::stringstream cvt;

        if (type == special_chars::hex) {
            cvt << std::hex << value.substr(1);
            cvt >> hvalue;
        }
        else if (type == special_chars::dec) {
            cvt << std::dec << value.substr(1);
            cvt >> hvalue;
        }
        else if (type == special_chars::bin) {
            std::string tmp = value.substr(1);
            hvalue = static_cast<u_hword>(bin_cvt(tmp));
        }
        else {
            hvalue = std::numeric_limits<u_hword>::max();
        }

        return hvalue;
    }

    u_byte compile_label(std::string &label, cpu::reg_value &value) {
        value = labels[label];
        return cpu::opflag_imm_size_32b;
    }

    u_byte compile_hex_literal(std::string &literal, cpu::reg_value &value) {
        auto len = literal.length();

        if (len && (literal[0] == special_chars::neg || literal[0] == special_chars::pos)) {
            --len;
        }

        std::stringstream cvt;
        cvt << std::hex << literal;
        cvt >> value.w0;
        u_byte type_byte = cpu::opflag_imm_size_64b;

        if (len <= 2) {
            type_byte = cpu::opflag_imm_size_08b;
        }
        else if (len <= 4) {
            type_byte = cpu::opflag_imm_size_16b;
        }
        else if (len <= 8) {
            type_byte = cpu::opflag_imm_size_32b;
        }
        else if (len <= 16) {
            type_byte = cpu::opflag_imm_size_64b;
        }
        else {
            throw std::logic_error("Invalid literal format");
        }

        return type_byte;
    }

    u_byte compile_dec_literal(std::string &literal, cpu::reg_value &value) {
        std::stringstream cvt;
        cvt << std::dec << literal;
        cvt >> value.w0;
        u_byte type_byte = cpu::opflag_imm_size_64b;

        if (value.w0 <= std::numeric_limits<u_byte>::max()) {
            type_byte = cpu::opflag_imm_size_08b;
        }
        else if (value.w0 <= std::numeric_limits<u_qword>::max()) {
            type_byte = cpu::opflag_imm_size_16b;
        }
        else if (value.w0 <= std::numeric_limits<u_hword>::max()) {
            type_byte = cpu::opflag_imm_size_32b;
        }
        else if (value.w0 <= std::numeric_limits<u_word>::max()) {
            type_byte = cpu::opflag_imm_size_64b;
        }
        else {
            throw std::logic_error("Invalid literal format");
        }

        return type_byte;
    }

    u_byte compile_bin_literal(std::string &literal, cpu::reg_value &value) {
        auto len = literal.length();
        std::string tmp = literal;

        if (len && (literal[0] == special_chars::neg || literal[0] == special_chars::pos)) {
            --len;
            tmp = tmp.substr(1);
        }

        value.w0 = bin_cvt(tmp);
        u_byte type_byte = cpu::opflag_imm_size_32b;

        if (len <= 8) {
            type_byte = cpu::opflag_imm_size_08b;
        }
        else if (len <= 16) {
            type_byte = cpu::opflag_imm_size_16b;
        }
        else if (len <= 32) {
            type_byte = cpu::opflag_imm_size_32b;
        }
        else if (len <= 64) {
            type_byte = cpu::opflag_imm_size_64b;
        }
        else {
            throw std::logic_error("Invalid literal format");
        }

        return type_byte;
    }

    u_byte compile_literal(std::string &literal, cpu::reg_value &value) {
        char type_char = literal[0];
        u_byte type_byte = 0;
        value = 0;

        if (type_char == special_chars::hex) {
            std::string sub = literal.substr(1);
            type_byte = compile_hex_literal(sub, value);
        }
        else if (type_char == special_chars::dec) {
            std::string sub = literal.substr(1);
            type_byte = compile_dec_literal(sub, value);
        }
        else if (type_char == special_chars::bin) {
            std::string sub = literal.substr(1);
            type_byte = compile_bin_literal(sub, value);
        }

        return type_byte;
    }

    bool is_literal(std::string &str) {
        return str[0] == special_chars::hex || str[0] == special_chars::bin || str[0] == special_chars::dec;
    }

    bool is_label(std::string &str) {
        return labels.contains(str);
    }

    void label_compiler(token_tree &tree, std::string &opcode_str) {
        expression<std::string>::expression_list::iterator it = tree.value.begin();
        auto &label_node = *it;
        auto label = label_node.key;
        ++it;
        auto value = it->key;

        /* Duplicate label declaration (maize-13): a second real declaration used
           to silently overwrite the resolved address. Reject it, citing both
           sites. A max() entry is only a forward-reference placeholder, which is
           a legitimate resolution, not a redeclaration. */
        if (labels.contains(label) && labels[label] != std::numeric_limits<u_hword>::max()) {
            src_loc first = label_decl_loc[label];
            fatal(label_node.loc_file, label_node.loc_line,
                "label '" + label + "' redeclared (first declared at "
                + first.file + ":" + std::to_string(first.line) + ")");
        }

        auto hvalue = convert_label_string(value);
        labels[label] = hvalue;
        label_decl_loc[label] = { label_node.loc_file, label_node.loc_line };
    }

    void address_compiler(token_tree &tree, std::string &opcode_str) {
        auto data_string {tree.value.begin()->key};
        u_hword data = 0;

        if (is_label(data_string)) {
            cpu::reg_value value = labels[data_string];
        }
        else {
            data = convert_label_string(data_string);
            current_address += cpu::mm.write_hword(current_address, data);
        }
    }

    void string_compiler(token_tree &tree, std::string &opcode_str) {
        int i = 0;

        for (auto &sub_tree : tree.value) {
            auto &literal = sub_tree.key;
            auto it = literal.begin();

            while (it != literal.end()) {
                char c = *it;

                if (c == '\\') {
                    ++it;
                    c = *it;

                    switch (c) {
                    case '\\':
                    case '\'':
                    case '\"':
                        current_address += cpu::mm.write_byte(current_address, static_cast<u_byte>(c));
                        break;

                    case '0':
                        current_address += cpu::mm.write_byte(current_address, static_cast<u_byte>(0));
                        break;

                    case 't':
                        current_address += cpu::mm.write_byte(current_address, static_cast<u_byte>('\t'));
                        break;

                    case 'r':
                        current_address += cpu::mm.write_byte(current_address, static_cast<u_byte>('\r'));
                        break;

                    case 'n':
                        current_address += cpu::mm.write_byte(current_address, static_cast<u_byte>('\n'));
                        break;

                    case 'a':
                        current_address += cpu::mm.write_byte(current_address, static_cast<u_byte>('\a'));
                        break;

                    case 'b':
                        current_address += cpu::mm.write_byte(current_address, static_cast<u_byte>('\b'));
                        break;

                    case 'f':
                        current_address += cpu::mm.write_byte(current_address, static_cast<u_byte>('\f'));
                        break;

                    case 'e':
                        current_address += cpu::mm.write_byte(current_address, static_cast<u_byte>(0x1B));
                        break;

                    case 'v':
                        current_address += cpu::mm.write_byte(current_address, static_cast<u_byte>('\v'));
                        break;

                    }
                }
                else {
                    current_address += cpu::mm.write_byte(current_address, static_cast<u_byte>(c));
                }

                ++it;
            }
        }
    }

    void data_compiler(token_tree &tree, std::string &opcode_str) {
        for (auto &sub_tree : tree.value) {
            cpu::reg_value value;
            auto literal = sub_tree.key;
            auto type_byte = compile_literal(literal, value);

            if ((type_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_16b) {
                current_address += cpu::mm.write_qword(current_address, value.q0);
            }
            else if ((type_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_32b) {
                current_address += cpu::mm.write_hword(current_address, value.h0);
            }
            else if ((type_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_64b) {
                current_address += cpu::mm.write_word(current_address, value.w0);
            }
            else {
                current_address += cpu::mm.write_byte(current_address, value.b0);
            }
        }

    }

    void code_compiler(token_tree &tree, std::string &opcode_str) {
        auto &label_node = *tree.value.begin();
        auto label {label_node.key};
        auto address = convert_label_string(label);

        if (address == std::numeric_limits<u_hword>::max()) {
            /* Duplicate label declaration (maize-13): the old code read the
               stale, already-resolved address out of labels[label] and rewound
               current_address to it, silently corrupting the output. A real
               (non-max) entry here means this label was already declared, so this
               is a redeclaration; reject it citing both sites. A max() entry is a
               forward-reference placeholder, which we correctly resolve below. */
            bool already_declared = labels.contains(label)
                && labels[label] != std::numeric_limits<u_hword>::max();

            if (already_declared) {
                src_loc first = label_decl_loc[label];
                fatal(label_node.loc_file, label_node.loc_line,
                    "label '" + label + "' redeclared (first declared at "
                    + first.file + ":" + std::to_string(first.line) + ")");
            }

            address = current_address;
            labels[label] = address;
            label_decl_loc[label] = { label_node.loc_file, label_node.loc_line };
        }

        current_address = address;

        for (auto &sub_tree : tree.value) {
            auto &key = sub_tree.key;

            if (keywords.contains(key)) {
                keywords[key].compiler(sub_tree, key);
            }
            else if (opcodes.contains(key)) {
                opcodes[key].compiler(sub_tree, key);
            }
        }
    }

    std::unordered_map<std::string, u_byte> reg_map {
        {"R0", cpu::opflag_reg_r0},
        {"R1", cpu::opflag_reg_r1},
        {"R2", cpu::opflag_reg_r2},
        {"R3", cpu::opflag_reg_r3},
        {"R4", cpu::opflag_reg_r4},
        {"R5", cpu::opflag_reg_r5},
        {"R6", cpu::opflag_reg_r6},
        {"R7", cpu::opflag_reg_r7},
        {"R8", cpu::opflag_reg_r8},
        {"R9", cpu::opflag_reg_r9},
        {"RT", cpu::opflag_reg_rt},
        {"RV", cpu::opflag_reg_rv},
        {"RF", cpu::opflag_reg_rf},
        {"RB", cpu::opflag_reg_rb},
        {"RP", cpu::opflag_reg_rp},
        {"RS", cpu::opflag_reg_rs}
    };

    std::unordered_map<std::string, u_byte> subreg_map {
        {"B0", cpu::opflag_subreg_b0},
        {"B1", cpu::opflag_subreg_b1},
        {"B2", cpu::opflag_subreg_b2},
        {"B3", cpu::opflag_subreg_b3},
        {"B4", cpu::opflag_subreg_b4},
        {"B5", cpu::opflag_subreg_b5},
        {"B6", cpu::opflag_subreg_b6},
        {"B7", cpu::opflag_subreg_b7},
        {"Q0", cpu::opflag_subreg_q0},
        {"Q1", cpu::opflag_subreg_q1},
        {"Q2", cpu::opflag_subreg_q2},
        {"Q3", cpu::opflag_subreg_q3},
        {"H0", cpu::opflag_subreg_h0},
        {"H1", cpu::opflag_subreg_h1},
        {"W0", cpu::opflag_subreg_w0},
        {"W",  cpu::opflag_subreg_w0}
    };

    u_byte compile_register(std::string &reg) {
        std::string reg_str;
        std::transform(reg.begin(), reg.end(), std::back_inserter(reg_str), ::toupper);
        u_byte reg_byte {cpu::opflag_subreg_w0};

        /* Get some special cases out of the way */
        if (reg_str == "SP") {
            reg_byte = cpu::opflag_reg_rs | cpu::opflag_subreg_w0;
        }
        else if (reg_str == "BP") {
            reg_byte = cpu::opflag_reg_rb | cpu::opflag_subreg_w0;
        }
        else if (reg_str == "PC") {
            reg_byte = cpu::opflag_reg_rp | cpu::opflag_subreg_w0;
        }
        else if (reg_str == "FL") {
            reg_byte = cpu::opflag_reg_rf | cpu::opflag_subreg_h0;
        }
        else {
            std::string reg {reg_str};
            std::string subreg {};
            auto subreg_sep {reg_str.find('.')};

            if (subreg_sep != std::string::npos) {
                reg = reg_str.substr(0, subreg_sep);
                subreg = reg_str.substr(subreg_sep + 1);

                if (subreg_map.contains(subreg)) {
                    reg_byte = subreg_map[subreg];
                }
            }
            else {
                reg_byte = cpu::opflag_subreg_w0;
            }

            if (reg_map.contains(reg)) {
                reg_byte |= reg_map[reg];
            }
        }

        return reg_byte;
    }

    bool is_register(std::string &str) {
        std::string reg_str;
        std::transform(str.begin(), str.end(), std::back_inserter(reg_str), ::toupper);
        u_byte reg_byte {cpu::opflag_subreg_w0};

        /* Get some special cases out of the way */
        if (reg_str == "SP") {
            return true;
        }
        else if (reg_str == "BP") {
            return true;
        }
        else if (reg_str == "PC") {
            return true;
        }
        else if (reg_str == "FL") {
            return true;
        }
        else {
            std::string reg {reg_str};
            auto subreg_sep {reg_str.find('.')};

            if (subreg_sep != std::string::npos) {
                reg = reg_str.substr(0, subreg_sep);
            }

            if (reg_map.contains(reg)) {
                return true;
            }
        }

        return false;
    }

    u_hword write_label(u_hword address, std::string &label, cpu::reg_value value) {
        if (value.h0 == std::numeric_limits<u_hword>::max()) {
            fixups[current_address] = label;
        }

        return cpu::mm.write_hword(address, value.h0);
    }

    void no_operand_compiler(token_tree &tree, std::string &opcode_str) {
        u_byte opcode {opcodes[opcode_str].opcode};
        current_address += cpu::mm.write_byte(current_address, opcode);
    }

    u_byte add_label(std::string &label, cpu::reg_value &value) {
        value.h0 = std::numeric_limits<u_hword>::max();
        labels[label] = value.h0;

        /* Remember where a label was first referenced so an undefined-label
           error (maize-13) can cite the reference site. */
        if (!label_ref_loc.contains(label)) {
            label_ref_loc[label] = current_ref_loc;
        }

        return cpu::opflag_imm_size_32b;
    }

    void regimm_reg_compiler(token_tree &tree, std::string &opcode_str) {
        u_byte opcode {opcodes[opcode_str].opcode};
        auto it {tree.value.begin()};
        current_ref_loc = { it->loc_file, it->loc_line };
        auto operand1 {it->key};
        ++it;
        auto operand2 {it->key};
        u_byte operand1_byte {0};
        bool operand_is_immediate {false};
        bool operand_is_label {false};
        cpu::reg_value operand1_literal {0};

        bool operand1_is_address = (operand1[0] == special_chars::address);

        /* Memory-boundary discipline (card maize-43): the data-movement mnemonics each name
           exactly one thing. CP/CPZ take a value source (register or immediate) and never
           touch memory; LD/LDZ read from a memory address (source prefixed with '@'). The
           ALU/CMP/TEST ops keep accepting either form (Maize is CISC), so they are not
           constrained here. */
        if ((opcode_str == "CP" || opcode_str == "CPZ") && operand1_is_address) {
            fatal(current_ref_loc.file, current_ref_loc.line,
                opcode_str + " takes a value source (register or immediate); use LD to read from a memory address");
        }
        if ((opcode_str == "LD" || opcode_str == "LDZ") && !operand1_is_address) {
            fatal(current_ref_loc.file, current_ref_loc.line,
                opcode_str + " reads from a memory address; prefix the source with '@', or use CP for a value");
        }

        if (operand1_is_address) {
            opcode |= cpu::opcode_flag_srcAddr;
            operand1 = operand1.substr(1);
        }

        if (is_register(operand1)) {
            operand1_byte = compile_register(operand1);
        }
        else {
            operand_is_immediate = true;
            opcode |= cpu::opcode_flag_srcImm;

            if (is_literal(operand1)) {
                operand1_byte |= compile_literal(operand1, operand1_literal);
            }
            else if (is_label(operand1)) {
                operand_is_label = true;
                operand1_byte |= compile_label(operand1, operand1_literal);
            }
            else {
                operand_is_label = true;
                operand1_byte |= add_label(operand1, operand1_literal);
            }
        }

        u_byte operand2_byte {compile_register(operand2)};

        current_address += cpu::mm.write_byte(current_address, opcode);
        current_address += cpu::mm.write_byte(current_address, operand1_byte);
        current_address += cpu::mm.write_byte(current_address, operand2_byte);

        if (operand_is_immediate) {
            if (operand_is_label && operand1_literal.h0 == std::numeric_limits<u_hword>::max()) {
                current_address += write_label(current_address, operand1, operand1_literal);
            }
            else {
                if ((operand1_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_16b) {
                    current_address += cpu::mm.write_qword(current_address, operand1_literal.q0);
                }
                else if ((operand1_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_32b) {
                    current_address += cpu::mm.write_hword(current_address, operand1_literal.h0);
                }
                else if ((operand1_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_64b) {
                    current_address += cpu::mm.write_word(current_address, operand1_literal.w0);
                }
                else {
                    current_address += cpu::mm.write_byte(current_address, operand1_literal.b0);
                }
            }
        }
        else if (operand_is_label) {
            current_address += write_label(current_address, operand1, operand1_literal);
        }
    }

    void regimm_compiler(token_tree &tree, std::string &opcode_str) {
        u_byte opcode {opcodes[opcode_str].opcode};
        auto it {tree.value.begin()};
        current_ref_loc = { it->loc_file, it->loc_line };
        auto operand1 {it->key};
        u_byte operand1_byte {0};
        bool operand_is_immediate {false};
        bool operand_is_label {false};
        cpu::reg_value operand1_literal {0};

        if (operand1[0] == special_chars::address) {
            opcode |= cpu::opcode_flag_srcAddr;
            operand1 = operand1.substr(1);
        }

        if (is_register(operand1)) {
            operand1_byte = compile_register(operand1);
        }
        else {
            operand_is_immediate = true;
            opcode |= cpu::opcode_flag_srcImm;

            if (is_literal(operand1)) {
                operand1_byte |= compile_literal(operand1, operand1_literal);
            }
            else if (is_label(operand1)) {
                operand_is_label = true;
                operand1_byte |= compile_label(operand1, operand1_literal);
            }
            else {
                operand_is_label = true;
                operand1_byte |= add_label(operand1, operand1_literal);
            }
        }

        current_address += cpu::mm.write_byte(current_address, opcode);
        current_address += cpu::mm.write_byte(current_address, operand1_byte);

        if (operand_is_immediate) {
            if (operand_is_label && operand1_literal.h0 == std::numeric_limits<u_hword>::max()) {
                current_address += write_label(current_address, operand1, operand1_literal);
            }
            else {
                if ((operand1_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_16b) {
                    current_address += cpu::mm.write_qword(current_address, operand1_literal.q0);
                }
                else if ((operand1_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_32b) {
                    current_address += cpu::mm.write_hword(current_address, operand1_literal.h0);
                }
                else if ((operand1_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_64b) {
                    current_address += cpu::mm.write_word(current_address, operand1_literal.w0);
                }
                else {
                    current_address += cpu::mm.write_byte(current_address, operand1_literal.b0);
                }
            }
        }
        else if (operand_is_label) {
            current_address += write_label(current_address, operand1, operand1_literal);
        }
    }

    void reg_compiler(token_tree &tree, std::string &opcode_str) {
        u_byte opcode {opcodes[opcode_str].opcode};
        auto it {tree.value.begin()};
        current_ref_loc = { it->loc_file, it->loc_line };
        auto operand1 {it->key};
        u_byte operand1_byte {compile_register(operand1)};
        current_address += cpu::mm.write_byte(current_address, opcode);
        current_address += cpu::mm.write_byte(current_address, operand1_byte);
    }

    void regimm_imm_compiler(token_tree &tree, std::string &opcode_str) {
        u_byte opcode {opcodes[opcode_str].opcode};
        auto it {tree.value.begin()};
        current_ref_loc = { it->loc_file, it->loc_line };
        auto operand1 {it->key};
        ++it;
        auto operand2 {it->key};
        u_byte operand1_byte {0};
        u_byte operand2_byte {0};
        bool operand_is_immediate {false};
        bool operand1_is_label {false};
        bool operand2_is_label {false};
        cpu::reg_value operand1_literal {0};
        cpu::reg_value operand2_literal {0};

        if (operand2[0] == special_chars::address) {
            operand2 = operand2.substr(1);
        }

        if (is_register(operand1)) {
            operand1_byte = compile_register(operand1);
        }
        else {
            operand_is_immediate = true;
            opcode |= cpu::opcode_flag_srcImm;

            if (is_literal(operand1)) {
                operand1_byte |= compile_literal(operand1, operand1_literal);
            }
            else if (is_label(operand1)) {
                operand1_is_label = true;
                operand1_byte |= compile_label(operand1, operand1_literal);
            }
            else {
                operand1_is_label = true;
                operand1_byte |= add_label(operand1, operand1_literal);
            }
        }

        if (is_label(operand2)) {
            operand2_is_label = true;
            operand2_byte |= compile_label(operand2, operand2_literal);
        }
        else {
            operand2_byte = compile_literal(operand2, operand2_literal);
        }

        current_address += cpu::mm.write_byte(current_address, opcode);
        current_address += cpu::mm.write_byte(current_address, operand1_byte);
        current_address += cpu::mm.write_byte(current_address, operand2_byte);

        if (operand_is_immediate) {
            if ((operand1_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_16b) {
                current_address += cpu::mm.write_qword(current_address, operand1_literal.q0);
            }
            else if ((operand1_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_32b) {
                current_address += cpu::mm.write_hword(current_address, operand1_literal.h0);
            }
            else if ((operand1_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_64b) {
                current_address += cpu::mm.write_word(current_address, operand1_literal.w0);
            }
            else {
                current_address += cpu::mm.write_byte(current_address, operand1_literal.b0);
            }
        }
        else if (operand1_is_label) {
            current_address += write_label(current_address, operand1, operand1_literal);
        }

        if (operand2_is_label) {
            current_address += write_label(current_address, operand2, operand2_literal);
        }
        else {
            if ((operand2_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_16b) {
                current_address += cpu::mm.write_qword(current_address, operand2_literal.q0);
            }
            else if ((operand2_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_32b) {
                current_address += cpu::mm.write_hword(current_address, operand2_literal.h0);
            }
            else if ((operand2_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_64b) {
                current_address += cpu::mm.write_word(current_address, operand2_literal.w0);
            }
            else {
                current_address += cpu::mm.write_byte(current_address, operand2_literal.b0);
            }
        }
    }

    void regimm_regaddr_compiler(token_tree &tree, std::string &opcode_str) {
        u_byte opcode {opcodes[opcode_str].opcode};
        auto it {tree.value.begin()};
        current_ref_loc = { it->loc_file, it->loc_line };
        auto operand1 {it->key};
        ++it;
        auto operand2 {it->key};
        u_byte operand1_byte {0};
        bool operand_is_immediate {false};
        bool operand_is_label {false};
        cpu::reg_value operand1_literal {0};

        if (is_register(operand1)) {
            operand1_byte = compile_register(operand1);
        }
        else {
            operand_is_immediate = true;
            opcode |= cpu::opcode_flag_srcImm;

            if (is_literal(operand1)) {
                operand1_byte |= compile_literal(operand1, operand1_literal);
            }
            else if (is_label(operand1)) {
                operand_is_label = true;
                operand1_byte |= compile_label(operand1, operand1_literal);
            }
            else {
                operand_is_label = true;
                operand1_byte |= add_label(operand1, operand1_literal);
            }
        }

        u_byte operand2_byte {0};

        /* The destination of ST / CMPIND / TSTIND is a memory address; require the '@'
           marker so every memory access is explicit (card maize-43). */
        if (operand2[0] != special_chars::address) {
            fatal(it->loc_file, it->loc_line,
                opcode_str + " uses a memory-address destination; prefix the register with '@'");
        }

        operand2 = operand2.substr(1);
        operand2_byte = compile_register(operand2);

        current_address += cpu::mm.write_byte(current_address, opcode);
        current_address += cpu::mm.write_byte(current_address, operand1_byte);
        current_address += cpu::mm.write_byte(current_address, operand2_byte);

        if (operand_is_immediate) {
            if ((operand1_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_16b) {
                current_address += cpu::mm.write_qword(current_address, operand1_literal.q0);
            }
            else if ((operand1_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_32b) {
                current_address += cpu::mm.write_hword(current_address, operand1_literal.h0);
            }
            else if ((operand1_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_64b) {
                current_address += cpu::mm.write_word(current_address, operand1_literal.w0);
            }
            else {
                current_address += cpu::mm.write_byte(current_address, operand1_literal.b0);
            }
        }
        else if (operand_is_label) {
            current_address += write_label(current_address, operand1, operand1_literal);
        }
    }

    void regimm_regreg_compiler(token_tree &tree, std::string &opcode_str) {
        u_byte opcode {opcodes[opcode_str].opcode};
        auto it {tree.value.begin()};
        current_ref_loc = { it->loc_file, it->loc_line };
        auto operand1 {it->key};
        ++it;
        auto operand2 {it->key};
        ++it;
        auto operand3 {it->key};

        u_byte operand1_byte {0};
        bool operand_is_immediate {false};
        bool operand_is_label {false};
        cpu::reg_value operand1_literal {0};

        if (operand1[0] == special_chars::address) {
            opcode |= cpu::opcode_flag_srcAddr;
            operand1 = operand1.substr(1);
        }

        if (is_register(operand1)) {
            operand1_byte = compile_register(operand1);
        }
        else {
            operand_is_immediate = true;
            opcode |= cpu::opcode_flag_srcImm;

            if (is_literal(operand1)) {
                operand1_byte |= compile_literal(operand1, operand1_literal);
            }
            else if (is_label(operand1)) {
                operand_is_label = true;
                operand1_byte |= compile_label(operand1, operand1_literal);
            }
            else {
                operand_is_label = true;
                operand1_byte |= add_label(operand1, operand1_literal);
            }
        }

        u_byte operand2_byte {compile_register(operand2)};
        u_byte operand3_byte {compile_register(operand3)};

        current_address += cpu::mm.write_byte(current_address, opcode);
        current_address += cpu::mm.write_byte(current_address, operand1_byte);
        current_address += cpu::mm.write_byte(current_address, operand2_byte);
        current_address += cpu::mm.write_byte(current_address, operand3_byte);

        if (operand_is_immediate) {
            if (operand_is_label && operand1_literal.h0 == std::numeric_limits<u_hword>::max()) {
                current_address += write_label(current_address, operand1, operand1_literal);
            }
            else {
                if ((operand1_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_16b) {
                    current_address += cpu::mm.write_qword(current_address, operand1_literal.q0);
                }
                else if ((operand1_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_32b) {
                    current_address += cpu::mm.write_hword(current_address, operand1_literal.h0);
                }
                else if ((operand1_byte & cpu::opflag_imm_size) == cpu::opflag_imm_size_64b) {
                    current_address += cpu::mm.write_word(current_address, operand1_literal.w0);
                }
                else {
                    current_address += cpu::mm.write_byte(current_address, operand1_literal.b0);
                }
            }
        }
        else if (operand_is_label) {
            current_address += write_label(current_address, operand1, operand1_literal);
        }
    }
}

int main(int argc, char* argv[]) {

    /* Flags are position-independent; the first non-flag argument is the input
       file. Unrecognized --flags are ignored rather than fatal (maize-46). */
    std::string input_file {};
    std::string base_path_arg {};

    /* --base-path and --source-name are two-token flags: each consumes the
       following argv slot as its value (maize-49). */
    for (int i = 1; i < argc; ++i) {
        std::string arg {argv[i]};

        if (arg == "--check") {
            check_only = true;
        }
        else if (arg == "--stdin") {
            stdin_mode = true;
        }
        else if (arg == "--base-path" && i + 1 < argc) {
            base_path_arg = argv[++i];
        }
        else if (arg == "--source-name" && i + 1 < argc) {
            source_name = argv[++i];
        }
        else if (arg.rfind("--", 0) == 0) {
            /* ignore unknown flags */
        }
        else if (input_file.empty()) {
            input_file = arg;
        }
    }

    if (stdin_mode) {
        /* Stdin is a check-only surface: there is no input path to derive a
           .bin target from, and no file directory to resolve INCLUDEs
           against, so both must be explicit. */
        if (!check_only) {
            std::cerr << "mazm: error: --stdin requires --check" << std::endl;
            return 1;
        }

        if (base_path_arg.empty()) {
            std::cerr << "mazm: error: --stdin requires --base-path" << std::endl;
            return 1;
        }

        try {
            base_path = base_path_arg;
            current_file = source_name;
            current_line = 1;
            token_line = 1;

            token_tree tree {source_name};
            tokenize(std::cin, tree);
            compile(tree);
        }
        catch (const asm_error &e) {
            std::cerr << e.what() << std::endl;
            return 1;
        }
        catch (const std::exception &e) {
            std::cerr << "mazm: error: " << e.what() << std::endl;
            return 1;
        }

        return 0;
    }

    if (!input_file.empty()) {
        try {
            auto input_path = std::filesystem::path(input_file);
            auto canonical_path = std::filesystem::canonical(input_path).make_preferred();
            auto parent_path = canonical_path.parent_path().make_preferred();
            base_path = base_path_arg.empty() ? parent_path.string() : base_path_arg;

            if (!check_only) {
                std::cout << "Assembling from " << canonical_path.string() << std::endl;
            }

            assemble(canonical_path.string());
        }
        catch (const asm_error &e) {
            /* Already formatted as "mazm: <file>:<line>: error: <msg>". */
            std::cerr << e.what() << std::endl;
            return 1;
        }
        catch (const std::exception &e) {
            /* Anything else (e.g. a missing input file makes std::filesystem
               throw): still exit nonzero with a mazm-prefixed message rather than
               an unhandled-exception abort. */
            std::cerr << "mazm: error: " << e.what() << std::endl;
            return 1;
        }
    }

    return 0;
}
