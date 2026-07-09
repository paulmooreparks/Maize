/* maize-14: mzdis, the Maize disassembler.

   Turns a flat .mzb or a linked .mzx back into readable Maize assembly text.
   .mzo (relocatable object) input is explicitly rejected -- see the OQ3 note on
   card maize-14; object disassembly (unresolved symbols, relocations) is a
   separate, harder problem.

   Single translation unit, no link dependency on cpu.cpp/sys.cpp: only the
   `const` numeric constants in maize_cpu.h's `instr` namespace and `opflag_*`
   tables are used (never the `regs::*` extern singletons, which live in
   cpu.cpp), plus maize_obj.h's shared .mzx/.mzo layout helpers. Mirrors
   mzld.cpp's footprint (CMakeLists.txt, single-source executable target).

   Decode algorithm and opcode table are cross-referenced against:
     - src/maize_cpu.h:418-729  (the `instr` namespace, symbolic opcode bytes)
     - src/cpu.cpp's tick() dispatch (src/cpu.cpp:2045-, the actual
       byte-consumption ground truth: regs::rp.w0 += / op*_imm_size() calls)
     - README.md:1109-1369      ("Opcodes Sorted Numerically", the full 256-byte
       table with mnemonics and operand shapes)

   Usage:  mzdis [-o out_path] <input>
           default output = stdout
*/

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "maize_cpu.h"
#include "maize_obj.h"

using namespace maize;
using namespace maize::obj;
namespace instr_ns = maize::cpu::instr;

namespace {

    /* ---- opcode decode table ------------------------------------------- */

    /* Operand descriptor kinds (spec: "Decode algorithm", step 2):
         src     - SrcFlagged: the operand's actual kind (register-value /
                   immediate-value / register-address / immediate-address) is
                   read off the opcode byte's own bits 6 (imm) and 7 (addr).
         reg     - FixedReg: always a plain (reg, subreg) param byte, never
                   dereferenced, regardless of the opcode's own flag bits.
         regaddr - FixedReg, but always rendered as a dereferenced memory
                   operand ('@' prefix) -- ST/CMPIND/TSTIND's destination.
         port    - PortImm: always an immediate (imm-size selector byte +
                   that many immediate bytes), independent of the opcode's own
                   flag bits -- OUT's port operand.
         imm     - Immediate jump target: an imm-size selector byte + that many
                   immediate bytes, like `port`, but the opcode's own high bits are
                   the condition selector (not src flags) -- Jcc's target (maize-64). */
    enum class operand_kind : std::uint8_t { none, src, reg, regaddr, port, imm };

    struct opcode_entry {
        const char *mnemonic {nullptr};
        std::array<operand_kind, 3> operands {operand_kind::none, operand_kind::none, operand_kind::none};
        int operand_count {0};

        bool assigned() const { return mnemonic != nullptr; }
    };

    std::array<opcode_entry, 256> g_table {};

    void set_entry(u_byte byte, const char *mnem, std::initializer_list<operand_kind> ops) {
        opcode_entry &e = g_table[byte];
        e.mnemonic = mnem;
        e.operand_count = static_cast<int>(ops.size());
        int i = 0;
        for (auto k : ops) {
            e.operands[i++] = k;
        }
    }

    /* Two-operand ALU-style family: SrcFlagged op1, FixedReg op2. All four
       addressing-mode rows populated (regVal/immVal/regAddr/immAddr). */
    void add_alu_family(const char *mnem, u_byte base) {
        for (u_byte flag : {u_byte(0x00), u_byte(0x40), u_byte(0x80), u_byte(0xC0)}) {
            set_entry(u_byte(base | flag), mnem, {operand_kind::src, operand_kind::reg});
        }
    }

    /* Three-operand family (LEA/CMPXCHG/MULW/UMULW): SrcFlagged op1, FixedReg
       op2, FixedReg op3. All four addressing-mode rows populated. */
    void add_3reg_family(const char *mnem, u_byte base) {
        for (u_byte flag : {u_byte(0x00), u_byte(0x40), u_byte(0x80), u_byte(0xC0)}) {
            set_entry(u_byte(base | flag), mnem, {operand_kind::src, operand_kind::reg, operand_kind::reg});
        }
    }

    /* Single-operand control-transfer family (JMP/CALL): SrcFlagged op1 only. All
       four addressing-mode rows populated. */
    void add_jump_family(const char *mnem, u_byte base) {
        for (u_byte flag : {u_byte(0x00), u_byte(0x40), u_byte(0x80), u_byte(0xC0)}) {
            set_entry(u_byte(base | flag), mnem, {operand_kind::src});
        }
    }

    /* Conditional branch (Jcc), card maize-64: a single immediate target. The
       opcode byte's high bits are the condition selector (not src flags), so the
       operand is always an immediate; one flat opcode byte per condition. */
    void add_jcc(const char *mnem, u_byte byte) {
        set_entry(byte, mnem, {operand_kind::imm});
    }

    /* Two-row family, regVal/immVal only (no address forms populated): op1
       SrcFlagged, op2 always a dereferenced FixedReg -- ST/CMPIND/TSTIND. */
    void add_src_regaddr_2row(const char *mnem, u_byte base) {
        set_entry(base, mnem, {operand_kind::src, operand_kind::regaddr});
        set_entry(u_byte(base | 0x40), mnem, {operand_kind::src, operand_kind::regaddr});
    }

    /* Two-row family, regVal/immVal only: single SrcFlagged operand --
       PUSH/INT/SYS. No address forms exist for these (decision D6492: not
       every base opcode has all four rows populated). */
    void add_src_only_2row(const char *mnem, u_byte base) {
        set_entry(base, mnem, {operand_kind::src});
        set_entry(u_byte(base | 0x40), mnem, {operand_kind::src});
    }

    /* Single row, single plain-register operand -- CLR/INC/DEC/NOT/NEG/POP. */
    void add_reg_only(const char *mnem, u_byte byte) {
        set_entry(byte, mnem, {operand_kind::reg});
    }

    /* Single row, no operands -- HALT/RET/IRET/SETINT/CLRINT/SETCRY/CLRCRY/
       NOP/BRK. */
    void add_zero_operand(const char *mnem, u_byte byte) {
        set_entry(byte, mnem, {});
    }

    void build_table() {
        struct fam { const char *mnem; u_byte base; };

        for (auto &f : std::initializer_list<fam> {
            {"ADD", instr_ns::add_opcode}, {"SUB", instr_ns::sub_opcode}, {"MUL", instr_ns::mul_opcode},
            {"DIV", instr_ns::div_opcode}, {"MOD", instr_ns::mod_opcode}, {"AND", instr_ns::and_opcode},
            {"OR", instr_ns::or_opcode}, {"NOR", instr_ns::nor_opcode}, {"NAND", instr_ns::nand_opcode},
            {"XOR", instr_ns::xor_opcode}, {"SHL", instr_ns::shl_opcode}, {"SHR", instr_ns::shr_opcode},
            {"SAR", instr_ns::sar_opcode},
            {"CMP", instr_ns::cmp_opcode}, {"TEST", instr_ns::test_opcode}, {"UDIV", instr_ns::udiv_opcode},
            {"UMOD", instr_ns::umod_opcode}, {"ADC", instr_ns::adc_opcode}, {"SBB", instr_ns::sbb_opcode},
        }) {
            add_alu_family(f.mnem, f.base);
        }

        for (auto &f : std::initializer_list<fam> {
            {"CMPXCHG", instr_ns::cmpxchg_opcode}, {"LEA", instr_ns::lea_opcode},
            {"MULW", instr_ns::mulw_opcode}, {"UMULW", instr_ns::umulw_opcode},
        }) {
            add_3reg_family(f.mnem, f.base);
        }

        for (auto &f : std::initializer_list<fam> {
            {"JMP", instr_ns::jmp_opcode}, {"CALL", instr_ns::call_opcode},
        }) {
            add_jump_family(f.mnem, f.base);
        }

        /* Jcc (card maize-64): ten immediate-target conditional branches, condition
           encoded in the opcode byte's row/column bits. Freed slots ($1A/$1B/$1C/
           $37/$38/$39/$3A) and the row-11 spares ($D8/$D9) stay reserved. */
        add_jcc("JZ", instr_ns::jz_opcode);
        add_jcc("JNZ", instr_ns::jnz_opcode);
        add_jcc("JLT", instr_ns::jlt_opcode);
        add_jcc("JB", instr_ns::jb_opcode);
        add_jcc("JGT", instr_ns::jgt_opcode);
        add_jcc("JA", instr_ns::ja_opcode);
        add_jcc("JGE", instr_ns::jge_opcode);
        add_jcc("JLE", instr_ns::jle_opcode);
        add_jcc("JBE", instr_ns::jbe_opcode);
        add_jcc("JAE", instr_ns::jae_opcode);

        /* CP / LD share ld_opcode's base byte ($01): the mnemonic is CP when the
           opcode's own srcAddr bit (0x80) is clear (regVal/immVal, a value
           source), LD when it's set (regAddr/immAddr, a memory-address source).
           Both render op2 as a plain destination register. */
        set_entry(instr_ns::cp_regVal_reg, "CP", {operand_kind::src, operand_kind::reg});
        set_entry(instr_ns::cp_immVal_reg, "CP", {operand_kind::src, operand_kind::reg});
        set_entry(instr_ns::ld_regAddr_reg, "LD", {operand_kind::src, operand_kind::reg});
        set_entry(instr_ns::ld_immAddr_reg, "LD", {operand_kind::src, operand_kind::reg});

        /* CPZ: only regVal/immVal rows (the address forms, $93/$D3, were LDZ,
           removed as redundant, card maize-29; now reserved). */
        set_entry(instr_ns::cpz_regVal_reg, "CPZ", {operand_kind::src, operand_kind::reg});
        set_entry(instr_ns::cpz_immVal_reg, "CPZ", {operand_kind::src, operand_kind::reg});

        for (u_byte flag : {u_byte(0x00), u_byte(0x40), u_byte(0x80), u_byte(0xC0)}) {
            /* OUT: op2 is always an immediate port number (PortImm), independent
               of the opcode's own flag bits. */
            set_entry(u_byte(instr_ns::out_opcode | flag), "OUT", {operand_kind::src, operand_kind::port});
            /* OUTR/IN: op2 is always a plain register (the port register / the
               destination register), despite maize_cpu.h naming some of these
               constants "..._imm" -- that name is misleading (spec note,
               src/maize_cpu.h:662-672); cpu.cpp always treats op2 as a register
               (op2_reg().q0). */
            set_entry(u_byte(instr_ns::outr_opcode | flag), "OUTR", {operand_kind::src, operand_kind::reg});
            set_entry(u_byte(instr_ns::in_opcode | flag), "IN", {operand_kind::src, operand_kind::reg});
        }

        add_src_only_2row("PUSH", instr_ns::push_opcode);
        add_reg_only("CLR", instr_ns::clr_opcode);

        /* SETcc (card maize-55): ten flat single-register opcodes materializing a
           flag condition as 0/1. $EC/$ED stay reserved. */
        add_reg_only("SETZ", instr_ns::setz_opcode);
        add_reg_only("SETNZ", instr_ns::setnz_opcode);
        add_reg_only("SETLT", instr_ns::setlt_opcode);
        add_reg_only("SETGE", instr_ns::setge_opcode);
        add_reg_only("SETGT", instr_ns::setgt_opcode);
        add_reg_only("SETLE", instr_ns::setle_opcode);
        add_reg_only("SETB", instr_ns::setb_opcode);
        add_reg_only("SETAE", instr_ns::setae_opcode);
        add_reg_only("SETA", instr_ns::seta_opcode);
        add_reg_only("SETBE", instr_ns::setbe_opcode);

        /* INT ($24 regVal / $64 immVal) is assigned a mnemonic in maize_cpu.h's
           instr namespace and in README's numeric opcode table, so it gets a real
           decode entry here even though cpu.cpp's tick() has no dispatch case (its
           execution semantics await an interrupt-vector-table format, card maize-10);
           that is an execution-time gap, not an encoding gap. DUP/SWAP, which used to
           be decoded here too, were header-only ghosts and are removed (card
           maize-64); their freed bytes now disassemble as reserved. */
        add_src_only_2row("INT", instr_ns::int_opcode);
        add_reg_only("POP", instr_ns::pop_opcode);
        add_zero_operand("RET", instr_ns::ret_opcode);
        add_zero_operand("IRET", instr_ns::iret_opcode);
        add_zero_operand("SETINT", instr_ns::setint_opcode);

        add_src_regaddr_2row("ST", instr_ns::st_opcode);
        add_src_regaddr_2row("CMPIND", instr_ns::cmpind_opcode);
        add_src_regaddr_2row("TSTIND", instr_ns::testind_opcode);

        add_reg_only("INC", instr_ns::inc_opcode);
        add_reg_only("DEC", instr_ns::dec_opcode);
        add_reg_only("NOT", instr_ns::not_opcode);
        add_reg_only("NEG", instr_ns::neg_opcode);

        add_src_only_2row("SYS", instr_ns::sys_opcode);

        set_entry(instr_ns::xchg_opcode, "XCHG", {operand_kind::reg, operand_kind::reg});
        add_zero_operand("SETCRY", instr_ns::setcry_opcode);
        add_zero_operand("CLRCRY", instr_ns::clrcry_opcode);
        add_zero_operand("CLRINT", instr_ns::clrint_opcode);

        add_zero_operand("NOP", instr_ns::nop_opcode);
        add_zero_operand("BRK", instr_ns::brk_opcode);
        add_zero_operand("HALT", instr_ns::halt_opcode);
    }

    /* ---- rendering helpers ------------------------------------------------ */

    const char *REG_NAMES[16] = {
        "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
        "R8", "R9", "RT", "RV", "RF", "RB", "RP", "RS"
    };

    /* Index 14 (W0, full 64-bit width) renders with NO suffix -- README.md:387's
       "R0" == "R0.W0" convention; always emit the bare form (AC6484). Index 15
       is not an assigned subregister (README.md:849-865 only defines 0x0-0xE);
       mazm never emits it, so this is a defensive fallback, not a tested path. */
    const char *SUBREG_SUFFIX[16] = {
        ".B0", ".B1", ".B2", ".B3", ".B4", ".B5", ".B6", ".B7",
        ".Q0", ".Q1", ".Q2", ".Q3", ".H0", ".H1", "", ".?RESERVED"
    };

    std::string render_reg(u_byte b) {
        int reg_i = (b >> 4) & 0x0F;
        int sub_i = b & 0x0F;
        return std::string(REG_NAMES[reg_i]) + SUBREG_SUFFIX[sub_i];
    }

    std::string hex_u8(u_byte v) {
        std::ostringstream oss;
        oss << "$" << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned int>(v);
        return oss.str();
    }

    /* Fixed-width zero-padded hex matching the operand's declared byte width,
       with a backtick digit-group separator every 4 hex digits for widths >= 4
       bytes (spec: "Operand rendering"). `bytes` holds the operand's raw bytes
       in little-endian file order (as read from the stream); this reconstructs
       the numeric value before formatting, big-endian display order. */
    std::string render_imm(const std::vector<u_byte> &bytes) {
        std::size_t width = bytes.size();
        u_word value = 0;
        for (std::size_t i = width; i-- > 0; ) {
            value = (value << 8) | bytes[i];
        }

        std::size_t hexdigits = width * 2;
        std::ostringstream oss;
        oss << std::uppercase << std::hex << std::setw(static_cast<int>(hexdigits)) << std::setfill('0') << value;
        std::string hex = oss.str();

        std::string result = "$";
        if (width >= 4) {
            std::size_t n = hex.size();
            for (std::size_t i = 0; i < n; ++i) {
                if (i > 0 && (n - i) % 4 == 0) {
                    result += '`';
                }
                result += hex[i];
            }
        }
        else {
            result += hex;
        }
        return result;
    }

    std::string format_addr(u_word addr) {
        std::vector<u_byte> bytes(8);
        for (int i = 0; i < 8; ++i) {
            bytes[static_cast<std::size_t>(i)] = static_cast<u_byte>((addr >> (8 * i)) & 0xFF);
        }
        return render_imm(bytes);
    }

    /* ---- instruction decode ------------------------------------------- */

    enum class decode_status { ok, unknown_opcode, malformed, truncated };

    struct decode_result {
        decode_status status {decode_status::ok};
        std::size_t consumed {0};   // valid for ok/unknown_opcode/malformed
        std::string text {};        // rendered "MNEMONIC operand ..." body, for ok
        std::size_t need {0};       // for truncated: total bytes short
        u_byte bad_byte {0};        // for malformed: the offending param byte
    };

    /* Decode exactly one instruction starting at buf[seg_start + offset],
       bounded to the window [seg_start, seg_start + seg_len). `offset` is
       relative to seg_start (0-based within the window, e.g. a .mzx segment's
       own byte range or the whole flat file). */
    decode_result decode_one(const std::vector<u_byte> &buf, std::size_t seg_start, std::size_t seg_len, std::size_t offset) {
        std::size_t avail = seg_len - offset;
        u_byte op = buf[seg_start + offset];
        const opcode_entry &entry = g_table[op];

        if (!entry.assigned()) {
            decode_result r;
            r.status = decode_status::unknown_opcode;
            r.consumed = 1;
            r.text = "DB " + hex_u8(op);
            return r;
        }

        std::size_t param_count = static_cast<std::size_t>(entry.operand_count);
        if (avail < 1 + param_count) {
            decode_result r;
            r.status = decode_status::truncated;
            r.need = (1 + param_count) - avail;
            return r;
        }

        std::array<u_byte, 3> params {};
        for (std::size_t i = 0; i < param_count; ++i) {
            params[i] = buf[seg_start + offset + 1 + i];
        }

        u_byte src_flag = op & 0xC0;

        struct imm_need { std::size_t operand_index; std::size_t width; bool addr_prefix; };
        std::vector<imm_need> imm_needs;
        std::array<std::string, 3> operand_text {};
        bool malformed = false;
        u_byte bad_byte = 0;

        for (std::size_t i = 0; i < param_count && !malformed; ++i) {
            operand_kind k = entry.operands[i];
            u_byte pbyte = params[i];

            switch (k) {
                case operand_kind::reg:
                    operand_text[i] = render_reg(pbyte);
                    break;
                case operand_kind::regaddr:
                    operand_text[i] = "@" + render_reg(pbyte);
                    break;
                case operand_kind::port:
                case operand_kind::imm: {
                    if (pbyte & 0xF8) {
                        malformed = true;
                        bad_byte = pbyte;
                        break;
                    }
                    std::size_t w = std::size_t(1) << (pbyte & 0x07);
                    imm_needs.push_back({i, w, false});
                    break;
                }
                case operand_kind::src: {
                    if (src_flag == 0x00) {
                        operand_text[i] = render_reg(pbyte);
                    }
                    else if (src_flag == 0x80) {
                        operand_text[i] = "@" + render_reg(pbyte);
                    }
                    else {
                        if (pbyte & 0xF8) {
                            malformed = true;
                            bad_byte = pbyte;
                            break;
                        }
                        std::size_t w = std::size_t(1) << (pbyte & 0x07);
                        imm_needs.push_back({i, w, src_flag == 0xC0});
                    }
                    break;
                }
                default:
                    break;
            }
        }

        if (malformed) {
            decode_result r;
            r.status = decode_status::malformed;
            r.consumed = 1;
            r.bad_byte = bad_byte;
            return r;
        }

        std::size_t imm_total = 0;
        for (auto &n : imm_needs) {
            imm_total += n.width;
        }

        if (avail < 1 + param_count + imm_total) {
            decode_result r;
            r.status = decode_status::truncated;
            r.need = (1 + param_count + imm_total) - avail;
            return r;
        }

        std::size_t imm_cursor = offset + 1 + param_count;
        for (auto &n : imm_needs) {
            std::vector<u_byte> bytes(
                buf.begin() + static_cast<long>(seg_start + imm_cursor),
                buf.begin() + static_cast<long>(seg_start + imm_cursor + n.width));
            std::string rendered = render_imm(bytes);
            operand_text[n.operand_index] = (n.addr_prefix ? "@" : "") + rendered;
            imm_cursor += n.width;
        }

        std::string line = entry.mnemonic;
        for (std::size_t i = 0; i < param_count; ++i) {
            line += " ";
            line += operand_text[i];
        }

        decode_result r;
        r.status = decode_status::ok;
        r.consumed = 1 + param_count + imm_total;
        r.text = line;
        return r;
    }

    /* Linear sweep over [seg_start, seg_start + seg_len). Writes rendered
       lines to `out`. Returns true if the sweep hit a truncated tail (caller
       exits 1); writes the trailing "; TRUNCATED ..." diagnostic itself.
       `entry_addr`, when set, annotates the instruction at that address with
       "; ENTRY" (the .mzx CODE segment's recorded entry point). */
    bool decode_sweep(const std::vector<u_byte> &buf, std::size_t seg_start, std::size_t seg_len,
                       u_word base_addr, std::optional<u_word> entry_addr, std::ostream &out) {
        std::size_t offset = 0;
        while (offset < seg_len) {
            u_word addr = base_addr + offset;
            decode_result r = decode_one(buf, seg_start, seg_len, offset);

            switch (r.status) {
                case decode_status::ok: {
                    out << "    " << r.text << "    ; " << format_addr(addr);
                    if (entry_addr.has_value() && *entry_addr == addr) {
                        out << " ENTRY";
                    }
                    out << "\n";
                    offset += r.consumed;
                    break;
                }
                case decode_status::unknown_opcode: {
                    out << "    " << r.text << "    ; " << format_addr(addr) << " unknown opcode\n";
                    offset += r.consumed;
                    break;
                }
                case decode_status::malformed: {
                    out << "    ; " << format_addr(addr) << " malformed operand byte " << hex_u8(r.bad_byte)
                        << " (reserved bits set)\n";
                    offset += r.consumed;
                    break;
                }
                case decode_status::truncated: {
                    out << "    ; TRUNCATED at " << format_addr(addr) << ": expected " << r.need
                        << " more byte(s)\n";
                    return true;
                }
            }
        }
        return false;
    }

    void render_data_segment(const std::vector<u_byte> &buf, std::uint64_t file_off, std::uint64_t file_size,
                              u_word vaddr, const char *kind_label, std::ostream &out) {
        out << "; ---- " << kind_label << ": vaddr=" << format_addr(vaddr) << ", size=" << file_size << " ----\n";
        std::uint64_t i = 0;
        while (i < file_size) {
            std::uint64_t n = std::min<std::uint64_t>(16, file_size - i);
            out << "    DATA";
            for (std::uint64_t j = 0; j < n; ++j) {
                out << " " << hex_u8(buf[static_cast<std::size_t>(file_off + i + j)]);
            }
            out << "    ; " << format_addr(vaddr + i) << "\n";
            i += n;
        }
    }

    void render_bss_segment(u_word vaddr, u_word size, std::ostream &out) {
        out << "; ---- BSS: vaddr=" << format_addr(vaddr) << ", size=" << size << " ----\n";
        out << "    ZERO #" << size << "\n";
    }

    /* ---- format detection ------------------------------------------------ */

    bool looks_like_mzo(const std::vector<u_byte> &buf) {
        return buf.size() >= 3 && buf[0] == MZO_MAGIC0 && buf[1] == MZO_MAGIC1 && buf[2] == MZO_MAGIC2;
    }

    bool looks_like_mzx(const std::vector<u_byte> &buf) {
        return buf.size() >= MZX_HEADER_SIZE
            && buf[0] == MZX_MAGIC0 && buf[1] == MZX_MAGIC1 && buf[2] == MZX_MAGIC2 && buf[3] == MZX_VERSION;
    }

} // namespace

static void print_usage(std::ostream &out) {
    out <<
        "usage: mzdis [-o out_path] <input>\n"
        "\n"
        "Maize disassembler. Turns a flat .mzb memory image or a linked .mzx\n"
        "executable back into readable Maize assembly text, written to stdout\n"
        "unless -o redirects it to a file. .mzo relocatable objects are not\n"
        "supported (see maize-14 follow-on).\n"
        "\n"
        "options:\n"
        "  -o <out_path>   write disassembly to out_path instead of stdout\n"
        "  -h, --help      show this help and exit\n"
        "\n"
        "Exits 0 on a full clean decode. Exits 1 if the input cannot be opened,\n"
        "is a .mzo object, or the sweep hits a truncated instruction tail near\n"
        "end of file (any output already rendered is still written before\n"
        "returning 1).\n"
        "\n"
        "An input path is required.\n";
}

int main(int argc, char **argv) {
    std::string out_path;
    std::string in_path;
    bool have_input = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(std::cout);
            return 0;
        }
        else if (arg == "-o" && i + 1 < argc) {
            out_path = argv[++i];
        }
        else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "usage: mzdis [-o out_path] <input>" << std::endl;
            return 1;
        }
        else if (!have_input) {
            in_path = arg;
            have_input = true;
        }
        else {
            std::cerr << "usage: mzdis [-o out_path] <input>" << std::endl;
            return 1;
        }
    }

    if (!have_input) {
        print_usage(std::cerr);
        return 1;
    }

    std::ifstream fin(in_path, std::ios::binary);
    if (!fin) {
        std::cerr << "mzdis: error: cannot open '" << in_path << "'" << std::endl;
        return 1;
    }
    std::vector<u_byte> buf((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
    fin.close();

    /* .mzo is out of scope for this card (see the header comment). Reject
       before any other processing: no stdout output, no fall-through to the
       flat-decode path (which would misinterpret the .mzo header bytes as
       garbage instructions). */
    if (looks_like_mzo(buf)) {
        std::cerr << "mzdis: error: '" << in_path
                   << "' is a .mzo relocatable object; object disassembly is not supported (see maize-14 follow-on)"
                   << std::endl;
        return 1;
    }

    build_table();

    std::ostringstream out;
    bool truncated = false;

    if (looks_like_mzx(buf)) {
        std::uint16_t seg_count = get_u16(buf.data(), 6);
        std::uint64_t entry = get_u64(buf.data(), 8);
        std::uint64_t shoff = get_u64(buf.data(), 16);

        for (std::uint16_t i = 0; i < seg_count; ++i) {
            std::size_t so = static_cast<std::size_t>(shoff) + static_cast<std::size_t>(i) * SEGMENT_SIZE;
            if (so + SEGMENT_SIZE > buf.size()) {
                std::cerr << "mzdis: error: '" << in_path << "': malformed .mzx (segment table out of bounds)" << std::endl;
                return 1;
            }

            std::uint8_t kind = buf[so + 0];
            std::uint64_t vaddr = get_u64(buf.data(), so + 8);
            std::uint64_t file_off = get_u64(buf.data(), so + 16);
            std::uint64_t mem_size = get_u64(buf.data(), so + 24);
            std::uint64_t file_size = get_u64(buf.data(), so + 32);

            if (file_off + file_size > buf.size()) {
                std::cerr << "mzdis: error: '" << in_path << "': malformed .mzx (segment contents out of bounds)" << std::endl;
                return 1;
            }

            if (kind == SEC_CODE) {
                bool seg_truncated = decode_sweep(buf, static_cast<std::size_t>(file_off),
                    static_cast<std::size_t>(file_size), vaddr, entry, out);
                truncated = truncated || seg_truncated;
            }
            else if (kind == SEC_RODATA) {
                render_data_segment(buf, file_off, file_size, vaddr, "RODATA", out);
            }
            else if (kind == SEC_DATA) {
                render_data_segment(buf, file_off, file_size, vaddr, "DATA", out);
            }
            else if (kind == SEC_BSS) {
                render_bss_segment(vaddr, mem_size, out);
            }
            /* SEC_NULL or an unrecognized kind byte: skip silently. */
        }
    }
    else {
        /* No recognized magic: flat image, decoded from address 0 to EOF --
           matches maize.cpp's loader fallback exactly (maize.cpp:79-85), so
           "what mzdis disassembles" and "what maize would execute" stay the
           same file-format decision in both tools. */
        truncated = decode_sweep(buf, 0, buf.size(), 0, std::nullopt, out);
    }

    if (!out_path.empty()) {
        std::ofstream fout(out_path, std::ios::binary);
        if (!fout) {
            std::cerr << "mzdis: error: cannot write '" << out_path << "'" << std::endl;
            return 1;
        }
        fout << out.str();
    }
    else {
        std::cout << out.str();
    }

    return truncated ? 1 : 0;
}
