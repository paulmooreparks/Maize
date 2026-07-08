#include <filesystem>
#include <unordered_map>
#include <iostream>
#include <sstream>
#include <list>
#include <algorithm>
#include <utility>
#include <stdexcept>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <cstdint>
#include "maize.h"
#include "maize_obj.h"
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

    /* Circular-INCLUDE detection (maize-37). Canonical (resolved, symlink-free)
       paths of every INCLUDE frame currently open, innermost last. A path
       already on this chain being reopened is a self-include or a mutual
       A-INCLUDEs-B / B-INCLUDEs-A cycle; without this guard, include_tokenizer
       recurses through tokenize() with no bound until the process stack
       overflows. The top-level file (assemble()'s own argument) seeds the
       chain so a top-level self-include is caught at the same depth as any
       other frame, not one level later. */
    std::vector<std::string> include_chain {};

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
    /* Ordered by fixup address (maize-50) so undefined-label diagnostics come
       out in source-reference order, deterministically. */
    std::map<maize::u_hword, std::string> fixups {};
    std::map<maize::u_word, std::vector<u_byte>> memory_map {};

    /* Object-emission mode (maize-12). When emit_object is set (mazm -c), the
       assembler emits a relocatable .mzo instead of a resolved flat .bin: every
       symbolic operand becomes a relocation (never resolved inline), labels
       become symbols, and content is partitioned into CODE/RODATA/DATA/BSS
       sections. The flat path is completely untouched, preserving hello.bin. */
    bool emit_object {false};

    /* A contiguous run of the flat assembly image that belongs to one section
       kind. SECTION directives close the current span and open a new one; the
       .mzo writer slices cpu::mm by these spans. */
    struct sec_span {
        maize::u_byte kind;
        maize::u_word start;
        maize::u_word end;
    };
    std::vector<sec_span> obj_spans {};
    maize::u_byte obj_cur_kind {maize::obj::SEC_CODE};
    maize::u_word obj_cur_start {0};

    /* One relocation: the flat address of the immediate bytes to patch, the
       referenced symbol name, and the width-keyed relocation type. */
    struct obj_reloc_rec {
        maize::u_word flat_off;
        std::string symbol;
        maize::u_byte r_type;
    };
    std::vector<obj_reloc_rec> obj_relocs {};

    /* Names exported via the GLOBAL directive (GLOBAL binding); all other
       defined labels stay LOCAL. */
    std::set<std::string> obj_exports {};

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

    /* Object-mode directives (maize-12). Registered as keywords in the LABEL /
       DATA / STRING style; each reads a single operand via the shared
       one-parameter tokenizer. In flat mode their compilers are no-ops. */
    void section_compiler(token_tree &tree, std::string &opcode_str);
    void global_compiler(token_tree &tree, std::string &opcode_str);
    void zero_compiler(token_tree &tree, std::string &opcode_str);
    void write_object(std::string const &path);

    std::unordered_map<std::string, keyword_data> keywords {
        { "ADDRESS", {address_tokenizer, address_compiler}},
        { "LABEL", {label_tokenizer, label_compiler}},
        { "DATA", {data_tokenizer, data_compiler}},
        { "STRING", {string_tokenizer, string_compiler}},
        { "INCLUDE", {include_tokenizer, nullptr}},
        { "CODE", {nullptr, code_compiler}},
        { "SECTION", {opcode_1param_tokenizer, section_compiler}},
        { "GLOBAL", {opcode_1param_tokenizer, global_compiler}},
        { "ZERO", {opcode_1param_tokenizer, zero_compiler}}
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
        { "SAR",    {cpu::instr::sar_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "CMP",    {cpu::instr::cmp_opcode    , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "TEST",   {cpu::instr::test_opcode   , opcode_2param_tokenizer, regimm_reg_compiler}},
        { "CMPXCHG",{cpu::instr::cmpxchg_opcode, opcode_3param_tokenizer, regimm_regreg_compiler}},
        { "LEA",    {cpu::instr::lea_opcode,     opcode_3param_tokenizer, regimm_regreg_compiler}},
        { "MULW",   {cpu::instr::mulw_opcode,    opcode_3param_tokenizer, regimm_regreg_compiler}},
        { "UMULW",  {cpu::instr::umulw_opcode,   opcode_3param_tokenizer, regimm_regreg_compiler}},
        { "CPZ",    {cpu::instr::cpz_opcode,     opcode_2param_tokenizer, regimm_reg_compiler}},
        { "INC",    {cpu::instr::inc_opcode    , opcode_1param_tokenizer, reg_compiler}},
        { "DEC",    {cpu::instr::dec_opcode    , opcode_1param_tokenizer, reg_compiler}},
        { "NOT",    {cpu::instr::not_opcode    , opcode_1param_tokenizer, reg_compiler}},
        { "OUT",    {cpu::instr::out_opcode    , opcode_2param_tokenizer, regimm_imm_compiler}},
        { "LNGJMP", {cpu::instr::lngjmp_opcode , opcode_1param_tokenizer, regimm_compiler}},
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
        { "SETZ",   {cpu::instr::setz_opcode   , opcode_1param_tokenizer, reg_compiler}},
        { "SETNZ",  {cpu::instr::setnz_opcode  , opcode_1param_tokenizer, reg_compiler}},
        { "SETLT",  {cpu::instr::setlt_opcode  , opcode_1param_tokenizer, reg_compiler}},
        { "SETGE",  {cpu::instr::setge_opcode  , opcode_1param_tokenizer, reg_compiler}},
        { "SETGT",  {cpu::instr::setgt_opcode  , opcode_1param_tokenizer, reg_compiler}},
        { "SETLE",  {cpu::instr::setle_opcode  , opcode_1param_tokenizer, reg_compiler}},
        { "SETB",   {cpu::instr::setb_opcode   , opcode_1param_tokenizer, reg_compiler}},
        { "SETAE",  {cpu::instr::setae_opcode  , opcode_1param_tokenizer, reg_compiler}},
        { "SETA",   {cpu::instr::seta_opcode   , opcode_1param_tokenizer, reg_compiler}},
        { "SETBE",  {cpu::instr::setbe_opcode  , opcode_1param_tokenizer, reg_compiler}},
        { "CMPIND", {cpu::instr::cmpind_opcode , opcode_2param_tokenizer, regimm_regaddr_compiler}},
        { "INT",    {cpu::instr::int_opcode    , opcode_1param_tokenizer, regimm_compiler}},
        { "TSTIND", {cpu::instr::testind_opcode, opcode_2param_tokenizer, regimm_regaddr_compiler}},
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

    /* Environment / I-O failure (maize-50). Deliberately NOT derived from
       asm_error so it sails past every recovery catch: a missing include must
       be one diagnostic, not itself plus a cascade of undefined labels. */
    struct env_error : public std::runtime_error {
        env_error(std::string const &msg) : std::runtime_error(msg) { }
    };

    [[noreturn]] void fatal_env(std::string const &file, int line, std::string const &msg) {
        std::ostringstream os;
        os << "mazm: " << file << ":" << line << ": error: " << msg;
        throw env_error(os.str());
    }

    /* Multi-error collection (maize-50). Every recovered diagnostic flows
       through record_diag; at the cap it appends a final stopping line and
       throws error_limit_reached, which no recovery catch intercepts, so
       collection ends cleanly however deep the INCLUDE recursion is. */
    struct error_limit_reached { };

    constexpr size_t max_diags {50};
    std::vector<std::string> diags {};

    void record_diag(std::string const &formatted) {
        diags.push_back(formatted);

        if (diags.size() >= max_diags) {
            std::ostringstream os;
            os << "mazm: " << current_file << ":" << current_line
               << ": error: too many errors; stopping";
            diags.push_back(os.str());
            throw error_limit_reached {};
        }
    }

    void flush_diags() {
        for (auto const &line : diags) {
            std::cerr << line << std::endl;
        }
    }

    /* Read one byte without skipping whitespace (the project-wide idiom was
       "fin >> std::noskipws >> c"). Centralized so newline counting happens in
       exactly one place. Only '\n' advances the line counter, so both LF and
       CRLF files count one line per line. */
    /* True when the last character consumed was a newline (maize-50). The
       recovery resync cannot trust any frame's local copy of the current
       character: a tokenizer that throws may already have consumed through
       the newline in its own frame, leaving the catcher's copy stale. */
    bool at_line_start {true};

    std::istream& read_char(std::istream &fin, char &c) {
        fin >> std::noskipws >> c;

        if (fin) {
            at_line_start = (c == '\n');

            if (c == '\n') {
                ++current_line;
            }
        }

        return fin;
    }

    /* Panic-mode resynchronization (maize-50): discard the partial token and
       consume the remainder of the current line so tokenization resumes at a
       clean statement boundary. Reads the stream's own position via
       at_line_start rather than any caller's stale character copy. */
    void resync_to_newline(std::istream &fin) {
        current_token.clear();

        char c {};
        while (!at_line_start && read_char(fin, c)) {
        }
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

        /* Object mode (maize-12) emits <file>.mzo; the default flat path emits
           <file>.bin, byte for byte unchanged. */
        std::filesystem::path out_path {asm_path};
        out_path.replace_extension(emit_object ? "mzo" : "bin");
        std::filesystem::path bin_path {out_path};

        /* Stale-binary rule (maize-13, AC9): remove any pre-existing output up
           front, so a failed assembly never leaves a previously-good artifact
           sitting at the target path looking like a fresh build for the now-broken
           source. In --check mode nothing is produced, so nothing may be
           destroyed either: a broken intermediate save must not cost the last-good
           binary. */
        if (!check_only) {
            std::error_code remove_ec;
            std::filesystem::remove(bin_path, remove_ec);
        }

        std::fstream fin(file_path, std::fstream::in);
        current_file = file_path;
        current_line = 1;
        token_line = 1;
        skip_bom(fin);

        /* Circular INCLUDE detection (maize-37): seed the chain with the
           top-level file itself, so a top-level self-include (INCLUDE'ing the
           very file being assembled) is caught by include_tokenizer at the
           same depth as any other frame's self-include, not one level later. */
        include_chain.clear();
        include_chain.push_back(asm_path.string());

        token_tree tree {file_path};
        tokenize(fin, tree);
        compile(tree);

        if (check_only) {
            return;
        }

        if (!diags.empty()) {
            /* Recovered errors were recorded (maize-50): success is the only
               state that produces a binary. The up-front stale-.bin removal
               already ran, per the maize-13 rule. */
            return;
        }

        if (emit_object) {
            std::cout << "Output to " << bin_path.string() << std::endl;
            write_object(bin_path.string());
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
            /* Recovery site 1 of 5 (maize-50): a source error anywhere below
               records its diagnostic, resyncs to the next line, and
               tokenization continues. env_error and error_limit_reached
               deliberately pass through. Nodes added by the failed construct
               are pruned: a partially-tokenized node reaching its compiler is
               undefined behavior (missing operand children). */
            auto nodes_before = tree.value.size();

            try {
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
            catch (const asm_error &e) {
                record_diag(e.what());

                while (tree.value.size() > nodes_before) {
                    tree.value.pop_back();
                }

                resync_to_newline(fin);
                state = parser_state::newline;
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

            /* Recovery site 3 of 5 (maize-50): a bad node records and the
               compile continues with the next node. */
            try {
                compiler(sub_tree, key);
            }
            catch (const asm_error &e) {
                record_diag(e.what());
            }
        }

        /* Object mode (maize-12) never resolves addresses inline: every symbolic
           operand is already recorded as a relocation, and undefined labels are
           legal (they become UNDEF symbols for the linker). Skip the flat
           inline-fixup pass entirely. */
        for (auto &pair : fixups) {
            if (emit_object) {
                break;
            }
            /* Recovery site 5 of 5 (maize-50): every undefined label reports;
               nothing is written for an unresolved entry. */
            try {
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
            catch (const asm_error &e) {
                record_diag(e.what());
            }
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

            /* EOF/read-failure guard (maize-37, cross-ref maize-52): the loop
               used to keep going on read_char's return value being ignored,
               trusting the (unmodified-on-failure) char c to eventually go
               negative on its own -- it doesn't, for a stream that has already
               produced at least one character. Check the stream state directly. */
            if (!read_char(fin, c)) {
                break;
            }
        }

        /* EOF reached (maize-37, cross-ref maize-52): flush a trailing token
           that was never terminated by whitespace instead of silently dropping
           it. address_tokenizer (ADDRESS's shared literal parser) and
           label_tokenizer's value branch (LABEL) both fed a short/empty node to
           their compiler otherwise, crashing on a dereference past
           tree.value.end(). Mirrors the EOF-flush pattern already used by
           opcode_1/2/3param_tokenizer. */
        if (!current_token.empty()) {
            tree.add(current_token);
            current_token.clear();
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
            /* Recovery site 2 of 5 (maize-50): an error on one instruction
               under a label resyncs and continues WITHIN the block loop, so
               the rest of the routine stays attached to its label instead of
               re-tokenizing as disconnected top-level fragments. Partial
               nodes from the failed instruction are pruned, same as site 1. */
            auto nodes_before = sub_tree.value.size();

            try {
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
            }
            catch (const asm_error &e) {
                record_diag(e.what());

                while (sub_tree.value.size() > nodes_before) {
                    sub_tree.value.pop_back();
                }

                resync_to_newline(fin);
                state = parser_state::newline;
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
        int instr_line {current_line};

        while (parse_literal(fin, tree, c) != literal_state::end) {
        }

        /* Bare ADDRESS at EOF (maize-37, cross-ref maize-52): parse_literal now
           flushes any trailing token it managed to accumulate, but a keyword
           with no operand at all (EOF arrives before any non-whitespace
           character) still leaves a 0-child node. address_compiler
           unconditionally dereferences tree.value.begin(); reject the
           truncation here instead of handing it a node to crash on. */
        if (tree.value.empty()) {
            fatal(current_file, instr_line,
                "unexpected end of file: 'ADDRESS' expects a value, got none");
        }

        return parser_state::whitespace;
    }

    parser_state label_tokenizer(std::istream &fin, token_tree &tree, char c) {
        label_state state {label_state::start};
        int instr_line {current_line};

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

        /* EOF reached (maize-37, cross-ref maize-52): flush a trailing name or
           value that was never terminated by whitespace (mirrors
           opcode_1/2/3param_tokenizer's EOF-flush pattern), then reject a
           truncated LABEL that never accumulated both a name and a value
           instead of handing label_compiler a short node to crash on. A
           1-child node here is the exact "LABEL x" (name flushed, EOF before
           any value char) crash shape; a 0-child node is "LABEL" with no name
           token flushed either. */
        if (!current_token.empty()) {
            tree.add(current_token);
            current_token.clear();
        }

        if (tree.value.size() < 2) {
            fatal(current_file, instr_line,
                "unexpected end of file: 'LABEL' expects a name and a value, got "
                + std::to_string(tree.value.size()));
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
            /* env_error, not asm_error (maize-50): a missing include is never
               recovered, or every symbol it provides cascades into a derived
               undefined-label diagnostic. */
            fatal_env(include_from_file, include_from_line,
                "cannot open include file '" + include_path.string()
                + "' (included from " + include_from_file + ":"
                + std::to_string(include_from_line) + ")");
        }

        /* Circular INCLUDE detection (maize-37): canonicalize so the same file
           reached via different relative spellings still matches the chain. A
           path already open anywhere on the chain (self-include, or a mutual
           A-INCLUDEs-B / B-INCLUDEs-A cycle) would otherwise recurse through
           tokenize() with no bound until the process stack-overflows. The
           stream is already known-open above, so canonical() resolving
           against a real file should not itself fail; fall back to the
           unresolved path on the (defensive) error_code branch rather than
           throwing out of a diagnostic path. */
        std::error_code canon_ec;
        std::filesystem::path resolved_path =
            std::filesystem::canonical(include_path, canon_ec);
        std::string resolved = canon_ec ? include_path.string() : resolved_path.string();

        if (std::find(include_chain.begin(), include_chain.end(), resolved) != include_chain.end()) {
            fatal_env(include_from_file, include_from_line,
                "circular INCLUDE of '" + resolved + "'");
        }

        include_chain.push_back(resolved);

        /* Nested-INCLUDE base path (maize-37, decision): each INCLUDE's
           relative path resolves against the directory of the file that
           contains that INCLUDE directive, not a single top-level global.
           Push/pop base_path around the recursive tokenize() call below so a
           deeper frame's own INCLUDEs see its containing directory, then
           restore the caller's base_path on the way back out. The single
           top-level frame (direct assembly, or --stdin's explicit
           --base-path) is unaffected: it never nests, so there's exactly one
           frame doing the resolving either way. */
        std::string saved_base_path {base_path};
        if (!canon_ec && resolved_path.has_parent_path()) {
            base_path = resolved_path.parent_path().string();
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
        base_path = saved_base_path;

        include_chain.pop_back();

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
                /* Malformed binary literal (maize-37): a non-0/1 character used to
                   print a bare "Error" straight to std::cerr, with no file/line, no
                   "mazm:" prefix, and no fatal()/record_diag involvement at all --
                   then return a garbage static_cast<uint64_t>(-1) sentinel that got
                   silently truncated into the compiled value. Route through fatal
                   like every other diagnostic instead. */
                fatal(current_ref_loc.file, current_ref_loc.line,
                    "malformed binary literal '" + str + "'");
            }
        }

        return tmp;
    }

    /* Group-separator strip (maize-37). ',' and '`' are digit-group separators
       (see hello.mazm's own "$0000`0000: ; the back-tick is used as a number
       separator" comment) that opcode_1/2/3param_tokenizer and parse_literal
       already discard character-by-character while accumulating an operand or
       a LABEL/ADDRESS value token. A CODE block's own header token (e.g.
       "$0000`0000:") is instead accumulated by process_char_stream, which does
       NOT discard them, so convert_label_string sees the raw separators. Strip
       them here so the new fail-bit/eof() validation below doesn't reject
       legitimate grouped literals as malformed. */
    std::string strip_group_separators(std::string const &str) {
        std::string out;
        out.reserve(str.size());

        for (char ch : str) {
            if (ch != ',' && ch != '`') {
                out.push_back(ch);
            }
        }

        return out;
    }

    maize::u_hword convert_label_string(std::string const &value) {
        maize::u_hword hvalue {0};
        char type = value.empty() ? '\0' : value[0];

        std::stringstream cvt;

        if (type == special_chars::hex) {
            cvt << std::hex << strip_group_separators(value.substr(1));
            cvt >> hvalue;

            /* Malformed literal (maize-37): stringstream extraction used to run
               unchecked -- a token like "$ZZ" silently stored whatever partial
               value operator>> left behind (often 0). Reject a failed extraction
               and reject trailing garbage that didn't parse (e.g. "$12x4"). */
            if (cvt.fail() || !cvt.eof()) {
                fatal(current_ref_loc.file, current_ref_loc.line,
                    "malformed hex literal '" + value + "'");
            }
        }
        else if (type == special_chars::dec) {
            cvt << std::dec << strip_group_separators(value.substr(1));
            cvt >> hvalue;

            if (cvt.fail() || !cvt.eof()) {
                fatal(current_ref_loc.file, current_ref_loc.line,
                    "malformed decimal literal '" + value + "'");
            }
        }
        else if (type == special_chars::bin) {
            std::string tmp = strip_group_separators(value.substr(1));
            hvalue = static_cast<u_hword>(bin_cvt(tmp));
        }
        else {
            /* Unrecognized LABEL/ADDRESS value prefix (maize-37): this used to
               silently return the max() undefined-label sentinel with no
               diagnostic at all -- a typo'd prefix (or e.g. "AUTO", see
               README.md's now-corrected LABEL syntax note) compiled clean and
               only surfaced later as a confusing "undefined label" error.
               Name the bad token directly instead. */
            fatal(current_ref_loc.file, current_ref_loc.line,
                "malformed LABEL/ADDRESS value '" + value + "'");
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

        /* Malformed literal (maize-37): see convert_label_string for the same
           unchecked-extraction issue this mirrors. Overlong literals still fall
           through to the pre-existing "Invalid literal format" throw below,
           unchanged (out of scope for this card). */
        if (cvt.fail() || !cvt.eof()) {
            fatal(current_ref_loc.file, current_ref_loc.line,
                "malformed hex literal '" + literal + "'");
        }

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

        /* Malformed literal (maize-37): a token like "12x4" used to silently
           extract "12" and drop the trailing "x4" with no diagnostic, since
           partial extraction doesn't set failbit on its own -- checking eof()
           too catches unconsumed trailing garbage. Overlong-value handling
           (the "Invalid literal format" throw below) is unchanged, out of
           scope for this card. */
        if (cvt.fail() || !cvt.eof()) {
            fatal(current_ref_loc.file, current_ref_loc.line,
                "malformed decimal literal '" + literal + "'");
        }

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

    /* Malformed literal (maize-37): validation for a non-0/1 character lives in
       bin_cvt itself (called below), which now raises the fatal diagnostic
       directly instead of printing a bare std::cerr line and returning a
       garbage sentinel. */
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
        current_ref_loc = { it->loc_file, it->loc_line };

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

    /* Forward declarations: add_label/write_label are defined later in this
       file (used by the regimm_* compilers) but address_compiler (maize-37
       fix) needs to route ADDRESS-by-label through the same fixup machinery
       they implement. */
    u_byte add_label(std::string &label, cpu::reg_value &value);
    u_hword write_label(u_hword address, std::string &label, cpu::reg_value value);

    void address_compiler(token_tree &tree, std::string &opcode_str) {
        auto &value_node = *tree.value.begin();
        auto data_string {value_node.key};
        current_ref_loc = { value_node.loc_file, value_node.loc_line };
        u_hword data = 0;

        /* ADDRESS accepts either a numeric literal or a label name (README:
           "ADDRESS address | labelName"). convert_label_string only
           understands $/#/%-prefixed numeric literals; route a label-name
           token around it entirely instead of shape-testing via is_label
           (which only reports whether the label happens to be already
           declared) so a genuinely malformed numeric literal still gets the
           new diagnostic while a bare label-name token -- forward-declared
           or not -- never reaches convert_label_string's unrecognized-prefix
           path at all.

           maize-37 code-review fix: the label-name branch used to read
           labels[data_string] into a local that was immediately discarded --
           zero bytes written, current_address never advanced, no diagnostic.
           A forward-referenced label (not yet declared at this point in the
           file) silently vanished, misaligning every subsequent address.
           Route through the SAME add_label/compile_label + write_label
           fixup machinery every other label-operand call site already uses
           (see regimm_reg_compiler etc.): compile_label reads the existing
           labels[] entry (real address, or the max() forward-reference
           placeholder) for an already-known name; add_label registers a
           fresh max() placeholder + reference-site for a name not seen yet.
           Either way, write_label reserves the correctly-sized placeholder
           immediately, advances current_address, and -- only when the value
           is still the max() sentinel -- records a fixup that the end-of-
           compile fixup pass patches once every label is known. Backward
           and forward references both resolve through this one path; no
           special-casing which came first. */
        if (is_literal(data_string)) {
            data = convert_label_string(data_string);
            current_address += cpu::mm.write_hword(current_address, data);
        }
        else {
            cpu::reg_value value {0};

            if (is_label(data_string)) {
                compile_label(data_string, value);
            }
            else {
                add_label(data_string, value);
            }

            current_address += write_label(current_address, data_string, value);
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
            current_ref_loc = { sub_tree.loc_file, sub_tree.loc_line };
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
        current_ref_loc = { label_node.loc_file, label_node.loc_line };

        /* A CODE block header is either a numeric start address ("$0000`0000:")
           or a named label ("main:") -- see hello.mazm for both forms in active
           use. convert_label_string only understands $/#/%-prefixed numeric
           literals; a bare name is the intentional signal (via the max()
           sentinel below) that this block's address is "here", resolved from
           current_address just below. Route the name case around
           convert_label_string entirely (maize-37) so a plain label like
           "main" never reaches its unrecognized-prefix diagnostic, which is
           meant for a genuinely malformed *numeric* literal. */
        auto address = is_literal(label)
            ? convert_label_string(label)
            : std::numeric_limits<u_hword>::max();

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

            /* Recovery site 4 of 5 (maize-50): each instruction in the block
               gets its own diagnostic instead of aborting the routine. */
            try {
                if (keywords.contains(key)) {
                    keywords[key].compiler(sub_tree, key);
                }
                else if (opcodes.contains(key)) {
                    opcodes[key].compiler(sub_tree, key);
                }
            }
            catch (const asm_error &e) {
                record_diag(e.what());
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

    /* Unrecognized register name (maize-37): several call sites are
       grammar-mandated to receive a register operand (not "could be a register
       or an immediate/label", which is guarded by is_register at the call
       site already) and used to call compile_register unconditionally --
       compile_register silently falls back to register 0 for a name it
       doesn't recognize, with no diagnostic. Guard those sites through here
       instead of calling compile_register directly. */
    u_byte compile_register_checked(std::string &reg, std::string const &file, int line) {
        if (!is_register(reg)) {
            fatal(file, line, "unknown register '" + reg + "'");
        }

        return compile_register(reg);
    }

    /* Width in bytes patched by a register operand's sub-register field. Drives
       the immediate width (and thus the relocation width) of a label reference
       whose destination register is that operand (maize-12). */
    size_t obj_width_from_subreg(u_byte reg_byte) {
        u_byte sub = reg_byte & cpu::opflag_subreg;
        if (sub <= cpu::opflag_subreg_b7) {
            return 1;
        }
        if (sub <= cpu::opflag_subreg_q3) {
            return 2;
        }
        if (sub <= cpu::opflag_subreg_h1) {
            return 4;
        }
        return 8;
    }

    u_byte obj_imm_size_flag(size_t width) {
        switch (width) {
            case 1:  return cpu::opflag_imm_size_08b;
            case 2:  return cpu::opflag_imm_size_16b;
            case 4:  return cpu::opflag_imm_size_32b;
            default: return cpu::opflag_imm_size_64b;
        }
    }

    /* Ensure a referenced label is present in the symbol universe. An undefined
       reference is legal in object mode: it stays a max() placeholder, which the
       .mzo writer records as an UNDEF symbol for the linker to resolve. */
    void obj_note_ref(std::string const &label) {
        if (!labels.contains(label)) {
            labels[label] = std::numeric_limits<u_hword>::max();
        }
        if (!label_ref_loc.contains(label)) {
            label_ref_loc[label] = current_ref_loc;
        }
    }

    /* Emit a width-keyed relocation for a label reference and write that many
       placeholder (zero) bytes; the linker fills them in. */
    void obj_emit_label_ref(std::string &label, size_t width) {
        obj_note_ref(label);
        obj_relocs.push_back({ current_address, label, maize::obj::reloc_for_width(width) });
        for (size_t i = 0; i < width; ++i) {
            current_address += cpu::mm.write_byte(current_address, 0);
        }
    }

    u_hword write_label(u_hword address, std::string &label, cpu::reg_value value) {
        /* Object mode: record a 32-bit (ABS32) relocation and leave placeholder
           bytes rather than resolving inline. This is the single choke point for
           the single-/three-operand label paths (CALL/JMP/PUSH/SYS, LEA, ST);
           the two-operand CP/LD path picks its own width from the destination
           sub-register before reaching here (maize-12). */
        if (emit_object) {
            obj_note_ref(label);
            obj_relocs.push_back({ current_address, label, maize::obj::R_MAIZE_ABS32 });
            return cpu::mm.write_hword(address, 0);
        }

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
           touch memory; LD reads from a memory address (source prefixed with '@'). The
           ALU/CMP/TEST ops keep accepting either form (Maize is CISC), so they are not
           constrained here. (LDZ was removed as redundant, card maize-29.) */
        if ((opcode_str == "CP" || opcode_str == "CPZ") && operand1_is_address) {
            fatal(current_ref_loc.file, current_ref_loc.line,
                opcode_str + " takes a value source (register or immediate); use LD to read from a memory address");
        }
        if (opcode_str == "LD" && !operand1_is_address) {
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

        u_byte operand2_byte {compile_register_checked(operand2, it->loc_file, it->loc_line)};

        /* Object mode (maize-12): the label's immediate width follows the
           destination sub-register (CP <label> Rn -> ABS64, CP <label> Rn.H0 ->
           ABS32, .B0 -> ABS8). Fix operand1_byte's immediate-size field to match
           before it is written, then emit a width-keyed relocation. */
        size_t obj_label_width {0};
        if (operand_is_label && emit_object) {
            obj_label_width = obj_width_from_subreg(operand2_byte);
            operand1_byte = (operand1_byte & ~cpu::opflag_imm_size)
                | obj_imm_size_flag(obj_label_width);
        }

        current_address += cpu::mm.write_byte(current_address, opcode);
        current_address += cpu::mm.write_byte(current_address, operand1_byte);
        current_address += cpu::mm.write_byte(current_address, operand2_byte);

        if (operand_is_label && emit_object) {
            obj_emit_label_ref(operand1, obj_label_width);
        }
        else if (operand_is_immediate) {
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
        u_byte operand1_byte {compile_register_checked(operand1, it->loc_file, it->loc_line)};
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

        /* Object mode (maize-12): label operands in the three-operand forms are
           not yet supported; the inline-immediate branch below would bypass
           relocation emission. Fail loudly rather than corrupt the object. The
           common LEA idioms use a register or literal offset, not a label. */
        if (emit_object && (operand1_is_label || operand2_is_label)) {
            fatal(current_ref_loc.file, current_ref_loc.line,
                opcode_str + ": label operands are not supported in object mode yet (maize-12)");
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
        operand2_byte = compile_register_checked(operand2, it->loc_file, it->loc_line);

        /* Object mode (maize-12): see the regimm_imm guard. A label source here
           would be inlined without a relocation. */
        if (emit_object && operand_is_label) {
            fatal(current_ref_loc.file, current_ref_loc.line,
                opcode_str + ": label operands are not supported in object mode yet (maize-12)");
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
        auto operand2_it {it};
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

        u_byte operand2_byte {compile_register_checked(operand2, operand2_it->loc_file, operand2_it->loc_line)};
        u_byte operand3_byte {compile_register_checked(operand3, it->loc_file, it->loc_line)};

        /* Object mode (maize-12): a resolved label source would be inlined below
           without a relocation; fail loudly. LEA's common forms use a register or
           literal source operand. */
        if (emit_object && operand_is_label) {
            fatal(current_ref_loc.file, current_ref_loc.line,
                opcode_str + ": label operands are not supported in object mode yet (maize-12)");
        }

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

    /* ---- object-mode directives (maize-12) ------------------------------- */

    void section_compiler(token_tree &tree, std::string &opcode_str) {
        if (!emit_object) {
            /* SECTION is inert in flat mode: the whole image is a single blob. */
            return;
        }
        if (tree.value.empty()) {
            fatal(current_file, token_line, "SECTION requires a kind (CODE/RODATA/DATA/BSS)");
        }
        std::string raw = tree.value.begin()->key;
        std::string up;
        std::transform(raw.begin(), raw.end(), std::back_inserter(up), ::toupper);

        u_byte kind;
        if (up == "CODE" || up == "TEXT" || up == ".TEXT") {
            kind = maize::obj::SEC_CODE;
        }
        else if (up == "RODATA" || up == ".RODATA") {
            kind = maize::obj::SEC_RODATA;
        }
        else if (up == "DATA" || up == ".DATA") {
            kind = maize::obj::SEC_DATA;
        }
        else if (up == "BSS" || up == ".BSS") {
            kind = maize::obj::SEC_BSS;
        }
        else {
            fatal(current_file, token_line, "unknown section kind '" + raw + "'");
            return;
        }

        /* Close the current span at the present address and open the new one. */
        obj_spans.push_back({ obj_cur_kind, obj_cur_start, current_address });
        obj_cur_kind = kind;
        obj_cur_start = current_address;
    }

    void global_compiler(token_tree &tree, std::string &opcode_str) {
        if (!emit_object) {
            return;
        }
        if (tree.value.empty()) {
            fatal(current_file, token_line, "GLOBAL requires a symbol name");
        }
        obj_exports.insert(tree.value.begin()->key);
    }

    void zero_compiler(token_tree &tree, std::string &opcode_str) {
        if (!emit_object) {
            return;
        }
        if (tree.value.empty()) {
            fatal(current_file, token_line, "ZERO requires a byte count");
        }
        auto &lit_node = *tree.value.begin();
        std::string lit = lit_node.key;
        current_ref_loc = { lit_node.loc_file, lit_node.loc_line };
        cpu::reg_value value {0};
        compile_literal(lit, value);
        /* Reserve space with no file bytes (NOBITS): advance the cursor without
           writing. Only meaningful inside a BSS section. */
        current_address += static_cast<maize::u_hword>(value.w0);
    }

    /* Serialize the assembled image as a relocatable .mzo object (maize-12). */
    void write_object(std::string const &path) {
        using namespace maize::obj;

        /* Close the final open span. */
        obj_spans.push_back({ obj_cur_kind, obj_cur_start, current_address });

        /* Slice the flat image into per-kind section content, and remember each
           span's section-relative base so flat addresses can be mapped to
           (kind, offset). */
        struct span_info { u_byte kind; maize::u_word start; maize::u_word end; maize::u_word base; };
        std::vector<span_info> spans;
        std::vector<u_byte> sec_data[5];
        maize::u_word sec_size[5] = {0, 0, 0, 0, 0};
        maize::u_word cum[5] = {0, 0, 0, 0, 0};

        for (auto const &sp : obj_spans) {
            if (sp.end < sp.start) {
                continue;
            }
            maize::u_word len = sp.end - sp.start;
            spans.push_back({ sp.kind, sp.start, sp.end, cum[sp.kind] });
            if (sp.kind != SEC_BSS) {
                for (maize::u_word a = sp.start; a < sp.end; ++a) {
                    sec_data[sp.kind].push_back(cpu::mm.read_byte(a));
                }
            }
            sec_size[sp.kind] += len;
            cum[sp.kind] += len;
        }

        /* Fixed section order CODE, RODATA, DATA, BSS; only present kinds emit. */
        const u_byte order[4] = { SEC_CODE, SEC_RODATA, SEC_DATA, SEC_BSS };
        int sec_index_of[5];
        for (int i = 0; i < 5; ++i) {
            sec_index_of[i] = -1;
        }
        std::vector<u_byte> present;
        for (u_byte k : order) {
            if (sec_size[k] > 0) {
                sec_index_of[k] = static_cast<int>(present.size());
                present.push_back(k);
            }
        }

        auto locate = [&](maize::u_word addr, u_byte &kind, maize::u_word &off) -> bool {
            for (auto const &s : spans) {
                if (addr >= s.start && addr < s.end) {
                    kind = s.kind;
                    off = s.base + (addr - s.start);
                    return true;
                }
            }
            /* Boundary: a label sitting exactly at a span end belongs to it. */
            for (auto const &s : spans) {
                if (s.end > s.start && addr >= s.start && addr <= s.end) {
                    kind = s.kind;
                    off = s.base + (addr - s.start);
                    return true;
                }
            }
            return false;
        };

        /* String table: index 0 is the empty string. */
        std::vector<u_byte> strtab;
        strtab.push_back(0);
        auto add_str = [&](std::string const &s) -> std::uint32_t {
            std::uint32_t off = static_cast<std::uint32_t>(strtab.size());
            for (char ch : s) {
                strtab.push_back(static_cast<u_byte>(ch));
            }
            strtab.push_back(0);
            return off;
        };

        /* Section names, in present order. */
        auto kind_name = [](u_byte k) -> std::string {
            switch (k) {
                case SEC_CODE:   return ".text";
                case SEC_RODATA: return ".rodata";
                case SEC_DATA:   return ".data";
                case SEC_BSS:    return ".bss";
                default:         return "";
            }
        };
        std::vector<std::uint32_t> sec_name_off;
        for (u_byte k : present) {
            sec_name_off.push_back(add_str(kind_name(k)));
        }

        /* Symbols, deterministically ordered by name. */
        std::vector<std::string> names;
        for (auto const &kv : labels) {
            names.push_back(kv.first);
        }
        std::sort(names.begin(), names.end());

        struct out_sym {
            std::uint32_t name_off;
            std::uint16_t section_index;
            u_byte binding;
            u_byte type;
            std::uint64_t value;
            std::uint64_t size;
        };
        std::vector<out_sym> symbols;
        std::map<std::string, std::uint32_t> sym_index;

        for (auto const &name : names) {
            maize::u_hword val = labels[name];
            out_sym s {};
            s.name_off = add_str(name);
            s.size = 0;
            if (val == std::numeric_limits<maize::u_hword>::max()) {
                s.section_index = SHN_UNDEF;
                s.binding = BIND_GLOBAL;
                s.type = TYPE_NOTYPE;
                s.value = 0;
            }
            else {
                u_byte kind;
                maize::u_word off;
                if (locate(val, kind, off) && sec_index_of[kind] >= 0) {
                    s.section_index = static_cast<std::uint16_t>(sec_index_of[kind]);
                    s.value = off;
                    s.type = (kind == SEC_CODE) ? TYPE_FUNC : TYPE_OBJECT;
                }
                else {
                    /* An absolute constant (e.g. a LABEL directive) that maps to
                       no section. */
                    s.section_index = SHN_ABS;
                    s.value = val;
                    s.type = TYPE_NOTYPE;
                }
                s.binding = obj_exports.count(name) ? BIND_GLOBAL : BIND_LOCAL;
            }
            sym_index[name] = static_cast<std::uint32_t>(symbols.size());
            symbols.push_back(s);
        }

        /* Relocations, grouped by target section. */
        struct out_reloc { std::uint64_t r_offset; std::uint32_t r_symbol; u_byte r_type; };
        std::vector<std::vector<out_reloc>> sec_relocs(present.size());
        for (auto const &rel : obj_relocs) {
            u_byte kind;
            maize::u_word off;
            if (!locate(rel.flat_off, kind, off) || sec_index_of[kind] < 0) {
                fatal(current_file, 0, "internal: relocation outside any section");
            }
            out_reloc r {};
            r.r_offset = off;
            r.r_symbol = sym_index[rel.symbol];
            r.r_type = rel.r_type;
            sec_relocs[sec_index_of[kind]].push_back(r);
        }

        /* Entry symbol. */
        std::uint32_t entry_sym = ENTRY_NONE;
        if (sym_index.count("_start")) {
            entry_sym = sym_index["_start"];
        }

        /* ---- compute file layout ---- */
        std::uint64_t off = MZO_HEADER_SIZE;
        std::uint64_t shoff = off;
        off += static_cast<std::uint64_t>(present.size()) * SECTION_HDR_SIZE;

        std::vector<std::uint64_t> sec_file_off(present.size(), 0);
        for (std::size_t i = 0; i < present.size(); ++i) {
            u_byte k = present[i];
            if (k != SEC_BSS) {
                sec_file_off[i] = off;
                off += sec_data[k].size();
            }
        }

        std::vector<std::uint64_t> sec_reloc_off(present.size(), 0);
        for (std::size_t i = 0; i < present.size(); ++i) {
            if (!sec_relocs[i].empty()) {
                sec_reloc_off[i] = off;
                off += sec_relocs[i].size() * RELOC_SIZE;
            }
        }

        std::uint64_t symoff = off;
        off += symbols.size() * SYMBOL_SIZE;
        std::uint64_t stroff = off;
        off += strtab.size();

        /* ---- serialize ---- */
        std::vector<u_byte> file;

        /* Header (48 bytes). */
        put_u8(file, MZO_MAGIC0);
        put_u8(file, MZO_MAGIC1);
        put_u8(file, MZO_MAGIC2);
        put_u8(file, MZO_VERSION);
        put_u16(file, 0);                                        /* flags */
        put_u16(file, static_cast<std::uint16_t>(present.size()));
        put_u64(file, shoff);
        put_u64(file, symoff);
        put_u32(file, static_cast<std::uint32_t>(symbols.size()));
        put_u64(file, stroff);
        put_u32(file, static_cast<std::uint32_t>(strtab.size()));
        put_u32(file, entry_sym);
        put_u32(file, 0);                                        /* reserved */

        /* Section headers (40 bytes each). */
        for (std::size_t i = 0; i < present.size(); ++i) {
            u_byte k = present[i];
            put_u32(file, sec_name_off[i]);
            put_u8(file, k);
            put_u8(file, default_attrs(k));
            put_u8(file, 1);                                     /* align */
            put_u8(file, 0);                                     /* reserved */
            put_u64(file, (k == SEC_BSS) ? 0 : sec_file_off[i]);
            put_u64(file, sec_size[k]);
            put_u64(file, sec_reloc_off[i]);
            put_u64(file, static_cast<std::uint64_t>(sec_relocs[i].size()));
        }

        /* Section contents (non-BSS). */
        for (std::size_t i = 0; i < present.size(); ++i) {
            u_byte k = present[i];
            if (k != SEC_BSS) {
                file.insert(file.end(), sec_data[k].begin(), sec_data[k].end());
            }
        }

        /* Relocation arrays. */
        for (std::size_t i = 0; i < present.size(); ++i) {
            for (auto const &r : sec_relocs[i]) {
                put_u64(file, r.r_offset);
                put_u32(file, r.r_symbol);
                put_u8(file, r.r_type);
                put_u8(file, 0);
                put_u8(file, 0);
                put_u8(file, 0);
                put_u64(file, 0);                                /* r_addend */
            }
        }

        /* Symbol table. */
        for (auto const &s : symbols) {
            put_u32(file, s.name_off);
            put_u16(file, s.section_index);
            put_u8(file, s.binding);
            put_u8(file, s.type);
            put_u64(file, s.value);
            put_u64(file, s.size);
        }

        /* String table. */
        file.insert(file.end(), strtab.begin(), strtab.end());

        std::ofstream fout(path, std::fstream::binary);
        fout.write(reinterpret_cast<const char *>(file.data()), file.size());
        fout.close();
    }

    void print_usage(std::ostream &out) {
        out <<
            "usage: mazm [options] <input.mazm>\n"
            "\n"
            "Maize assembler. Assembles <input.mazm> into a flat memory image written\n"
            "next to the input file as <input>.bin, or into a relocatable object\n"
            "<input>.mzo with -c. On assembly errors no output is produced and any\n"
            "stale output at the target path is removed.\n"
            "\n"
            "options:\n"
            "  -c, --emit-object     emit a relocatable .mzo object instead of a flat .bin\n"
            "  --check               validate only: run the full assembly pipeline with\n"
            "                        no filesystem effects (nothing written or removed)\n"
            "  --stdin               read source from standard input instead of a file;\n"
            "                        requires --check and --base-path\n"
            "  --base-path <dir>     directory INCLUDE paths resolve against\n"
            "                        (default: the input file's directory)\n"
            "  --source-name <name>  name reported in diagnostics for --stdin input\n"
            "                        (default: <stdin>)\n"
            "  -h, --help            show this help and exit\n"
            "\n"
            "Unrecognized --flags are ignored, so editor integrations can pass newer\n"
            "flags to older assemblers.\n";
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

        if (arg == "-h" || arg == "--help") {
            print_usage(std::cout);
            return 0;
        }
        else if (arg == "--check") {
            check_only = true;
        }
        else if (arg == "-c" || arg == "--emit-object") {
            emit_object = true;
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
        catch (const error_limit_reached &) {
            /* record_diag already appended the stopping line; fall through
               to the flush below. */
        }
        catch (const env_error &e) {
            flush_diags();
            std::cerr << e.what() << std::endl;
            return 1;
        }
        catch (const asm_error &e) {
            /* Safety net: the recovery sites should have caught this. */
            flush_diags();
            std::cerr << e.what() << std::endl;
            return 1;
        }
        catch (const std::exception &e) {
            flush_diags();
            std::cerr << "mazm: error: " << e.what() << std::endl;
            return 1;
        }

        if (!diags.empty()) {
            flush_diags();
            return 1;
        }

        return 0;
    }

    if (input_file.empty()) {
        /* No input and no --stdin: nothing to do. Print help and exit nonzero
           so a bare or misspelled invocation is not mistaken for success. */
        print_usage(std::cerr);
        return 1;
    }

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
    catch (const error_limit_reached &) {
        /* record_diag already appended the stopping line; fall through
           to the flush below. */
    }
    catch (const env_error &e) {
        flush_diags();
        std::cerr << e.what() << std::endl;
        return 1;
    }
    catch (const asm_error &e) {
        /* Safety net: the recovery sites should have caught this.
           Already formatted as "mazm: <file>:<line>: error: <msg>". */
        flush_diags();
        std::cerr << e.what() << std::endl;
        return 1;
    }
    catch (const std::exception &e) {
        /* Anything else (e.g. a missing input file makes std::filesystem
           throw): still exit nonzero with a mazm-prefixed message rather than
           an unhandled-exception abort. */
        flush_diags();
        std::cerr << "mazm: error: " << e.what() << std::endl;
        return 1;
    }

    if (!diags.empty()) {
        flush_diags();
        return 1;
    }

    return 0;
}
