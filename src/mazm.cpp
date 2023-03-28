#include <filesystem>
#include <unordered_map>
#include <iostream>
#include <sstream>
#include <list>
#include <algorithm>
#include <utility>
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
        escape = '\\'
    };

    std::string base_path {};
    std::string current_token {};
    maize::hword current_address {};
    std::unordered_map<std::string, maize::hword> labels {};
    std::unordered_map<maize::hword, std::string> fixups {};
    std::map<maize::word, std::vector<byte>> memory_map {};

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

        expression<T>& add(T token) {
            value.push_back(expression<T>(token));
            return value.back();
        }
    };

    typedef expression<std::string> token_tree;

    typedef parser_state(*tokenizer_fn)(std::fstream &fin, token_tree &tree, char c);
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
        opcode_data(byte opcode_init, tokenizer_fn tokenizer_init, compiler_fn compiler_init) : 
            opcode(opcode_init),
            keyword_data(tokenizer_init, compiler_init)
        { }

        byte opcode;
    };

    void assemble(std::string file_path);
    void tokenize(std::fstream &fin, token_tree &tree);
    void compile(token_tree &tree);

    literal_state parse_literal(std::fstream &fin, token_tree& tree, char c);

    parser_state process_char_stream(parser_state state, std::fstream &fin, token_tree &tree, char c);

    parser_state parse_keyword(std::fstream &fin, token_tree &tree, char c);
    parser_state parse_code_block(std::fstream &fin, token_tree &tree, char c);
    parser_state process_keyword(parser_state state, std::fstream &fin, token_tree &tree, char c);
    parser_state parse_opcode(std::fstream &fin, token_tree &tree, char c);

    parser_state address_tokenizer(std::fstream &fin, token_tree &tree, char c);
    parser_state string_tokenizer(std::fstream &fin, token_tree &tree, char c);
    parser_state label_tokenizer(std::fstream &fin, token_tree &tree, char c);
    parser_state data_tokenizer(std::fstream &fin, token_tree &tree, char c);
    parser_state include_tokenizer(std::fstream &fin, token_tree &tree, char c);
    parser_state opcode_0param_tokenizer(std::fstream &fin, token_tree &tree, char c);
    parser_state opcode_1param_tokenizer(std::fstream &fin, token_tree &tree, char c);
    parser_state opcode_2param_tokenizer(std::fstream &fin, token_tree &tree, char c);

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
        { "ST",     {cpu::instr::st_opcode     , opcode_2param_tokenizer, regimm_regaddr_compiler}},
        { "ADD",    {cpu::instr::add_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "SUB",    {cpu::instr::sub_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "MUL",    {cpu::instr::mul_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "DIV",    {cpu::instr::div_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "MOD",    {cpu::instr::mod_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "AND",    {cpu::instr::and_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "OR",     {cpu::instr::or_opcode     , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "NOR",    {cpu::instr::nor_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "NAND",   {cpu::instr::nand_opcode   , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "XOR",    {cpu::instr::xor_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "SHL",    {cpu::instr::shl_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "SHR",    {cpu::instr::shr_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "CMP",    {cpu::instr::cmp_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "TEST",   {cpu::instr::test_opcode   , opcode_2param_tokenizer, regimm_reg_compiler}},
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
        { "BRK",    {cpu::instr::brk_opcode    , opcode_0param_tokenizer, no_operand_compiler}}
    };

    void assemble(std::string file_path) {
        std::filesystem::path asm_path {std::filesystem::canonical(file_path)};
        std::fstream fin(file_path, std::fstream::in);
        token_tree tree {file_path};
        tokenize(fin, tree);
        compile(tree);

        std::filesystem::path bin_path {asm_path};
        std::filesystem::path ext {"bin"};

        bin_path.replace_extension(ext);

        /* write here */
        std::cout << "Output to " << bin_path << std::endl;
        std::ofstream bin(bin_path, std::fstream::binary);

        maize::word last_block {cpu::mm.last_block()};
        maize::word end {last_block + cpu::mm.block_size};
        maize::word current_address {0};

        while (current_address < end) {
            char c = cpu::mm.read_byte(current_address);
            bin.write(&c, 1);
            ++current_address;
        }

        bin.close();
    }

    void tokenize(std::fstream &fin, token_tree &tree) {
        parser_state state {parser_state::whitespace};
        char c {};

        while (fin >> std::noskipws >> c) {
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
            cpu::mm.write_hword(pair.first, labels[pair.second]);
        }
    }

    literal_state parse_literal(std::fstream &fin, token_tree &tree, char c) {
        literal_state state = literal_state::start;

        while (c >= 0) {
            switch (state) {
            case literal_state::start:
                if (isspace(c)) {
                    state = literal_state::start;
                }
                else if (c == ',' || c == '`') {
                    state = literal_state::value;
                }
                else {
                    current_token.push_back(c);
                    state = literal_state::value;
                }
                break;

            case literal_state::type:
                break;

            case literal_state::value:
                if (c == ',' || c == '`') {
                    break;
                }

                if (isspace(c)) {
                    tree.add(current_token);
                    current_token.clear();
                    return literal_state::end;
                }
                else {
                    current_token.push_back(c);
                    state = literal_state::value;
                }

                break;

            case literal_state::end:
                return state;
                break;

            default:
                break;
            }

            fin >> std::noskipws >> c;
        }

        return literal_state::end;
    }

    parser_state process_char_stream(parser_state state, std::fstream &fin, token_tree &tree, char c) {
        if (isspace(c)) {
            return parser_state::whitespace;
        }

        if (isalnum(c) || c == ',' || c == '`') {
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

        current_token.push_back(c);
        return state;
    }

    parser_state process_keyword(parser_state state, std::fstream &fin, token_tree &tree, char c) {
        /* TODO: Why am I processing this up here instead of in parse_keyword with the rest of the keywords?
        I didn't document this in the C# version, so I have no idea. */
        if (isspace(c)) {
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

    parser_state parse_keyword(std::fstream &fin, token_tree &tree, char c) {
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

        return parser_state::whitespace;
    }

    parser_state parse_code_block(std::fstream &fin, token_tree &tree, char c) {
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

        } while (fin >> std::noskipws >> c);

        return state;
    }

    parser_state parse_opcode(std::fstream &fin, token_tree &tree, char c) {
        std::string keyword = current_token;
        current_token.clear();
        token_tree &sub_tree = tree.add(keyword);

        if (opcodes.contains(keyword)) {
            return opcodes[keyword].tokenizer(fin, sub_tree, c);
        }

        return parser_state::whitespace;
    }

    parser_state address_tokenizer(std::fstream& fin, token_tree &tree, char c) {
        while (parse_literal(fin, tree, c) != literal_state::end) {
        }

        return parser_state::whitespace;
    }

    parser_state label_tokenizer(std::fstream &fin, token_tree &tree, char c) {
        label_state state {label_state::start};

        while (fin.peek() >= 0) {
            fin >> std::noskipws >> c;

            switch (state) {
            case label_state::start:
                if (isalpha(c) || c == '_') {
                    state = label_state::name;
                    current_token.push_back(c);
                }

                break;

            case label_state::name:
                if (isalnum(c) || c == '_') {
                    state = label_state::name;
                    current_token.push_back(c);
                }
                else if (isspace(c)) {
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

    parser_state string_tokenizer(std::fstream &fin, token_tree &tree, char c) {
        auto state = string_state::start;

        while (fin.peek() >= 0) {
            fin >> std::noskipws >> c;

            if (c == '\r' || c == '\n') {
                return parser_state::newline;
            }

            switch (state) {
            case string_state::start:
                if (c == special_chars::comment_start) {
                    return parser_state::comment;
                }

                if (c == special_chars::quote) {
                    state = string_state::read;
                }

                break;
            
            case string_state::read:
                if (c == special_chars::escape) {
                    current_token.push_back(c);
                    state = string_state::escape;
                    break;
                }

                if (c == special_chars::quote) {
                    tree.add(current_token);
                    current_token.clear();
                    return parser_state::whitespace;
                }

                current_token.push_back(c);

                break;
            
            case string_state::escape:
                current_token.push_back(c);
                state = string_state::read;
                break;
            
            case string_state::end:
                break;
            
            default:
                break;
            }
        }

        return parser_state::whitespace;
    }

    parser_state data_tokenizer(std::fstream& fin, token_tree &tree, char c) {
        while (fin.peek() >= 0) {
            fin >> std::noskipws >> c;

            if (c == '.' || c == '`') {
                continue;
            }

            if (isalnum(c) || c == special_chars::bin || c == special_chars::dec || c == special_chars::hex) {
                current_token.push_back(c);
            }
            else if ((isspace(c) || c == '\r' || c == '\n' || c == special_chars::comment_start) && !current_token.empty()) {
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

    parser_state include_tokenizer(std::fstream& fin, token_tree &tree, char c) {
        token_tree string_tree {""};
        string_tokenizer(fin, string_tree, c);
        auto file_path = string_tree.value.begin()->key;
        std::filesystem::path include_path = base_path;
        include_path.append(file_path);

        std::fstream finclude(include_path.string(), std::fstream::in);
        tokenize(finclude, tree);
        finclude.close();

        return parser_state::whitespace;
    }

    parser_state opcode_0param_tokenizer(std::fstream &fin, token_tree &tree, char c) {
        return parser_state::whitespace;
    }

    parser_state opcode_1param_tokenizer(std::fstream &fin, token_tree &tree, char c) {
        opcode_state state {opcode_state::start};

        while (fin.peek() >= 0) {
            fin >> std::noskipws >> c;

            switch (state) {
            case opcode_state::start:
                if (isspace(c)) {
                    continue;
                }
                else if (c == ',' || c == '`') {
                    state = opcode_state::operand1;
                }
                else {
                    state = opcode_state::operand1;
                    current_token.push_back(c);
                }

                break;

            case opcode_state::operand1:
                if (isspace(c)) {
                    tree.add(current_token);
                    current_token.clear();
                    return parser_state::whitespace;
                }
                else if (c == ',' || c == '`') {
                    break;
                }
                else {
                    current_token.push_back(c);
                }

                break;
            }
        }

        return parser_state::whitespace;
    }

    parser_state opcode_2param_tokenizer(std::fstream &fin, token_tree &tree, char c) {
        opcode_state state {opcode_state::start};

        while (fin.peek() >= 0) {
            fin >> std::noskipws >> c;

            switch (state) {
            case opcode_state::start:
                if (isspace(c)) {
                    continue;
                }
                else if (c == ',' || c == '`') {
                    state = opcode_state::operand1;
                }
                else {
                    state = opcode_state::operand1;
                    current_token.push_back(c);
                }

                break;

            case opcode_state::operand1:
                if (isspace(c)) {
                    tree.add(current_token);
                    current_token.clear();
                    state = opcode_state::operand2;
                }
                else if (c == ',' || c == '`') {
                    break;
                }
                else {
                    current_token.push_back(c);
                }

                break;

            case opcode_state::operand2:
                if (isspace(c)) {
                    tree.add(current_token);
                    current_token.clear();
                    return parser_state::whitespace;
                }
                else if (c == ',' || c == '`') {
                    break;
                }
                else {
                    current_token.push_back(c);
                }

                break;
            }
        }

        return parser_state::whitespace;
    }


    maize::hword convert_label_string(std::string &value) {
        maize::hword hvalue {0};
        char type = value[0];

        std::stringstream cvt;

        if (type == special_chars::hex) {
            cvt << std::hex << value.substr(1);
        }
        else if (type == special_chars::dec) {
            cvt << std::dec << value.substr(1);
        }
        else if (type == special_chars::bin) {
            // cvt << std::hex << value.substr(1);
            throw std::logic_error("binary conversion not implemented yet");
        }
        else {
            hvalue = std::numeric_limits<hword>::max();
        }

        cvt >> hvalue;
        return hvalue;
    }

    byte compile_label(std::string &label, cpu::reg_value &value) {
        value = labels[label];
        return cpu::opflag_imm_size_32b;
    }

    byte compile_hex_literal(std::string &literal, cpu::reg_value &value) {
        auto len = literal.length();
        std::stringstream cvt;
        cvt << std::hex << literal;
        cvt >> value.w0;
        byte type_byte = cpu::opflag_imm_size_32b;

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

    byte compile_literal(std::string &literal, cpu::reg_value &value) {
        char type_char = literal[0];
        byte type_byte = 0;
        value = 0;

        if (type_char == special_chars::hex) {
            std::string sub = literal.substr(1);
            type_byte = compile_hex_literal(sub, value);
        }
        else if (type_char == special_chars::dec) {
            throw std::logic_error("Decimal literals not implemented yet");
        }
        else if (type_char == special_chars::bin) {
            throw std::logic_error("Binary literals not implemented yet");
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
        auto label = it->key;
        ++it;
        auto value = it->key;

         auto hvalue = convert_label_string(value);
         labels[label] = hvalue;
    }

    void address_compiler(token_tree &tree, std::string &opcode_str) {
        auto data_string {tree.value.begin()->key};
        hword data = 0;

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
                        current_address += cpu::mm.write_byte(current_address, static_cast<byte>(c));
                        break;

                    case '0':
                        current_address += cpu::mm.write_byte(current_address, static_cast<byte>(0));
                        break;

                    case 't':
                        current_address += cpu::mm.write_byte(current_address, static_cast<byte>('\t'));
                        break;

                    case 'r':
                        current_address += cpu::mm.write_byte(current_address, static_cast<byte>('\r'));
                        break;

                    case 'n':
                        current_address += cpu::mm.write_byte(current_address, static_cast<byte>('\n'));
                        break;

                    case 'a':
                        current_address += cpu::mm.write_byte(current_address, static_cast<byte>('\a'));
                        break;

                    case 'b':
                        current_address += cpu::mm.write_byte(current_address, static_cast<byte>('\b'));
                        break;

                    case 'f':
                        current_address += cpu::mm.write_byte(current_address, static_cast<byte>('\f'));
                        break;

                    case 'e':
                        current_address += cpu::mm.write_byte(current_address, static_cast<byte>(0x1B));
                        break;

                    case 'v':
                        current_address += cpu::mm.write_byte(current_address, static_cast<byte>('\v'));
                        break;

                    }
                }
                else {
                    current_address += cpu::mm.write_byte(current_address, static_cast<byte>(c));
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
        auto label {tree.value.begin()->key};
        auto address = convert_label_string(label);

        if (address == std::numeric_limits<hword>::max()) {
            if (labels.contains(label)) {
                address = labels[label];
            }
            else {
                address = std::numeric_limits<hword>::max();
            }

            if (address == std::numeric_limits<hword>::max()) {
                address = current_address;
                labels[label] = address;
            }
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

    std::unordered_map<std::string, byte> reg_map {
        {"A", cpu::opflag_reg_a},
        {"B", cpu::opflag_reg_b},
        {"C", cpu::opflag_reg_c},
        {"D", cpu::opflag_reg_d},
        {"E", cpu::opflag_reg_e},
        {"G", cpu::opflag_reg_g},
        {"H", cpu::opflag_reg_h},
        {"J", cpu::opflag_reg_j},
        {"K", cpu::opflag_reg_k},
        {"L", cpu::opflag_reg_l},
        {"M", cpu::opflag_reg_m},
        {"Z", cpu::opflag_reg_z},
        {"F", cpu::opflag_reg_f},
        {"IN", cpu::opflag_reg_in},
        {"P", cpu::opflag_reg_p},
        {"S", cpu::opflag_reg_s}
    };

    std::unordered_map<std::string, byte> subreg_map {
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

    byte compile_register(std::string &reg) {
        std::string reg_str;
        std::transform(reg.begin(), reg.end(), std::back_inserter(reg_str), ::toupper);
        byte reg_byte {cpu::opflag_subreg_w0};

        /* Get some special cases out of the way */
        if (reg_str == "SP") {
            reg_byte = cpu::opflag_reg_s | cpu::opflag_subreg_h0;
        }
        else if (reg_str == "BP") {
            reg_byte = cpu::opflag_reg_s | cpu::opflag_subreg_h1;
        }
        else if (reg_str == "PC") {
            reg_byte = cpu::opflag_reg_p | cpu::opflag_subreg_h0;
        }
        else if (reg_str == "CS") {
            reg_byte = cpu::opflag_reg_p | cpu::opflag_subreg_h1;
        }
        else if (reg_str == "FL") {
            reg_byte = cpu::opflag_reg_f | cpu::opflag_subreg_h0;
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

    hword write_label(hword address, std::string &label, cpu::reg_value value) {
        if (value.h0 == std::numeric_limits<hword>::max()) {
            fixups[current_address] = label;
        }

        return cpu::mm.write_hword(address, value.h0);
    }

    void no_operand_compiler(token_tree &tree, std::string &opcode_str) {
        byte opcode {opcodes[opcode_str].opcode};
        current_address += cpu::mm.write_byte(current_address, opcode);
    }

    void regimm_reg_compiler(token_tree &tree, std::string &opcode_str) {
        byte opcode {opcodes[opcode_str].opcode};
        auto it {tree.value.begin()};
        auto operand1 {it->key};
        ++it;
        auto operand2 {it->key};
        byte operand1_byte {0};
        bool operand_is_immediate {false};
        bool operand_is_label {false};
        cpu::reg_value operand1_literal {0};

        if (operand1[0] == special_chars::address) {
            opcode |= cpu::opcode_flag_srcAddr;
            operand1 = operand1.substr(1);
        }

        if (is_literal(operand1)) {
            operand_is_immediate = true;
            opcode |= cpu::opcode_flag_srcImm;
            operand1_byte |= compile_literal(operand1, operand1_literal);
        }
        else if (is_label(operand1)) {
            operand_is_label = true;
            operand_is_immediate = true;
            opcode |= cpu::opcode_flag_srcImm;
            operand1_byte |= compile_label(operand1, operand1_literal);
        }
        else {
            operand1_byte = compile_register(operand1);
        }

        byte operand2_byte {compile_register(operand2)};

        current_address += cpu::mm.write_byte(current_address, opcode);
        current_address += cpu::mm.write_byte(current_address, operand1_byte);
        current_address += cpu::mm.write_byte(current_address, operand2_byte);

        if (operand_is_immediate) {
            if (operand_is_label && operand1_literal.h0 == std::numeric_limits<hword>::max()) {
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
        byte opcode {opcodes[opcode_str].opcode};
        auto it {tree.value.begin()};
        auto operand1 {it->key};
        byte operand1_byte {0};
        bool operand_is_immediate {false};
        bool operand_is_label {false};
        cpu::reg_value operand1_literal {0};

        if (operand1[0] == special_chars::address) {
            opcode |= cpu::opcode_flag_srcAddr;
            operand1 = operand1.substr(1);
        }

        if (is_literal(operand1)) {
            operand_is_immediate = true;
            opcode |= cpu::opcode_flag_srcImm;
            operand1_byte |= compile_literal(operand1, operand1_literal);
        }
        else if (is_label(operand1)) {
            operand_is_label = true;
            operand_is_immediate = true;
            opcode |= cpu::opcode_flag_srcImm;
            operand1_byte |= compile_label(operand1, operand1_literal);
        }
        else {
            operand1_byte = compile_register(operand1);
        }

        current_address += cpu::mm.write_byte(current_address, opcode);
        current_address += cpu::mm.write_byte(current_address, operand1_byte);

        if (operand_is_immediate) {
            if (operand_is_label && operand1_literal.h0 == std::numeric_limits<hword>::max()) {
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
        byte opcode {opcodes[opcode_str].opcode};
        auto it {tree.value.begin()};
        auto operand1 {it->key};
        byte operand1_byte {compile_register(operand1)};
        current_address += cpu::mm.write_byte(current_address, opcode);
        current_address += cpu::mm.write_byte(current_address, operand1_byte);
    }

    void regimm_imm_compiler(token_tree &tree, std::string &opcode_str) {
        byte opcode {opcodes[opcode_str].opcode};
        auto it {tree.value.begin()};
        auto operand1 {it->key};
        ++it;
        auto operand2 {it->key};
        byte operand1_byte {0};
        byte operand2_byte {0};
        bool operand_is_immediate {false};
        bool operand1_is_label {false};
        bool operand2_is_label {false};
        cpu::reg_value operand1_literal {0};
        cpu::reg_value operand2_literal {0};

        if (operand2[0] == special_chars::address) {
            operand2 = operand2.substr(1);
        }

        if (is_literal(operand1)) {
            operand_is_immediate = true;
            opcode |= cpu::opcode_flag_srcImm;
            operand1_byte |= compile_literal(operand1, operand1_literal);
        }
        else if (is_label(operand1)) {
            operand1_is_label = true;
            operand_is_immediate = true;
            opcode |= cpu::opcode_flag_srcImm;
            operand1_byte |= compile_label(operand1, operand1_literal);
        }
        else {
            operand1_byte = compile_register(operand1);
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
        byte opcode {opcodes[opcode_str].opcode};
        auto it {tree.value.begin()};
        auto operand1 {it->key};
        ++it;
        auto operand2 {it->key};
        byte operand1_byte {0};
        bool operand_is_immediate {false};
        bool operand_is_label {false};
        cpu::reg_value operand1_literal {0};

        if (is_literal(operand1)) {
            operand_is_immediate = true;
            opcode |= cpu::opcode_flag_srcImm;
            operand1_byte |= compile_literal(operand1, operand1_literal);
        }
        else if (is_label(operand1)) {
            operand_is_label = true;
            operand_is_immediate = true;
            opcode |= cpu::opcode_flag_srcImm;
            operand1_byte |= compile_label(operand1, operand1_literal);
        }
        else {
            operand1_byte = compile_register(operand1);
        }

        byte operand2_byte {0};

        if (operand2[0] == special_chars::address) {
            operand2 = operand2.substr(1);
        }

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

}

int main(int argc, char* argv[]) {

    if (argc > 1) {
        std::string input_file {argv[1]};
        auto input_path = std::filesystem::path(input_file);
        auto canonical_path = std::filesystem::canonical(input_path);
        auto parent_path = canonical_path.parent_path();
        base_path = parent_path.string();
        canonical_path = canonical_path.native();

        // canonical_path.native()

        std::cout << "Assembling from " << canonical_path << std::endl;

        assemble(canonical_path.string());
    }
}
