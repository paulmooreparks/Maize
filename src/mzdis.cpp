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
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
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

    /* Single row, two plain-register operands -- the row-packed register-only FP
       ops (FSQRT/FNEG/FABS/FMIN/FMAX and the FCVT* family), card maize-122. */
    void add_reg_reg(const char *mnem, u_byte byte) {
        set_entry(byte, mnem, {operand_kind::reg, operand_kind::reg});
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
        add_zero_operand("SETSYSG", instr_ns::setsysg_opcode);
        add_zero_operand("CLRSYSG", instr_ns::clrsysg_opcode);

        add_zero_operand("NOP", instr_ns::nop_opcode);
        add_zero_operand("BRK", instr_ns::brk_opcode);
        add_zero_operand("HALT", instr_ns::halt_opcode);

        /* Floating-point ISA (card maize-122). Arithmetic + FCMP take the four
           addressing-mode source rows (SrcFlagged op1, FixedReg op2); FMA takes
           the three-operand regreg shape; the register-only families are one flat
           opcode byte per operation (the row bits are baked into the constant), so
           each decodes as two plain register operands. Reserved rows in the
           row-packed slots stay unassigned and disassemble as reserved. */
        add_alu_family("FADD", instr_ns::fadd_opcode);
        add_alu_family("FSUB", instr_ns::fsub_opcode);
        add_alu_family("FMUL", instr_ns::fmul_opcode);
        add_alu_family("FDIV", instr_ns::fdiv_opcode);
        add_alu_family("FCMP", instr_ns::fcmp_opcode);
        add_3reg_family("FMADD", instr_ns::fmadd_opcode);
        add_3reg_family("FMSUB", instr_ns::fmsub_opcode);
        add_reg_reg("FSQRT", instr_ns::fsqrt_opcode);
        add_reg_reg("FNEG", instr_ns::fneg_opcode);
        add_reg_reg("FABS", instr_ns::fabs_opcode);
        add_reg_reg("FMIN", instr_ns::fmin_opcode);
        add_reg_reg("FMAX", instr_ns::fmax_opcode);
        add_reg_reg("FCVTFF", instr_ns::fcvtff_opcode);
        add_reg_reg("FCVTFS", instr_ns::fcvtfs_opcode);
        add_reg_reg("FCVTFU", instr_ns::fcvtfu_opcode);
        add_reg_reg("FCVTSF", instr_ns::fcvtsf_opcode);
        add_reg_reg("FCVTUF", instr_ns::fcvtuf_opcode);
        add_reg_only("FGETCSR", instr_ns::fgetcsr_opcode);
        add_reg_only("FSETCSR", instr_ns::fsetcsr_opcode);
        add_jcc("JP", instr_ns::jp_opcode);
        add_reg_only("SETP", instr_ns::setp_opcode);
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

    /* Reconstructs the numeric value of a little-endian (file-order) byte
       sequence -- shared by render_imm (display) and the control-flow-target
       collection pass (maize-70), which needs the raw address, not text. */
    u_word bytes_to_value(const std::vector<u_byte> &bytes) {
        u_word value = 0;
        for (std::size_t i = bytes.size(); i-- > 0; ) {
            value = (value << 8) | bytes[i];
        }
        return value;
    }

    /* Fixed-width zero-padded hex matching the operand's declared byte width,
       with a backtick digit-group separator every 4 hex digits for widths >= 4
       bytes (spec: "Operand rendering"). `bytes` holds the operand's raw bytes
       in little-endian file order (as read from the stream); this reconstructs
       the numeric value before formatting, big-endian display order. */
    std::string render_imm(const std::vector<u_byte> &bytes) {
        std::size_t width = bytes.size();
        u_word value = bytes_to_value(bytes);

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

    /* maize-70: flat-image addresses are 32-bit (`u_hword`, mazm.cpp:108 --
       current_address's type for the flat/no-SECTION assembly path), so the
       symbolic listing's origin label and per-line address comments render at
       32-bit width (8 hex digits, one backtick group) to match mazm's own
       origin-label form exactly (e.g. hello.mazm:4's "$0000`0000:"), not the
       64-bit / 16-hex-digit form format_addr uses for .mzx vaddrs. */
    std::string format_addr32(u_word addr) {
        std::vector<u_byte> bytes(4);
        for (int i = 0; i < 4; ++i) {
            bytes[static_cast<std::size_t>(i)] = static_cast<u_byte>((addr >> (8 * i)) & 0xFF);
        }
        return render_imm(bytes);
    }

    /* Synthesized-label identifier suffix: lowercase hex, zero-padded to a
       minimum of 4 digits, no separators (spec: "Label / symbol synthesis
       rules" naming convention) -- a plain identifier mazm's tokenizer accepts
       unambiguously, distinct from the backtick-grouped $-prefixed address
       forms used in comments. */
    std::string label_hex(u_word addr) {
        std::ostringstream oss;
        oss << std::hex << addr;
        std::string s = oss.str();
        while (s.size() < 4) {
            s = "0" + s;
        }
        return s;
    }

    /* ---- instruction decode ------------------------------------------- */

    enum class decode_status { ok, unknown_opcode, malformed, truncated };

    struct decode_result {
        decode_status status {decode_status::ok};
        std::size_t consumed {0};   // valid for ok/unknown_opcode/malformed
        std::string text {};        // rendered "MNEMONIC operand ..." body (literal operands), for ok; "DATA $XX" for unknown_opcode
        std::size_t need {0};       // for truncated: total bytes short
        u_byte bad_byte {0};        // for malformed: the offending param byte

        /* maize-70: structured fields for the flat-image symbolic renderer's
           two-pass label synthesis. Populated alongside `text` for ok status;
           unused by the legacy (.mzx / pre-existing) rendering path, which
           reads `text` directly and stays byte-for-byte unchanged. */
        std::string mnemonic {};                     // for ok
        std::size_t param_count {0};                 // for ok
        std::array<std::string, 3> operand_texts {};  // for ok: literal-rendered per-operand text
        int cf_operand_index {-1};                   // for ok: index into operand_texts of a symbolizable control-flow target, else -1
        u_word cf_target {0};                         // valid iff cf_operand_index >= 0: the target address
        std::size_t cf_width {0};                     // valid iff cf_operand_index >= 0: observed immediate width in bytes
        char cf_role {0};                             // 'f' = CALL (fn_), 'l' = JMP/Jcc (loc_), 0 = not a candidate
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
            /* D-DATA (maize-70): DB is not a mazm keyword (mazm.cpp:284-296),
               so a DB line cannot be reassembled and would break round-trip.
               DATA $XX reassembles to the same byte. */
            r.text = "DATA " + hex_u8(op);
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

        /* maize-70: `is_cf` / `role` mark an immediate that is a candidate for
           symbolic-label rendering by the flat-image two-pass sweep -- a
           direct (non-`@`-indirect) CALL/JMP target, or any Jcc target (always
           immediate). `role` follows the spec's naming rule: 'f' for CALL
           (fn_), 'l' for JMP/Jcc (loc_). The legacy rendering path (.mzx /
           format_addr-based decode_sweep) ignores these fields entirely. */
        struct imm_need { std::size_t operand_index; std::size_t width; bool addr_prefix; bool is_cf; char role; };
        std::vector<imm_need> imm_needs;
        std::array<std::string, 3> operand_text {};
        bool malformed = false;
        u_byte bad_byte = 0;
        bool is_call = (std::string(entry.mnemonic) == "CALL");
        bool is_jump = (std::string(entry.mnemonic) == "JMP");

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
                case operand_kind::port: {
                    if (pbyte & 0xF8) {
                        malformed = true;
                        bad_byte = pbyte;
                        break;
                    }
                    std::size_t w = std::size_t(1) << (pbyte & 0x07);
                    imm_needs.push_back({i, w, false, false, 0});
                    break;
                }
                case operand_kind::imm: {
                    /* Jcc: always a direct immediate target (maize-64), never
                       indirect -- always a loc_ candidate. */
                    if (pbyte & 0xF8) {
                        malformed = true;
                        bad_byte = pbyte;
                        break;
                    }
                    std::size_t w = std::size_t(1) << (pbyte & 0x07);
                    imm_needs.push_back({i, w, false, true, 'l'});
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
                        bool addr_prefix = (src_flag == 0xC0);
                        /* '@' indirect forms (immAddr) are memory-indirect, not
                           code labels -- never symbolized (spec). Only the
                           direct (immVal) CALL/JMP operand is a candidate. */
                        bool is_cf = !addr_prefix && (is_call || is_jump);
                        char role = is_cf ? (is_call ? 'f' : 'l') : 0;
                        imm_needs.push_back({i, w, addr_prefix, is_cf, role});
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

        decode_result r;
        r.status = decode_status::ok;

        std::size_t imm_cursor = offset + 1 + param_count;
        for (auto &n : imm_needs) {
            std::vector<u_byte> bytes(
                buf.begin() + static_cast<long>(seg_start + imm_cursor),
                buf.begin() + static_cast<long>(seg_start + imm_cursor + n.width));
            std::string rendered = render_imm(bytes);
            operand_text[n.operand_index] = (n.addr_prefix ? "@" : "") + rendered;
            if (n.is_cf) {
                r.cf_operand_index = static_cast<int>(n.operand_index);
                r.cf_target = bytes_to_value(bytes);
                r.cf_width = n.width;
                r.cf_role = n.role;
            }
            imm_cursor += n.width;
        }

        std::string line = entry.mnemonic;
        for (std::size_t i = 0; i < param_count; ++i) {
            line += " ";
            line += operand_text[i];
        }

        r.consumed = 1 + param_count + imm_total;
        r.text = line;
        r.mnemonic = entry.mnemonic;
        r.param_count = param_count;
        r.operand_texts = operand_text;
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

    /* maize-70: the flat-`.mzb` symbolic disassembly path (spec: "Listing
       format" / "Label / symbol synthesis rules"). Two passes over the whole
       file, decoded from address 0 (flat images have no vaddr/base offset):

         Pass 1 (collect): linear-decode every item, recording its start
         address as a synchronization boundary and, for `ok` items, any
         control-flow-target candidate the item carries.

         Label map: a candidate becomes a synthesized label iff its observed
         width is exactly 4 bytes, its target is in-image, and the target
         coincides with a recorded item boundary (D-WIDTH; never mid-
         instruction / mid-DATA-line). CALL (fn_) wins over JMP/Jcc (loc_) on
         a shared address (deterministic role tie-break, spec).

         Pass 2 (render): the comment-only SYMBOLS index, the single origin
         label at address 0, then each item -- a label declaration line where
         one was synthesized, followed by the instruction/DATA line with the
         qualifying operand rendered symbolically (all others stay literal,
         `render_imm`-rendered exactly as before) and the byte/address
         trailing comment (documentation only, never load-bearing -- mazm
         strips comments on reassembly). */
    constexpr std::size_t COMMENT_COL = 40;

    bool decode_sweep_symbolic(const std::vector<u_byte> &buf, std::ostream &out) {
        struct sweep_item {
            u_word addr {0};
            decode_result result {};
        };

        std::size_t seg_len = buf.size();
        std::vector<sweep_item> items;
        std::unordered_set<std::uint64_t> boundaries;
        bool truncated = false;

        std::size_t offset = 0;
        while (offset < seg_len) {
            u_word addr = static_cast<u_word>(offset);
            decode_result r = decode_one(buf, 0, seg_len, offset);

            if (r.status == decode_status::truncated) {
                sweep_item it;
                it.addr = addr;
                it.result = r;
                items.push_back(it);
                truncated = true;
                break;
            }

            sweep_item it;
            it.addr = addr;
            it.result = r;
            items.push_back(it);
            boundaries.insert(static_cast<std::uint64_t>(addr));
            offset += r.consumed;
        }

        /* Build the label map: address -> role ('f' wins over 'l'), then
           address -> synthesized name. */
        std::map<u_word, char> roles;
        for (auto &it : items) {
            if (it.result.status != decode_status::ok) {
                continue;
            }
            if (it.result.cf_operand_index < 0) {
                continue;
            }
            if (it.result.cf_width != 4) {
                continue;
            }
            u_word target = it.result.cf_target;
            if (target >= static_cast<u_word>(seg_len)) {
                continue;
            }
            if (boundaries.find(static_cast<std::uint64_t>(target)) == boundaries.end()) {
                continue;
            }
            auto existing = roles.find(target);
            if (existing == roles.end() || existing->second != 'f') {
                roles[target] = it.result.cf_role;
            }
        }

        std::map<u_word, std::string> labels;
        for (auto &kv : roles) {
            std::string prefix = (kv.second == 'f') ? "fn_" : "loc_";
            labels[kv.first] = prefix + label_hex(kv.first);
        }

        /* Pass 2: render. */
        if (!labels.empty()) {
            out << "; SYMBOLS:\n";
            for (auto &kv : labels) {
                out << ";   " << kv.second << "  " << format_addr32(kv.first) << "\n";
            }
            out << "\n";
        }

        out << format_addr32(0) << ":\n";

        for (auto &it : items) {
            if (it.result.status == decode_status::truncated) {
                out << "    ; TRUNCATED at " << format_addr32(it.addr) << ": expected " << it.result.need
                    << " more byte(s)\n";
                continue;
            }

            auto lit = labels.find(it.addr);
            if (lit != labels.end()) {
                out << lit->second << ":\n";
            }

            if (it.result.status == decode_status::malformed) {
                out << "    ; " << format_addr32(it.addr) << " malformed operand byte " << hex_u8(it.result.bad_byte)
                    << " (reserved bits set)\n";
                continue;
            }

            /* Raw bytes this item occupies, for the trailing comment. */
            std::string bytes_str;
            for (std::size_t i = 0; i < it.result.consumed; ++i) {
                if (i > 0) {
                    bytes_str += " ";
                }
                bytes_str += hex_u8(buf[static_cast<std::size_t>(it.addr) + i]);
            }

            std::string line;
            if (it.result.status == decode_status::ok) {
                line = it.result.mnemonic;
                for (std::size_t i = 0; i < it.result.param_count; ++i) {
                    line += " ";
                    if (static_cast<int>(i) == it.result.cf_operand_index) {
                        auto tgt_lit = labels.find(it.result.cf_target);
                        if (tgt_lit != labels.end() && it.result.cf_width == 4) {
                            line += tgt_lit->second;
                            continue;
                        }
                    }
                    line += it.result.operand_texts[i];
                }
            }
            else {
                /* unknown_opcode: r.text already holds "DATA $XX". */
                line = it.result.text;
            }

            std::string body = "    " + line;
            if (body.size() < COMMENT_COL) {
                body += std::string(COMMENT_COL - body.size(), ' ');
            }
            else {
                body += " ";
            }

            out << body << "; " << format_addr32(it.addr) << ": " << bytes_str;
            if (it.result.status == decode_status::unknown_opcode) {
                out << " unknown opcode";
            }
            out << "\n";
        }

        return truncated;
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
        "mzdis - disassemble a Maize program back to assembly\n"
        "\n"
        "Usage:\n"
        "  mzdis <program>            disassemble a Maize program (.mzb or .mzx)\n"
        "  mzdis -o <file> <program>  write the disassembly to a file\n"
        "  mzdis -h, --help           show this help\n"
        "\n"
        "Example:\n"
        "  mzdis hello.mzb\n"
        "\n"
        "For a flat .mzb program, the listing reassembles through mazm back to\n"
        "the exact original file, byte for byte. Addresses that other\n"
        "instructions call or branch to get a short synthesized label (fn_ for\n"
        "call targets, loc_ for branch targets) shown both in a comment-only\n"
        "\"; SYMBOLS:\" index at the top and as the operand at each reference\n"
        "(e.g. \"CALL fn_0053\" instead of a raw address), so the listing reads\n"
        "like a normal program and still edits safely: references follow their\n"
        "label if you move code around. Every line also carries a trailing\n"
        "comment with its address and raw bytes for reference; comments are\n"
        "just documentation and are ignored on reassembly. Bytes that aren't\n"
        "part of any recognized instruction show up as \"DATA $XX\" so they\n"
        "still reassemble along with everything else.\n"
        "\n"
        "Disassembling .mzo object files is not supported yet.\n";
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
           same file-format decision in both tools. maize-70: the flat path
           renders through the two-pass symbolic sweep (sparse synthesized
           labels + symbolic operands); .mzx's decode_sweep call above is
           untouched (out of scope, see spec). */
        truncated = decode_sweep_symbolic(buf, out);
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
