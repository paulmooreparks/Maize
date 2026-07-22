/* maize-330 (JIT J1): tier-1 template JIT for Bare mode.
 *
 * This file is #included INTO src/cpu.cpp inside `namespace maize { namespace cpu {`,
 * AFTER the interpreter's operand/ALU/memory helpers are defined and BEFORE tick().
 * Being part of cpu.cpp's translation unit is deliberate: the emitted code calls the
 * interpreter's own semantic helpers (alu_add, copy_regval_reg, translate, mm, ...),
 * so every compiled instruction is bit-identical to the interpreter by construction.
 * The JIT's win is structural: it removes the per-instruction MAIZE_NEXT preamble
 * (fetch + translate + dispatch + interrupt/timer/input polling + PC bookkeeping) and
 * the operand-decode plumbing, running a hot basic block as a straight sequence of
 * semantic calls chained directly to its successors.
 *
 * HOST-CPU PORTABILITY (operator directive, 2026-07-23): the JIT is architected around
 * ANY host CPU, not just x86-64. Guest semantics never live in emitted machine code;
 * they live in the portable C++ thunks below, single-sourced with the interpreter. The
 * only host-specific component is the backend, whose entire contract is six primitives:
 *
 *   1. emit a call to a C function with up to four integer-constant arguments
 *   2. emit a return to the dispatcher
 *   3. emit "skip forward if the last call returned zero" (patchable forward branch)
 *   4. emit an unconditional patchable jump (the chaining site)
 *   5. emit "store a 64-bit constant to a fixed host address" (set the guest PC)
 *   6. patch a previously emitted branch/jump target (chaining / unlinking)
 *
 * Everything else (block discovery, decode, the block table, chaining with unlink,
 * store-driven invalidation, the differential checker, the W^X arena policy) is
 * host-CPU-neutral C++. A host without a backend builds and runs normally; --jit
 * prints a note and stays interpreted. An AArch64 backend is a later card and slots
 * in behind the same six primitives.
 *
 * Scope (J1): Bare mode only; a covered subset of opcodes (data movement, integer ALU,
 * unary micro-ops, stack ops, and the direct-transfer control flow that dominates the
 * J0 block survey). Anything else ends the block and hands control back to the
 * interpreter at the exact guest PC, so semantics never depend on emitter coverage.
 * Hosted (Sv48) correctness, register pinning, and lazy flags in codegen are later
 * phases (J2, J3).
 */

/* NOTE: this file is included at namespace scope, so it must not include headers
   itself; cpu.cpp includes everything the JIT needs (<algorithm>, <cstdlib>,
   <iostream>, and windows.h / sys/mman.h) at global scope before the namespace. */

#if defined(__x86_64__) || defined(_M_X64)
#  define MAIZE_JIT_BACKEND 1
#  define MAIZE_JIT_BACKEND_NAME "x86-64"
#else
#  define MAIZE_JIT_BACKEND 0
#  define MAIZE_JIT_BACKEND_NAME "none"
#endif

namespace {

/* ---- Switches (set from the enable_* entry points at the bottom of this file).
 * jit_active / jit_active_dispatch / jit_pending_boundary / jit_step_* / jit_seam_armed
 * are DEFINED near the top of cpu.cpp (they are referenced by the MAIZE_NEXT preamble,
 * the transfer sites, and the memory write seams, all of which precede this include). */
bool jit_check_mode = false;                            /* --jit-check */
bool jit_inject_miscompile = false;                     /* MAIZE_JIT_MISCOMPILE=1: deliberately
                                                           corrupt one template so the
                                                           differential net can be proven
                                                           to catch a miscompile (AC 4) */
std::size_t jit_cache_bytes = std::size_t(64) << 20;    /* --jit-cache-mb, default 64 MiB */
u_word jit_hotness_threshold = 50;                      /* docs/design/jit.md section 2 */

/* =====================================================================================
 * Host backend: the six primitives. x86-64 implementation.
 * ===================================================================================== */

#if MAIZE_JIT_BACKEND

/* Host register numbers (ModRM/REX encoding order). Emitted code clobbers only
 * caller-saved registers of the host ABI and makes ABI-conformant calls, so a block is
 * itself callable as a plain C function `void (*)()`. */
enum : u_byte { HR_RAX = 0, HR_RCX = 1, HR_RDX = 2,
                HR_RDI = 7, HR_RSI = 6, HR_R8 = 8, HR_R9 = 9, HR_R11 = 11 };

#if defined(_WIN32)
/* Win64: args rcx, rdx, r8, r9; 32-byte shadow space. Entry rsp%16 == 8, so a 40-byte
   frame makes every in-block call site 16-aligned. */
const u_byte JIT_ARG_REG[4] = { HR_RCX, HR_RDX, HR_R8, HR_R9 };
const u_byte JIT_CALL_FRAME = 40;
#else
/* System V: args rdi, rsi, rdx, rcx. 8-byte frame restores 16-alignment at call sites. */
const u_byte JIT_ARG_REG[4] = { HR_RDI, HR_RSI, HR_RDX, HR_RCX };
const u_byte JIT_CALL_FRAME = 8;
#endif

struct jit_emitter {
    u_byte* buf = nullptr;
    u_byte* end = nullptr;
    bool overflow = false;

    void u8(u_byte v)  { if (buf >= end) { overflow = true; return; } *buf++ = v; }
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<u_byte>(v >> (i * 8))); }
    void u64(u_word v) { for (int i = 0; i < 8; ++i) u8(static_cast<u_byte>(v >> (i * 8))); }

    void mov_ri(u_byte r, u_word imm) {          /* movabs r64, imm64 */
        u8(static_cast<u_byte>(0x48 | ((r >= 8) ? 1 : 0)));
        u8(static_cast<u_byte>(0xB8 | (r & 7)));
        u64(imm);
    }
    void sub_rsp(u_byte n) { u8(0x48); u8(0x83); u8(0xEC); u8(n); }
    void add_rsp(u_byte n) { u8(0x48); u8(0x83); u8(0xC4); u8(n); }

    /* Primitive 1: call fn(argv[0..argc-1]) with integer-constant args. Nothing stays
       live in host registers across the call. Return value lands in the host's
       integer-return register. */
    void call_fn(void* fn, int argc, const u_word* argv) {
        sub_rsp(JIT_CALL_FRAME);
        for (int i = 0; i < argc; ++i) { mov_ri(JIT_ARG_REG[i], argv[i]); }
        mov_ri(HR_RAX, reinterpret_cast<u_word>(fn));
        u8(0xFF); u8(0xD0);                      /* call rax */
        add_rsp(JIT_CALL_FRAME);
    }

    /* Primitive 2: return to the dispatcher. */
    void ret() { u8(0xC3); }

    /* Primitive 3: "if the last call returned zero, skip forward". Emits test+jz with a
       rel32 placeholder; returns the patch site (to be resolved with patch_branch once
       the skip target's address is known). */
    u_byte* skip_if_zero() {
        u8(0x48); u8(0x85); u8(0xC0);            /* test rax, rax */
        u8(0x0F); u8(0x84);                      /* jz rel32 */
        u_byte* site = buf; u32(0);
        return site;
    }

    /* Primitive 4: unconditional patchable jump; returns the rel32 patch site. */
    u_byte* jump_placeholder() { u8(0xE9); u_byte* site = buf; u32(0); return site; }

    /* Primitive 5: *(u_word*)addr = imm (used only to set the guest PC). */
    void store_const(void* addr, u_word imm) {
        mov_ri(HR_R11, reinterpret_cast<u_word>(addr));
        mov_ri(HR_RAX, imm);
        u8(0x49); u8(0x89); u8(0x03);            /* mov [r11], rax */
    }
};

/* Primitive 6: retarget a previously emitted rel32 branch/jump at `site` to `dst`. */
inline void jit_patch_branch(u_byte* site, u_byte* dst) {
    std::int64_t rel = static_cast<std::int64_t>(dst - (site + 4));
    std::uint32_t r = static_cast<std::uint32_t>(static_cast<std::int32_t>(rel));
    std::memcpy(site, &r, 4);
}

#endif  /* MAIZE_JIT_BACKEND */

/* =====================================================================================
 * Code-cache arena (W^X; host-OS-specific, host-CPU-neutral)
 * ===================================================================================== */

struct code_arena {
    u_byte* base = nullptr;
    std::size_t size = 0;
    std::size_t used = 0;
    bool executable = false;

    bool init(std::size_t bytes) {
#if defined(_WIN32)
        base = static_cast<u_byte*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
        void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        base = (p == MAP_FAILED) ? nullptr : static_cast<u_byte*>(p);
#endif
        if (base == nullptr) { return false; }
        size = bytes;
        used = 0;
        executable = false;
        return true;
    }
    /* Whole-arena W<->X flip. Never writable and executable at once. The instruction-
       cache flush is a no-op on x86 but load-bearing on ARM-class hosts; keeping it
       here is part of the any-CPU discipline. */
    void set_exec(bool exec) {
        if (base == nullptr || executable == exec) { return; }
#if defined(_WIN32)
        DWORD old;
        VirtualProtect(base, size, exec ? PAGE_EXECUTE_READ : PAGE_READWRITE, &old);
        if (exec) { FlushInstructionCache(GetCurrentProcess(), base, size); }
#else
        mprotect(base, size, exec ? (PROT_READ | PROT_EXEC) : (PROT_READ | PROT_WRITE));
#  if defined(__GNUC__) || defined(__clang__)
        if (exec) { __builtin___clear_cache(reinterpret_cast<char*>(base), reinterpret_cast<char*>(base + size)); }
#  endif
#endif
        executable = exec;
    }
    u_byte* cursor() { return base + used; }
    std::size_t remaining() const { return size - used; }
};

/* =====================================================================================
 * Blocks, edges, and the block table (host-CPU-neutral)
 * ===================================================================================== */

typedef void (*jit_fn)();

struct exit_edge {
    u_byte* patch_site = nullptr;   /* rel-branch field to (re)target (backend primitive 6) */
    u_byte* stub = nullptr;         /* local "return to dispatcher" this edge targets when unchained */
    u_word  target_key = 0;         /* successor block key */
    bool    chained = false;
};

struct jit_block {
    u_word key = 0;                 /* (physical entry << 1) | privilege */
    u_word entry_va = 0;
    u_word page = 0;                /* physical page number of the entry (blocks never span pages) */
    jit_fn code = nullptr;
    u_word instr_count = 0;
    std::vector<exit_edge> edges;               /* outgoing direct edges (final size <= 2) */
    std::vector<exit_edge*> incoming;           /* predecessor edges chained INTO this block */
};

struct jit_state {
    code_arena arena;
    bool ready = false;
    std::unordered_map<u_word, jit_block*> table;
    std::unordered_map<u_word, u_word> counter;                       /* tier-up counters */
    std::unordered_map<u_word, std::vector<exit_edge*>> pending;      /* unchained edges by target */
    std::unordered_map<u_word, std::vector<u_word>> code_pages;       /* phys page -> block keys */

    std::uint64_t blocks_compiled = 0;
    std::uint64_t block_runs = 0;
    std::uint64_t flushes = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t covered_instr = 0;
    std::uint64_t check_blocks = 0;
};
jit_state g_jit;

const u_word JIT_NEVER_COMPILE = ~u_word{0};

inline u_word jit_block_key(u_word pc) {
    return (translate(pc, access_kind::fetch) << 1)
         | (privilege_flag ? u_word{1} : u_word{0});
}

/* =====================================================================================
 * Differential-check store journal. In --jit-check mode the memory write seams record
 * every (address, value, width) a covered store produces, once for the compiled run and
 * once for the interpreter's oracle run, so the two write streams can be compared
 * byte-for-byte (spec item 8: "the bytes written to memory must match").
 * ===================================================================================== */

struct jit_store_rec { u_word addr; u_word value; u_word old; u_byte width; };
std::vector<jit_store_rec> jit_journal_a;    /* compiled run */
std::vector<jit_store_rec> jit_journal_b;    /* interpreter oracle run */
std::vector<jit_store_rec>* jit_journal = nullptr;   /* active journal, or null */

/* Store seam hook. Called (guarded by jit_seam_armed, declared early in cpu.cpp) from
 * memory_module::write_byte / write_bytes / write_from with the PHYSICAL address. Does
 * the page-invalidation check always, and journals when a differential run is active.
 * `value` carries the low `n` bytes for the two value seams; the bulk seam (write_from)
 * passes journal_value=false (covered blocks never bulk-write, so the differential
 * journal never needs it). */
void jit_invalidate_page(u_word pageno);
void jit_note_store(u_word pa, u_word value, std::size_t n, bool journal_value) {
    if (jit_journal != nullptr && journal_value) {
        /* Called BEFORE the write lands, so the current bytes are the pre-store value;
           captured for the memory rollback between the compiled run and the oracle run. */
        jit_store_rec r; r.addr = pa; r.value = value; r.old = 0; r.width = static_cast<u_byte>(n);
        mm.read_into(pa, reinterpret_cast<u_byte*>(&r.old), n);
        jit_journal->push_back(r);
    }
    if (g_jit.code_pages.empty()) { return; }
    u_word first = pa >> 12;
    u_word last = (pa + (n ? n - 1 : 0)) >> 12;
    for (u_word p = first; p <= last; ++p) {
        if (g_jit.code_pages.find(p) != g_jit.code_pages.end()) { jit_invalidate_page(p); }
    }
}

/* =====================================================================================
 * Semantic thunks (host-CPU-neutral). One per covered instruction shape; each mirrors
 * the matching tick() handler body exactly, using the interpreter's own helpers, so
 * results and flags are bit-identical. None of them touch RP; the block sets RP only at
 * its exits. Descriptor bytes and immediates arrive baked as integer constants.
 * ===================================================================================== */

inline reg& jit_reg(u_byte desc) {
    reg& r = *reg_map[(desc & opflag_reg) >> 4];
    if (&r == static_cast<reg*>(&regs::rf)) { materialize_flags(); }
    return r;
}
inline subreg_enum jit_sub(u_byte desc) { return static_cast<subreg_enum>(desc & opflag_subreg); }
inline u_byte jit_subsize(u_byte desc) { return subreg_size_map[desc & opflag_subreg]; }

typedef u_word (*alu_helper_fn)(u_byte, u_byte, u_word, u_word);

void jit_alu_rr(u_word fn, u_word opk, u_word src_desc, u_word dst_desc) {
    alu_helper_fn FN = reinterpret_cast<alu_helper_fn>(fn);
    u_word src = read_subreg_signext(jit_reg(static_cast<u_byte>(src_desc)), jit_sub(static_cast<u_byte>(src_desc)));
    u_byte size = jit_subsize(static_cast<u_byte>(dst_desc));
    u_word dst = read_subreg_bits(jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)));
    u_word res = FN(static_cast<u_byte>(opk), size, src, dst);
    write_subreg_bits(jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)), res);
}
void jit_alu_rr_nw(u_word fn, u_word opk, u_word src_desc, u_word dst_desc) {
    alu_helper_fn FN = reinterpret_cast<alu_helper_fn>(fn);
    u_word src = read_subreg_signext(jit_reg(static_cast<u_byte>(src_desc)), jit_sub(static_cast<u_byte>(src_desc)));
    u_byte size = jit_subsize(static_cast<u_byte>(dst_desc));
    u_word dst = read_subreg_bits(jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)));
    (void) FN(static_cast<u_byte>(opk), size, src, dst);
}
void jit_alu_ir(u_word fn, u_word opk, u_word imm_sx, u_word dst_desc) {
    alu_helper_fn FN = reinterpret_cast<alu_helper_fn>(fn);
    u_byte size = jit_subsize(static_cast<u_byte>(dst_desc));
    u_word dst = read_subreg_bits(jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)));
    u_word res = FN(static_cast<u_byte>(opk), size, imm_sx, dst);
    write_subreg_bits(jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)), res);
}
void jit_alu_ir_nw(u_word fn, u_word opk, u_word imm_sx, u_word dst_desc) {
    alu_helper_fn FN = reinterpret_cast<alu_helper_fn>(fn);
    u_byte size = jit_subsize(static_cast<u_byte>(dst_desc));
    u_word dst = read_subreg_bits(jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)));
    (void) FN(static_cast<u_byte>(opk), size, imm_sx, dst);
}
void jit_alu_mr(u_word fn, u_word opk, u_word addr_desc, u_word dst_desc) {
    alu_helper_fn FN = reinterpret_cast<alu_helper_fn>(fn);
    u_byte size = jit_subsize(static_cast<u_byte>(dst_desc));
    copy_regaddr_reg(jit_reg(static_cast<u_byte>(addr_desc)), jit_sub(static_cast<u_byte>(addr_desc)),
                     alu.op1_reg, width0_subreg(size));
    u_word src = alu.op1_reg.w0;
    u_word dst = read_subreg_bits(jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)));
    u_word res = FN(static_cast<u_byte>(opk), size, src, dst);
    write_subreg_bits(jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)), res);
}
void jit_alu_mr_nw(u_word fn, u_word opk, u_word addr_desc, u_word dst_desc) {
    alu_helper_fn FN = reinterpret_cast<alu_helper_fn>(fn);
    u_byte size = jit_subsize(static_cast<u_byte>(dst_desc));
    copy_regaddr_reg(jit_reg(static_cast<u_byte>(addr_desc)), jit_sub(static_cast<u_byte>(addr_desc)),
                     alu.op1_reg, width0_subreg(size));
    u_word src = alu.op1_reg.w0;
    u_word dst = read_subreg_bits(jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)));
    (void) FN(static_cast<u_byte>(opk), size, src, dst);
}
void jit_alu_imr(u_word fn, u_word opk, u_word addr_lit, u_word dst_desc) {
    alu_helper_fn FN = reinterpret_cast<alu_helper_fn>(fn);
    u_byte size = jit_subsize(static_cast<u_byte>(dst_desc));
    reg tmp; tmp.w0 = 0;
    mm.read(translate(addr_lit, access_kind::load), tmp, size, 0);
    u_word dst = read_subreg_bits(jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)));
    u_word res = FN(static_cast<u_byte>(opk), size, tmp.w0, dst);
    write_subreg_bits(jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)), res);
}
void jit_alu_imr_nw(u_word fn, u_word opk, u_word addr_lit, u_word dst_desc) {
    alu_helper_fn FN = reinterpret_cast<alu_helper_fn>(fn);
    u_byte size = jit_subsize(static_cast<u_byte>(dst_desc));
    reg tmp; tmp.w0 = 0;
    mm.read(translate(addr_lit, access_kind::load), tmp, size, 0);
    u_word dst = read_subreg_bits(jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)));
    (void) FN(static_cast<u_byte>(opk), size, tmp.w0, dst);
}

void jit_cp_rr(u_word src_desc, u_word dst_desc) {
    copy_regval_reg(jit_reg(static_cast<u_byte>(src_desc)), jit_sub(static_cast<u_byte>(src_desc)),
                    jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)));
}
void jit_cpz_rr(u_word src_desc, u_word dst_desc) {
    copy_regval_reg_zext(jit_reg(static_cast<u_byte>(src_desc)), jit_sub(static_cast<u_byte>(src_desc)),
                         jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)));
}
void jit_write_imm(u_word value, u_word dst_desc) {
    write_subreg_bits(jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)), value);
}
void jit_ld_rr(u_word addr_desc, u_word dst_desc) {
    copy_regaddr_reg(jit_reg(static_cast<u_byte>(addr_desc)), jit_sub(static_cast<u_byte>(addr_desc)),
                     jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)));
}
void jit_ldz_rr(u_word addr_desc, u_word dst_desc) {
    copy_regaddr_reg_zext(jit_reg(static_cast<u_byte>(addr_desc)), jit_sub(static_cast<u_byte>(addr_desc)),
                          jit_reg(static_cast<u_byte>(dst_desc)), jit_sub(static_cast<u_byte>(dst_desc)));
}
void jit_ld_ir(u_word addr_lit, u_word dst_desc) {
    reg tmp; tmp.w0 = 0;
    subreg_enum ds = jit_sub(static_cast<u_byte>(dst_desc));
    mm.read(translate(addr_lit, access_kind::load), tmp, subreg_size_map[static_cast<size_t>(ds)], 0);
    write_subreg_bits(jit_reg(static_cast<u_byte>(dst_desc)), ds, tmp.w0);
}
void jit_ldz_ir(u_word addr_lit, u_word dst_desc) {
    reg tmp; tmp.w0 = 0;
    subreg_enum ds = jit_sub(static_cast<u_byte>(dst_desc));
    mm.read(translate(addr_lit, access_kind::load), tmp, subreg_size_map[static_cast<size_t>(ds)], 0);
    commit_reg_w0(jit_reg(static_cast<u_byte>(dst_desc)), tmp.w0);
}
void jit_st_rr(u_word src_desc, u_word addr_desc) {
    copy_regval_regaddr(jit_reg(static_cast<u_byte>(src_desc)), jit_sub(static_cast<u_byte>(src_desc)),
                        jit_reg(static_cast<u_byte>(addr_desc)), jit_sub(static_cast<u_byte>(addr_desc)));
}
void jit_st_ir(u_word imm_raw, u_word size, u_word addr_desc) {
    reg src_data; src_data.w0 = imm_raw;
    subreg_enum as = jit_sub(static_cast<u_byte>(addr_desc));
    reg& ar = jit_reg(static_cast<u_byte>(addr_desc));
    auto aoff = subreg_offset_map[static_cast<size_t>(as)];
    auto amask = subreg_mask_map[static_cast<size_t>(as)];
    u_word addr = translate((ar.w0 & static_cast<u_word>(amask)) >> aoff, access_kind::store);
    switch (size) {
        case 1: mm.write_byte(addr, src_data.b0()); break;
        case 2: mm.write_qword(addr, src_data.q0()); break;
        case 4: mm.write_hword(addr, src_data.h0()); break;
        case 8: mm.write_word(addr, src_data.w0); break;
        default: break;
    }
}
void jit_unary(u_word uop_sel, u_word desc) {
    subreg_enum s = jit_sub(static_cast<u_byte>(desc));
    copy_regval_reg(jit_reg(static_cast<u_byte>(desc)), s, alu.op2_reg, subreg_enum::w0);
    alu.set_b0(static_cast<u_byte>(uop_sel));
    alu.set_b1(jit_subsize(static_cast<u_byte>(desc)));
    alu.set_b2(jit_subsize(static_cast<u_byte>(desc)));
    run_alu();
    copy_regval_reg(alu.op2_reg, subreg_enum::w0, jit_reg(static_cast<u_byte>(desc)), s);
}
void jit_clr(u_word desc) {
    clr_reg(jit_reg(static_cast<u_byte>(desc)), jit_sub(static_cast<u_byte>(desc)));
}
void jit_setcc(u_word cond_idx, u_word desc) {
    set_reg(jit_reg(static_cast<u_byte>(desc)), jit_sub(static_cast<u_byte>(desc)),
            eval_condition(static_cast<u_byte>(cond_idx)));
}
u_word jit_eval_cond(u_word cond_idx) {
    return eval_condition(static_cast<u_byte>(cond_idx)) ? u_word{1} : u_word{0};
}
void jit_setcry(u_word on) {
    materialize_flags();
    carryout_flag = (on != 0);
}
void jit_push_reg(u_word desc) {
    u_byte src_size = jit_subsize(static_cast<u_byte>(desc));
    reg new_rs;
    new_rs.w0 = regs::rs.w0 - src_size;
    copy_regval_regaddr(jit_reg(static_cast<u_byte>(desc)), jit_sub(static_cast<u_byte>(desc)),
                        new_rs, subreg_enum::w0);
    regs::rs.w0 = new_rs.w0;
}
void jit_push_imm(u_word imm_raw, u_word size) {
    reg src_data; src_data.w0 = imm_raw;
    reg new_rs;
    new_rs.w0 = regs::rs.w0 - size;
    u_word addr = translate(new_rs.w0, access_kind::store);
    switch (size) {
        case 1: mm.write_byte(addr, src_data.b0()); break;
        case 2: mm.write_qword(addr, src_data.q0()); break;
        case 4: mm.write_hword(addr, src_data.h0()); break;
        case 8: mm.write_word(addr, src_data.w0); break;
        default: break;
    }
    regs::rs.w0 = new_rs.w0;
}
void jit_pop(u_word desc) {
    u_byte src_size = jit_subsize(static_cast<u_byte>(desc));
    copy_memval_reg(regs::rs.w0, src_size, jit_reg(static_cast<u_byte>(desc)),
                    jit_sub(static_cast<u_byte>(desc)), access_kind::load);
    regs::rs.w0 += src_size;
}
void jit_xchg(u_word d1, u_word d2) {
    copy_regval_reg(jit_reg(static_cast<u_byte>(d1)), jit_sub(static_cast<u_byte>(d1)), operand1, subreg_enum::w0);
    copy_regval_reg(jit_reg(static_cast<u_byte>(d2)), jit_sub(static_cast<u_byte>(d2)),
                    jit_reg(static_cast<u_byte>(d1)), jit_sub(static_cast<u_byte>(d1)));
    copy_regval_reg(operand1, subreg_enum::w0, jit_reg(static_cast<u_byte>(d2)), jit_sub(static_cast<u_byte>(d2)));
}
/* LEA register form: op3 <- op1 + op2 at op2's width, flag-neutral (mirrors
   LBL_lea_regVal_regreg including the pending-descriptor save/restore). */
void jit_lea_rr(u_word d1, u_word d2, u_word d3) {
    copy_regval_reg(jit_reg(static_cast<u_byte>(d1)), jit_sub(static_cast<u_byte>(d1)), alu.op1_reg, subreg_enum::w0);
    copy_regval_reg(jit_reg(static_cast<u_byte>(d2)), jit_sub(static_cast<u_byte>(d2)), alu.op2_reg, subreg_enum::w0);
    alu.set_b0(instr::add_regVal_reg);
    alu.set_b1(jit_subsize(static_cast<u_byte>(d1)));
    alu.set_b2(jit_subsize(static_cast<u_byte>(d2)));
    pending_flags_t saved_pending = pending_flags;
    u_hword saved_fl = regs::rf.h0();
    run_alu();
    pending_flags = saved_pending;
    regs::rf.set_h0(saved_fl);
    copy_regval_reg(alu.op2_reg, subreg_enum::w0, jit_reg(static_cast<u_byte>(d3)), jit_sub(static_cast<u_byte>(d3)));
}
/* LEA immediate form: imm arrives sign-extended; b1 carries the encoded imm width. */
void jit_lea_ir(u_word imm_sx, u_word imm_size_and_d2, u_word d3) {
    u_byte imm_size = static_cast<u_byte>(imm_size_and_d2 >> 8);
    u_byte d2 = static_cast<u_byte>(imm_size_and_d2 & 0xFF);
    alu.op1_reg.w0 = imm_sx;
    copy_regval_reg(jit_reg(d2), jit_sub(d2), alu.op2_reg, subreg_enum::w0);
    alu.set_b0(instr::add_immVal_reg);
    alu.set_b1(imm_size);
    alu.set_b2(jit_subsize(d2));
    pending_flags_t saved_pending = pending_flags;
    u_hword saved_fl = regs::rf.h0();
    run_alu();
    pending_flags = saved_pending;
    regs::rf.set_h0(saved_fl);
    copy_regval_reg(alu.op2_reg, subreg_enum::w0, jit_reg(d3), jit_sub(d3));
}
void jit_call_push(u_word ret_addr) {
    reg r; r.w0 = ret_addr;
    reg new_rs; new_rs.w0 = regs::rs.w0 - subreg_size_map[static_cast<size_t>(subreg_enum::w0)];
    copy_regval_regaddr(r, subreg_enum::w0, new_rs, subreg_enum::w0);
    regs::rs.w0 = new_rs.w0;
}
void jit_ret_pop() {
    u_byte src_size = subreg_size_map[static_cast<size_t>(subreg_enum::w0)];
    copy_memval_reg(regs::rs.w0, src_size, regs::rp, subreg_enum::w0, access_kind::load);
    regs::rs.w0 += src_size;
}
/* JMP through a register value: full 64-bit target into RP (indirect exit). */
void jit_jmp_regval(u_word desc) {
    copy_regval_reg_zext(jit_reg(static_cast<u_byte>(desc)), subreg_enum::w0, regs::rp, subreg_enum::w0);
}

/* Per-block-exit boundary events: advance the perf counter, the instruction-tick timer,
   and the input poll by the block's instruction count, then report whether the compiled
   chain must break (deliverable interrupt pending, or the machine stopped). Interrupt
   and timer granularity coarsens from one instruction to one block, bounded by the
   512-instruction cap (docs/design/jit.md 3.3). In differential-check mode the oracle
   interpreter run owns all of this state, so the compiled side only tallies coverage. */
u_word jit_boundary_events(u_word n) {
    g_jit.covered_instr += n;
    if (jit_check_mode) { return 1; }   /* always return to the dispatcher (no chaining) */
    if (perf_count_enabled) { perf_insn_count += n; }
    if (active_timer_ptr != nullptr && (active_timer_ptr->control_reg.w0 & 0x1) != 0) {
        timer_device& t = *active_timer_ptr;
        for (u_word i = 0; i < n; ++i) { tick_active_timer(t); }
    }
    if (active_input_ptr != nullptr) {
        u_word before = input_tick_ctr_;
        input_tick_ctr_ += n;
        if ((before >> 14) != (input_tick_ctr_ >> 14)) {   /* crossed an INPUT_TICK_MASK+1 stride */
            active_input_ptr->on_input_tick();
        }
    }
    if (!running_flag) { return 1; }
    return (interrupt_enabled_flag && irq_pending.load(std::memory_order_relaxed)) ? u_word{1} : u_word{0};
}

/* =====================================================================================
 * Compile-time decode helpers
 * ===================================================================================== */

inline u_byte jg8(u_word va) { return mm.read_byte(translate(va, access_kind::fetch)); }
inline u_word jgN(u_word va, u_byte n) {
    reg t; t.w0 = 0; mm.read(translate(va, access_kind::fetch), t, n, 0); return t.w0;
}
inline u_word jit_signext(u_word raw, u_byte size) {
    subreg_enum ss = imm_size_subreg_map[static_cast<size_t>(size)];
    if (raw & subreg_sign_bit[static_cast<int>(ss)]) { raw |= subreg_neg_bits[static_cast<int>(ss)]; }
    return raw;
}
/* Compiled code does not maintain RP per instruction, so any covered instruction that
   names RP as a plain operand (reading a stale value, or writing it as a jump) must
   stay interpreted. Slot $E is RP. RF ($C) stays covered: jit_reg() materializes on
   read exactly as the interpreter's operand accessors do, and every register write
   lands through commit_reg_w0. */
inline bool jit_desc_is_rp(u_byte desc) { return ((desc & opflag_reg) >> 4) == 0xE; }

alu_helper_fn jit_alu_helper(u_byte base) {
    switch (base) {
        case instr::add_opcode: return &alu_add;
        case instr::sub_opcode: return &alu_sub;
        case instr::cmp_opcode: return &alu_sub;
        case instr::mul_opcode: return &alu_mul;
        case instr::adc_opcode: return &alu_adc;
        case instr::sbb_opcode: return &alu_sbb;
        case instr::and_opcode: return &alu_and;
        case instr::test_opcode: return &alu_and;
        case instr::or_opcode: return &alu_or;
        case instr::nor_opcode: return &alu_nor;
        case instr::nand_opcode: return &alu_nand;
        case instr::xor_opcode: return &alu_xor;
        case instr::shl_opcode: return &alu_shl;
        case instr::shr_opcode: return &alu_shr;
        case instr::sar_opcode: return &alu_sar;
        /* DIV/MOD/UDIV/UMOD are excluded: they can raise a divide-error trap, which
           unwinds via a C++ exception that must never cross an emitted frame. They end
           the block and run interpreted. */
        default: return nullptr;
    }
}
inline bool jit_alu_is_nw(u_byte base) {
    return base == instr::cmp_opcode || base == instr::test_opcode;
}

#if MAIZE_JIT_BACKEND

/* =====================================================================================
 * Block compilation
 * ===================================================================================== */

/* Emit a direct exit to a known guest target and record its chainable edge:
   set RP; run boundary events; if the chain must break, return to the dispatcher;
   otherwise take the patchable jump (initially to a local return; later chained
   straight to the successor block). In check mode the edge is never chained. */
void jit_emit_edge(jit_emitter& e, jit_block& b, u_word target_va, u_word n) {
    e.store_const(&regs::rp.w0, target_va);
    {
        u_word args[1] = { n };
        e.call_fn(reinterpret_cast<void*>(&jit_boundary_events), 1, args);
    }
    u_byte* skip_site = e.skip_if_zero();
    e.ret();                                    /* chain break: back to the dispatcher */
    u_byte* chain_path = e.buf;
    jit_patch_branch(skip_site, chain_path);
    u_byte* jmp_site = e.jump_placeholder();
    u_byte* stub = e.buf;
    e.ret();
    jit_patch_branch(jmp_site, stub);           /* start unchained */
    exit_edge ed;
    ed.patch_site = jmp_site;
    ed.stub = stub;
    ed.target_key = jit_block_key(target_va);
    ed.chained = false;
    b.edges.push_back(ed);
}

void jit_emit_indirect_exit(jit_emitter& e, u_word n) {
    u_word args[1] = { n };
    e.call_fn(reinterpret_cast<void*>(&jit_boundary_events), 1, args);
    e.ret();
}

bool jit_last_compile_uncoverable = false;

jit_block* jit_compile(u_word entry_pc) {
    jit_last_compile_uncoverable = false;
    if (!g_jit.ready) { return nullptr; }
    if (g_jit.arena.remaining() < 64 * 1024) { return nullptr; }

    g_jit.arena.set_exec(false);
    jit_emitter e;
    e.buf = g_jit.arena.cursor();
    e.end = g_jit.arena.base + g_jit.arena.size;

    jit_block* b = new jit_block();
    b->key = jit_block_key(entry_pc);
    b->entry_va = entry_pc;
    b->page = translate(entry_pc, access_kind::fetch) >> 12;
    b->code = reinterpret_cast<jit_fn>(g_jit.arena.cursor());
    b->edges.reserve(2);

    u_word pc = entry_pc;
    u_word count = 0;
    u_word entry_page_va = entry_pc & ~static_cast<u_word>(0xFFF);
    bool done = false;

    while (!done) {
        if ((pc & ~static_cast<u_word>(0xFFF)) != entry_page_va || count >= 512) {
            jit_emit_edge(e, *b, pc, count);    /* structural split: fall through */
            break;
        }
        u_byte op = jg8(pc);
        u_byte base = op & 0x3F;
        u_byte mode = op & opcode_flag;

        /* ---- Block-ending covered transfers. ---- */
        if (op == instr::jmp_immVal) {
            u_byte immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
            u_word target = jgN(pc + 2, immsz);
            jit_emit_edge(e, *b, target, count + 1);
            count += 1; break;
        }
        if (op == instr::jmp_regVal) {
            u_byte d = jg8(pc + 1);
            if (jit_desc_is_rp(d)) { jit_emit_edge(e, *b, pc, count); break; }
            u_word args[1] = { d };
            e.call_fn(reinterpret_cast<void*>(&jit_jmp_regval), 1, args);
            jit_emit_indirect_exit(e, count + 1);
            count += 1; break;
        }
        if (op == instr::call_immVal) {
            u_byte immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
            u_word target = jgN(pc + 2, immsz);
            u_word ret_addr = pc + 2 + immsz;
            u_word args[1] = { ret_addr };
            e.call_fn(reinterpret_cast<void*>(&jit_call_push), 1, args);
            jit_emit_edge(e, *b, target, count + 1);
            count += 1; break;
        }
        if (op == instr::ret_opcode) {
            e.call_fn(reinterpret_cast<void*>(&jit_ret_pop), 0, nullptr);
            jit_emit_indirect_exit(e, count + 1);
            count += 1; break;
        }
        if (base >= instr::jcc_base && base <= instr::jcc_base + 2) {
            u_byte row = mode >> 6;
            u_byte col = static_cast<u_byte>(base - instr::jcc_base);
            u_word cond_idx = static_cast<u_word>(row * 3 + col);
            if (cond_idx > 10) {                 /* $D9: unallocated spare, stays interpreted */
                jit_emit_edge(e, *b, pc, count); break;
            }
            u_byte immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
            u_word target = jgN(pc + 2, immsz);
            u_word fall = pc + 2 + immsz;
            u_word args[1] = { cond_idx };
            e.call_fn(reinterpret_cast<void*>(&jit_eval_cond), 1, args);
            u_byte* not_taken = e.skip_if_zero();
            jit_emit_edge(e, *b, target, count + 1);        /* taken */
            jit_patch_branch(not_taken, e.buf);
            jit_emit_edge(e, *b, fall, count + 1);          /* fall-through */
            count += 1; break;
        }

        /* ---- Straight-line covered ops. ---- */
        void* thunk = nullptr;
        int nargs = 0;
        u_word a[4] = { 0, 0, 0, 0 };
        u_word len = 0;
        bool covered = false;

        alu_helper_fn afn = jit_alu_helper(base);
        bool is_row_family =
            base == 0x31 || base == 0x32 || base == 0x27 || base == 0x29 || base == 0x24
            || base == 0x22 || base == 0x33 || base == 0x39 || base == 0x3A || base == 0x15
            || base == 0x28 || (base >= instr::setcc_base && base <= instr::setcc_base + 2)
            || (base >= instr::jcc_base && base <= instr::jcc_base + 2);
        if (afn != nullptr && !is_row_family) {
            bool nw = jit_alu_is_nw(base);
            if (mode == opcode_flag_srcReg) {
                u_byte d1 = jg8(pc + 1), d2 = jg8(pc + 2);
                if (!jit_desc_is_rp(d1) && !jit_desc_is_rp(d2)) {
                    thunk = nw ? reinterpret_cast<void*>(&jit_alu_rr_nw) : reinterpret_cast<void*>(&jit_alu_rr);
                    a[0] = reinterpret_cast<u_word>(afn); a[1] = base; a[2] = d1; a[3] = d2;
                    nargs = 4; len = 3; covered = true;
                }
            } else if (mode == opcode_flag_srcImm) {
                u_byte immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
                u_byte d2 = jg8(pc + 2);
                if (!jit_desc_is_rp(d2)) {
                    thunk = nw ? reinterpret_cast<void*>(&jit_alu_ir_nw) : reinterpret_cast<void*>(&jit_alu_ir);
                    a[0] = reinterpret_cast<u_word>(afn); a[1] = base;
                    a[2] = jit_signext(jgN(pc + 3, immsz), immsz); a[3] = d2;
                    nargs = 4; len = 3u + immsz; covered = true;
                }
            } else if (mode == opcode_flag_srcAddr) {
                u_byte d1 = jg8(pc + 1), d2 = jg8(pc + 2);
                if (!jit_desc_is_rp(d1) && !jit_desc_is_rp(d2)) {
                    thunk = nw ? reinterpret_cast<void*>(&jit_alu_mr_nw) : reinterpret_cast<void*>(&jit_alu_mr);
                    a[0] = reinterpret_cast<u_word>(afn); a[1] = base; a[2] = d1; a[3] = d2;
                    nargs = 4; len = 3; covered = true;
                }
            } else {
                u_byte immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
                u_byte d2 = jg8(pc + 2);
                if (!jit_desc_is_rp(d2)) {
                    thunk = nw ? reinterpret_cast<void*>(&jit_alu_imr_nw) : reinterpret_cast<void*>(&jit_alu_imr);
                    a[0] = reinterpret_cast<u_word>(afn); a[1] = base;
                    a[2] = jgN(pc + 3, immsz); a[3] = d2;
                    nargs = 4; len = 3u + immsz; covered = true;
                }
            }
        } else if (op == instr::cp_regVal_reg || op == instr::cpz_regVal_reg) {
            u_byte d1 = jg8(pc + 1), d2 = jg8(pc + 2);
            if (!jit_desc_is_rp(d1) && !jit_desc_is_rp(d2)) {
                thunk = (op == instr::cp_regVal_reg) ? reinterpret_cast<void*>(&jit_cp_rr)
                                                     : reinterpret_cast<void*>(&jit_cpz_rr);
                a[0] = d1; a[1] = d2; nargs = 2; len = 3; covered = true;
            }
        } else if (op == instr::cp_immVal_reg || op == instr::cpz_immVal_reg) {
            u_byte immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
            u_byte d2 = jg8(pc + 2);
            if (!jit_desc_is_rp(d2)) {
                u_word raw = jgN(pc + 3, immsz);
                thunk = reinterpret_cast<void*>(&jit_write_imm);
                a[0] = (op == instr::cp_immVal_reg) ? jit_signext(raw, immsz) : raw;
                if (jit_inject_miscompile) { a[0] += 1; }   /* AC-4 net proof: off-by-one */
                a[1] = d2; nargs = 2; len = 3u + immsz; covered = true;
            }
        } else if (op == instr::ld_regAddr_reg || op == instr::ldz_regAddr_reg) {
            u_byte d1 = jg8(pc + 1), d2 = jg8(pc + 2);
            if (!jit_desc_is_rp(d1) && !jit_desc_is_rp(d2)) {
                thunk = (op == instr::ld_regAddr_reg) ? reinterpret_cast<void*>(&jit_ld_rr)
                                                      : reinterpret_cast<void*>(&jit_ldz_rr);
                a[0] = d1; a[1] = d2; nargs = 2; len = 3; covered = true;
            }
        } else if (op == instr::ld_immAddr_reg || op == instr::ldz_immAddr_reg) {
            u_byte immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
            u_byte d2 = jg8(pc + 2);
            if (!jit_desc_is_rp(d2)) {
                thunk = (op == instr::ld_immAddr_reg) ? reinterpret_cast<void*>(&jit_ld_ir)
                                                      : reinterpret_cast<void*>(&jit_ldz_ir);
                a[0] = jgN(pc + 3, immsz); a[1] = d2; nargs = 2; len = 3u + immsz; covered = true;
            }
        } else if (op == instr::st_regVal_regAddr) {
            u_byte d1 = jg8(pc + 1), d2 = jg8(pc + 2);
            if (!jit_desc_is_rp(d1) && !jit_desc_is_rp(d2)) {
                thunk = reinterpret_cast<void*>(&jit_st_rr);
                a[0] = d1; a[1] = d2; nargs = 2; len = 3; covered = true;
            }
        } else if (op == instr::st_immVal_regAddr) {
            u_byte immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
            u_byte d2 = jg8(pc + 2);
            if (!jit_desc_is_rp(d2)) {
                thunk = reinterpret_cast<void*>(&jit_st_ir);
                a[0] = jgN(pc + 3, immsz); a[1] = immsz; a[2] = d2;
                nargs = 3; len = 3u + immsz; covered = true;
            }
        } else if (op == instr::inc_opcode || op == instr::dec_opcode
                   || op == instr::not_opcode || op == instr::neg_opcode) {
            static const u_byte uop_sel[4] = { alu_uop_inc, alu_uop_dec, alu_uop_not, alu_uop_neg };
            u_byte d1 = jg8(pc + 1);
            if (!jit_desc_is_rp(d1)) {
                thunk = reinterpret_cast<void*>(&jit_unary);
                a[0] = uop_sel[mode >> 6]; a[1] = d1; nargs = 2; len = 2; covered = true;
            }
        } else if (op == instr::clr_opcode) {
            u_byte d1 = jg8(pc + 1);
            if (!jit_desc_is_rp(d1)) {
                thunk = reinterpret_cast<void*>(&jit_clr);
                a[0] = d1; nargs = 1; len = 2; covered = true;
            }
        } else if (base >= instr::setcc_base && base <= instr::setcc_base + 2) {
            u_byte row = mode >> 6;
            u_byte col = static_cast<u_byte>(base - instr::setcc_base);
            u_word cond_idx = static_cast<u_word>(row * 3 + col);
            u_byte d1 = jg8(pc + 1);
            if (cond_idx <= 10 && !jit_desc_is_rp(d1)) {
                thunk = reinterpret_cast<void*>(&jit_setcc);
                a[0] = cond_idx; a[1] = d1; nargs = 2; len = 2; covered = true;
            }
        } else if (op == instr::setcry_opcode || op == instr::clrcry_opcode) {
            thunk = reinterpret_cast<void*>(&jit_setcry);
            a[0] = (op == instr::setcry_opcode) ? 1 : 0; nargs = 1; len = 1; covered = true;
        } else if (op == instr::nop_opcode) {
            thunk = nullptr; nargs = 0; len = 1; covered = true;
        } else if (op == instr::push_regVal) {
            u_byte d1 = jg8(pc + 1);
            if (!jit_desc_is_rp(d1)) {
                thunk = reinterpret_cast<void*>(&jit_push_reg);
                a[0] = d1; nargs = 1; len = 2; covered = true;
            }
        } else if (op == instr::push_immVal) {
            u_byte immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
            thunk = reinterpret_cast<void*>(&jit_push_imm);
            a[0] = jgN(pc + 2, immsz); a[1] = immsz; nargs = 2; len = 2u + immsz; covered = true;
        } else if (op == instr::pop_opcode) {
            u_byte d1 = jg8(pc + 1);
            if (!jit_desc_is_rp(d1)) {
                thunk = reinterpret_cast<void*>(&jit_pop);
                a[0] = d1; nargs = 1; len = 2; covered = true;
            }
        } else if (op == instr::xchg_opcode) {
            u_byte d1 = jg8(pc + 1), d2 = jg8(pc + 2);
            if (!jit_desc_is_rp(d1) && !jit_desc_is_rp(d2)) {
                thunk = reinterpret_cast<void*>(&jit_xchg);
                a[0] = d1; a[1] = d2; nargs = 2; len = 3; covered = true;
            }
        } else if (op == instr::lea_regVal_regreg) {
            u_byte d1 = jg8(pc + 1), d2 = jg8(pc + 2), d3 = jg8(pc + 3);
            if (!jit_desc_is_rp(d1) && !jit_desc_is_rp(d2) && !jit_desc_is_rp(d3)) {
                thunk = reinterpret_cast<void*>(&jit_lea_rr);
                a[0] = d1; a[1] = d2; a[2] = d3; nargs = 3; len = 4; covered = true;
            }
        } else if (op == instr::lea_immVal_regreg) {
            u_byte immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
            u_byte d2 = jg8(pc + 2), d3 = jg8(pc + 3);
            if (!jit_desc_is_rp(d2) && !jit_desc_is_rp(d3)) {
                thunk = reinterpret_cast<void*>(&jit_lea_ir);
                a[0] = jit_signext(jgN(pc + 4, immsz), immsz);
                a[1] = (static_cast<u_word>(immsz) << 8) | d2; a[2] = d3;
                nargs = 3; len = 4u + immsz; covered = true;
            }
        }

        if (!covered) {
            /* Uncovered: end the block; the interpreter executes from here. */
            jit_emit_edge(e, *b, pc, count);
            break;
        }
        if (thunk != nullptr) { e.call_fn(thunk, nargs, a); }
        pc += len;
        count += 1;
        if (e.overflow) { break; }
    }

    if (e.overflow || count == 0) {
        /* Nothing runnable was produced (first instruction uncovered, or out of space).
           Registering a zero-instruction block would spin the dispatcher forever. */
        g_jit.arena.set_exec(true);
        delete b;
        jit_last_compile_uncoverable = !e.overflow;
        return nullptr;
    }

    b->instr_count = count;
    g_jit.arena.used += static_cast<std::size_t>(e.buf - reinterpret_cast<u_byte*>(b->code));

    g_jit.table[b->key] = b;
    g_jit.code_pages[b->page].push_back(b->key);
    g_jit.blocks_compiled += 1;

    /* Chaining (skipped wholesale in check mode: every check run must be exactly one
       block so the oracle window matches). */
    if (!jit_check_mode) {
        for (auto& ed : b->edges) {
            auto it = g_jit.table.find(ed.target_key);
            if (it != g_jit.table.end() && it->second != b) {
                jit_patch_branch(ed.patch_site, reinterpret_cast<u_byte*>(it->second->code));
                ed.chained = true;
                it->second->incoming.push_back(&ed);
            } else if (it != g_jit.table.end() && it->second == b) {
                /* Self-loop: chain to own entry. */
                jit_patch_branch(ed.patch_site, reinterpret_cast<u_byte*>(b->code));
                ed.chained = true;
                b->incoming.push_back(&ed);
            } else {
                g_jit.pending[ed.target_key].push_back(&ed);
            }
        }
        auto pit = g_jit.pending.find(b->key);
        if (pit != g_jit.pending.end()) {
            for (exit_edge* ed : pit->second) {
                jit_patch_branch(ed->patch_site, reinterpret_cast<u_byte*>(b->code));
                ed->chained = true;
                b->incoming.push_back(ed);
            }
            g_jit.pending.erase(pit);
        }
    }
    g_jit.arena.set_exec(true);
    return b;
}

#endif  /* MAIZE_JIT_BACKEND */

/* Unlink and drop every compiled block on physical page `pageno`. Restores each
   incoming chained jump to its local stub FIRST (docs/design/jit.md 3.2a), so no stale
   chained jump survives the block's removal. */
void jit_invalidate_page(u_word pageno) {
#if MAIZE_JIT_BACKEND
    auto it = g_jit.code_pages.find(pageno);
    if (it == g_jit.code_pages.end()) { return; }
    std::vector<u_word> keys = it->second;      /* copy: table mutation below */
    g_jit.code_pages.erase(it);

    g_jit.arena.set_exec(false);
    for (u_word key : keys) {
        auto bit = g_jit.table.find(key);
        if (bit == g_jit.table.end()) { continue; }
        jit_block* b = bit->second;
        for (exit_edge* ed : b->incoming) {
            jit_patch_branch(ed->patch_site, ed->stub);
            ed->chained = false;
            g_jit.pending[ed->target_key].push_back(ed);
        }
        for (auto& ed : b->edges) {
            if (ed.chained) {
                auto tit = g_jit.table.find(ed.target_key);
                if (tit != g_jit.table.end()) {
                    auto& inc = tit->second->incoming;
                    inc.erase(std::remove(inc.begin(), inc.end(), &ed), inc.end());
                }
            } else {
                auto pp = g_jit.pending.find(ed.target_key);
                if (pp != g_jit.pending.end()) {
                    pp->second.erase(std::remove(pp->second.begin(), pp->second.end(), &ed), pp->second.end());
                    if (pp->second.empty()) { g_jit.pending.erase(pp); }
                }
            }
        }
        g_jit.table.erase(bit);
        g_jit.counter.erase(key);
        delete b;
    }
    g_jit.arena.set_exec(true);
    g_jit.invalidations += 1;
#else
    (void) pageno;
#endif
}

/* Whole-cache flush: the sole eviction policy (docs/design/jit.md section 5). */
void jit_flush_all() {
    g_jit.arena.set_exec(false);
    for (auto& kv : g_jit.table) { delete kv.second; }
    g_jit.table.clear();
    g_jit.counter.clear();
    g_jit.pending.clear();
    g_jit.code_pages.clear();
    g_jit.arena.used = 0;
    g_jit.arena.set_exec(true);
    g_jit.flushes += 1;
}

/* =====================================================================================
 * Differential checking (--jit-check)
 * ===================================================================================== */

struct arch_snapshot {
    u_word r[16];
    u_word fcsr;
    pending_flags_t pf;
};
void jit_snapshot(arch_snapshot& s) {
    for (int i = 0; i < 16; ++i) { s.r[i] = reg_map[i]->w0; }
    s.fcsr = regs::fcsr.w0;
    s.pf = pending_flags;
}
void jit_restore(const arch_snapshot& s) {
    for (int i = 0; i < 16; ++i) { reg_map[i]->w0 = s.r[i]; }
    regs::fcsr.w0 = s.fcsr;
    pending_flags = s.pf;
}

static const char* const jit_reg_names[16] = {
    "R0","R1","R2","R3","R4","R5","R6","R7","R8","R9","RT","RV","RF","RB","RP","RS"
};

bool jit_diff_check(u_word block_pc, u_word n,
                    const arch_snapshot& jit_end, const arch_snapshot& int_end) {
    bool ok = true;
    /* RF bits that flap asynchronously with DEVICE timing, not instruction flow: the
       interrupt-set mirror (raised by devices; the oracle ticks devices per instruction
       while a compiled block coarsens raises to its boundary, a sanctioned granularity
       difference per docs/design/jit.md 3.3) and the running bit (cross-thread
       power_off). Everything else in RF, including the real enable/privilege bits and
       all condition flags, stays strictly compared. */
    const u_word rf_async_mask = bit_interrupt_set | bit_running;
    for (int i = 0; i < 16; ++i) {
        u_word a = jit_end.r[i], bv = int_end.r[i];
        if (i == 12) { a &= ~rf_async_mask; bv &= ~rf_async_mask; }
        if (a != bv) {
            std::cerr << "maize: JIT MISCOMPILE block 0x" << std::hex << block_pc
                      << " (" << std::dec << n << " instrs) " << jit_reg_names[i]
                      << ": jit=0x" << std::hex << jit_end.r[i]
                      << " interp=0x" << int_end.r[i] << std::dec << std::endl;
            ok = false;
        }
    }
    if (jit_end.fcsr != int_end.fcsr) {
        std::cerr << "maize: JIT MISCOMPILE block 0x" << std::hex << block_pc
                  << " FCSR: jit=0x" << jit_end.fcsr << " interp=0x" << int_end.fcsr
                  << std::dec << std::endl;
        ok = false;
    }
    if (jit_journal_a.size() != jit_journal_b.size()) {
        std::cerr << "maize: JIT MISCOMPILE block 0x" << std::hex << block_pc << std::dec
                  << ": store count jit=" << jit_journal_a.size()
                  << " interp=" << jit_journal_b.size() << std::endl;
        ok = false;
    } else {
        for (std::size_t i = 0; i < jit_journal_a.size(); ++i) {
            const jit_store_rec& x = jit_journal_a[i];
            const jit_store_rec& y = jit_journal_b[i];
            if (x.addr != y.addr || x.width != y.width
                || (x.value << (64 - 8 * x.width)) != (y.value << (64 - 8 * y.width))) {
                std::cerr << "maize: JIT MISCOMPILE block 0x" << std::hex << block_pc
                          << " store #" << std::dec << i
                          << ": jit [0x" << std::hex << x.addr << "]=" << x.value
                          << "/" << std::dec << int(x.width)
                          << " interp [0x" << std::hex << y.addr << "]=" << y.value
                          << "/" << std::dec << int(y.width) << std::endl;
                ok = false;
            }
        }
    }
    return ok;
}

/* =====================================================================================
 * Dispatch: entered from tick()'s MAIZE_NEXT at block boundaries (jit_pending_boundary).
 * ===================================================================================== */

void jit_dispatch() {
#if MAIZE_JIT_BACKEND
    for (;;) {
        if (!running_flag) { return; }
        if (interrupt_enabled_flag && irq_pending.load(std::memory_order_relaxed)) { return; }
        if ((control_regs[0].w0 & 0xF) == 1) { return; }    /* Sv48 active: J2 territory */

        u_word pc = regs::rp.w0;
        u_word key = jit_block_key(pc);
        auto it = g_jit.table.find(key);
        jit_block* b = (it != g_jit.table.end()) ? it->second : nullptr;

        if (b == nullptr) {
            u_word& c = g_jit.counter[key];
            if (c == JIT_NEVER_COMPILE) { return; }
            if (++c < jit_hotness_threshold) { return; }
            if (g_jit.arena.remaining() < 64 * 1024) { jit_flush_all(); }
            b = jit_compile(pc);
            if (b == nullptr) {
                if (jit_last_compile_uncoverable) { g_jit.counter[key] = JIT_NEVER_COMPILE; }
                return;
            }
        }
        u_word block_len = b->instr_count;   /* b can be invalidated by its own stores */

        if (jit_check_mode) {
            /* Differential: run compiled; capture end state and store stream; rewind
               the registers; run the interpreter over the same N instructions as the
               oracle; compare. The oracle's state is canonical afterward. Memory is NOT
               rewound between the runs: the oracle re-executes every store, so a wrong
               compiled store surfaces in the journal comparison at the first diverging
               write, before any contaminated read can mask it. */
            g_jit.check_blocks += 1;
            arch_snapshot pre; jit_snapshot(pre);
            jit_journal_a.clear();
            jit_journal = &jit_journal_a;
            g_jit.block_runs += 1;
            b->code();
            jit_journal = nullptr;
            materialize_flags();
            arch_snapshot jit_end; jit_snapshot(jit_end);
            u_word n = block_len;

            /* Roll back the compiled run's memory effects (reverse order, journal-
               captured pre-store bytes) so the oracle sees the identical pre-state; a
               block that loads back through its own store address would otherwise read
               the compiled run's value. Seam disarmed during the rollback so it neither
               journals nor re-invalidates. */
            jit_restore(pre);
            {
                bool armed = jit_seam_armed;
                jit_seam_armed = false;
                for (std::size_t i = jit_journal_a.size(); i-- > 0; ) {
                    const jit_store_rec& r = jit_journal_a[i];
                    mm.write_from(r.addr, reinterpret_cast<const u_byte*>(&r.old), r.width);
                }
                jit_seam_armed = armed;
            }
            jit_journal_b.clear();
            jit_journal = &jit_journal_b;
            jit_step_budget = n;
            jit_step_on = true;
            bool saved_dispatch = jit_active_dispatch;
            jit_active_dispatch = false;    /* no recursive dispatch while stepping */
            tick();
            jit_active_dispatch = saved_dispatch;
            jit_step_on = false;
            jit_journal = nullptr;
            materialize_flags();
            arch_snapshot int_end; jit_snapshot(int_end);

            if (!jit_diff_check(pc, n, jit_end, int_end)) {
                std::cerr << "maize: aborting on JIT miscompile (--jit-check)" << std::endl;
                /* A hard nonzero exit, not power_off(): a miscompile must fail any
                   harness running under --jit-check. atexit handlers (terminal restore)
                   still run. */
                std::exit(2);
            }
            if (!running_flag) { return; }
            continue;
        }

        (void) block_len;
        g_jit.block_runs += 1;
        b->code();
        if (!running_flag) { return; }
    }
#endif
}

}   /* anonymous namespace */

/* =====================================================================================
 * Public entry points (declared in maize_cpu.h). Included into cpu.cpp at namespace
 * maize::cpu scope, so these have external linkage.
 * ===================================================================================== */

void enable_jit(bool check_mode) {
#if MAIZE_JIT_BACKEND
    if (!g_jit.ready) {
        if (!g_jit.arena.init(jit_cache_bytes)) {
            std::cerr << "maize: could not allocate the JIT code cache ("
                      << (jit_cache_bytes >> 20) << " MiB); running interpreted" << std::endl;
            return;
        }
        g_jit.ready = true;
    }
    jit_check_mode = check_mode;
    /* Covered-fraction reporting needs the total executed-instruction count. */
    perf_count_enabled = true;
#if defined(_WIN32)
    if (GetEnvironmentVariableA("MAIZE_JIT_MISCOMPILE", nullptr, 0) > 0) { jit_inject_miscompile = true; }
#else
    if (std::getenv("MAIZE_JIT_MISCOMPILE") != nullptr) { jit_inject_miscompile = true; }
#endif
    jit_seam_armed = true;
    jit_active_dispatch = true;
    jit_pending_boundary = true;
    jit_active = true;
#else
    (void) check_mode;
    std::cerr << "maize: no JIT backend for this host CPU (" << MAIZE_JIT_BACKEND_NAME
              << "); running interpreted" << std::endl;
#endif
}

void set_jit_cache_bytes(u_word bytes) {
    if (bytes >= (u_word{1} << 20) && !g_jit.ready) { jit_cache_bytes = static_cast<std::size_t>(bytes); }
}

void set_jit_threshold(u_word t) {
    if (t >= 1) { jit_hotness_threshold = t; }
}

void jit_report(std::ostream& out) {
    if (!jit_active) { return; }
    out << "maize: jit report\n";
    out << "  blocks compiled     : " << g_jit.blocks_compiled << "\n";
    out << "  block executions    : " << g_jit.block_runs << "\n";
    out << "  covered instructions: " << g_jit.covered_instr << "\n";
    out << "  page invalidations  : " << g_jit.invalidations << "\n";
    out << "  cache flushes       : " << g_jit.flushes << "\n";
    if (perf_count_enabled && perf_insn_count > 0) {
        out << "  covered fraction    : "
            << (g_jit.covered_instr * 1000 / perf_insn_count) / 10.0
            << "% of executed instructions\n";
    }
    if (g_jit.check_blocks > 0) {
        out << "  differential checks : " << g_jit.check_blocks << " block executions verified\n";
    }
}
