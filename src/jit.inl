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
bool jit_no_probe = false;                              /* MAIZE_JIT_NOPROBE=1: bisect knob */
/* Block-exit events go through the jit_boundary_events helper by default. The fully
 * inline emission (jit_emit_events_inline) is a further micro-opt that measured
 * negligible on doom_bench and currently has a Windows-only fault under investigation,
 * so it stays behind this knob (MAIZE_JIT_INLINE_EVENTS=1) rather than on the hot path. */
bool jit_no_inline_events = true;
bool jit_inject_miscompile = false;                     /* MAIZE_JIT_MISCOMPILE=1: deliberately
                                                           corrupt one template so the
                                                           differential net can be proven
                                                           to catch a miscompile (AC 4) */
std::size_t jit_cache_bytes = std::size_t(16) << 20;    /* --jit-cache-mb, default 16 MiB
                                                           (whole-cache flush on overflow;
                                                           DOOM fits in ~1 MiB, so this is
                                                           headroom, not a working set) */
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

/* ---- Anchor-relative addressing (the "merged J1+J3" inline tier, decision 9801).
 * One host register (r11) holds a fixed anchor (&regs::r0.w0) for the whole block;
 * every piece of VM state a template touches is a [r11+disp32] operand. The anchor is
 * re-established at block entry and after every thunk call (r11 is caller-saved in
 * both host ABIs). If any needed state falls outside disp32 range of the anchor
 * (never in practice; all of it is module globals), the inline tier disables itself
 * and the thunk tier carries on alone. */
u_byte* jit_anchor = nullptr;
bool jit_anchor_ok = false;
inline std::int64_t jit_disp64(const void* p) {
    return static_cast<const u_byte*>(p) - jit_anchor;
}
inline bool jit_disp_fits(const void* p) {
    std::int64_t d = jit_disp64(p);
    return d >= INT32_MIN && d <= INT32_MAX;
}
inline std::int32_t jit_disp(const void* p) { return static_cast<std::int32_t>(jit_disp64(p)); }

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

    /* ---- REX / ModRM plumbing for the inline templates. Base register is always
       r11 (the anchor) for VM state, or an explicit base for guest-memory access. */
    void rex(bool w, u_byte reg, bool x, bool b) {
        u8(static_cast<u_byte>(0x40 | (w ? 8 : 0) | ((reg >= 8) ? 4 : 0) | (x ? 2 : 0) | (b ? 1 : 0)));
    }
    void modrm_r11(u_byte reg, std::int32_t disp) {          /* [r11 + disp32] */
        u8(static_cast<u_byte>(0x80 | ((reg & 7) << 3) | 0x03));
        u32(static_cast<std::uint32_t>(disp));
    }
    void modrm_r11_sib(u_byte reg, u_byte idx, u_byte scale_log, std::int32_t disp) {
        u8(static_cast<u_byte>(0x80 | ((reg & 7) << 3) | 0x04));   /* [r11 + idx*s + disp32] */
        u8(static_cast<u_byte>((scale_log << 6) | ((idx & 7) << 3) | 0x03));
        u32(static_cast<std::uint32_t>(disp));
    }
    void modrm_base_idx(u_byte reg, u_byte base, u_byte idx) {  /* [base + idx], no disp */
        u8(static_cast<u_byte>(((reg & 7) << 3) | 0x04));
        u8(static_cast<u_byte>(((idx & 7) << 3) | (base & 7)));
    }

    /* Load from [r11+disp] into r64, by width, zero- or sign-extending. */
    void load_anchor(u_byte reg, std::int32_t disp, u_byte size, bool sx) {
        switch (size) {
            case 1: rex(true, reg, false, true); u8(0x0F); u8(sx ? 0xBE : 0xB6); modrm_r11(reg, disp); break;
            case 2: rex(true, reg, false, true); u8(0x0F); u8(sx ? 0xBF : 0xB7); modrm_r11(reg, disp); break;
            case 4:
                if (sx) { rex(true, reg, false, true); u8(0x63); modrm_r11(reg, disp); }
                else    { rex(false, reg, false, true); u8(0x8B); modrm_r11(reg, disp); }
                break;
            default: rex(true, reg, false, true); u8(0x8B); modrm_r11(reg, disp); break;
        }
    }
    /* Partial store of r64's low `size` bytes to [r11+disp] (the whole subregister
       merge: on a little-endian host a subregister IS `size` bytes at a byte offset). */
    void store_anchor(u_byte reg, std::int32_t disp, u_byte size) {
        switch (size) {
            case 1: rex(false, reg, false, true); u8(0x88); modrm_r11(reg, disp); break;
            case 2: u8(0x66); rex(false, reg, false, true); u8(0x89); modrm_r11(reg, disp); break;
            case 4: rex(false, reg, false, true); u8(0x89); modrm_r11(reg, disp); break;
            default: rex(true, reg, false, true); u8(0x89); modrm_r11(reg, disp); break;
        }
    }
    /* mov qword [r11+disp], imm32 (sign-extended). */
    void store_imm32_anchor(std::int32_t disp, std::int32_t imm) {
        rex(true, 0, false, true); u8(0xC7); modrm_r11(0, disp); u32(static_cast<std::uint32_t>(imm));
    }
    /* mov dword [r11+disp], imm32 (32-bit store; used for the pending-flags header). */
    void store_imm32_dword_anchor(std::int32_t disp, std::uint32_t imm) {
        rex(false, 0, false, true); u8(0xC7); modrm_r11(0, disp); u32(imm);
    }
    /* Load r64 from [r11 + idx*8 + disp32] (the L1 arrays). */
    void load_anchor_idx8(u_byte reg, u_byte idx, std::int32_t disp) {
        rex(true, reg, idx >= 8, true); u8(0x8B); modrm_r11_sib(reg, idx, 3, disp);
    }
    /* cmp byte [r11 + idx*1 + disp32], imm8 (the code-page bitmap probe). */
    void cmp8_anchor_idx1(u_byte idx, std::int32_t disp, u_byte imm) {
        rex(false, 0, idx >= 8, true); u8(0x80); modrm_r11_sib(7, idx, 0, disp); u8(imm);
    }
    /* cmp qword [r11+disp], imm8 (the journal-pointer probe in check flavor). */
    void cmp64_anchor_imm8(std::int32_t disp, u_byte imm) {
        rex(true, 0, false, true); u8(0x83); modrm_r11(7, disp); u8(imm);
    }
    /* cmp reg, qword [r11+disp32] (the fast-page key compare, maize-341). */
    void cmp_anchor(u_byte reg, std::int32_t disp) {
        rex(true, reg, false, true); u8(0x3B); modrm_r11(reg, disp);
    }
    /* cmp reg, [r11 + idx*8 + disp32] (the indirect-probe key compare). */
    void cmp_anchor_idx8(u_byte reg, u_byte idx, std::int32_t disp) {
        rex(true, reg, idx >= 8, true); u8(0x3B); modrm_r11_sib(reg, idx, 3, disp);
    }
    /* jmp rax (the indirect-probe hit path). */
    void jmp_rax() { u8(0xFF); u8(0xE0); }
    /* add qword [r11+disp], imm32. */
    void add_mem_imm32(std::int32_t disp, std::int32_t imm) {
        rex(true, 0, false, true); u8(0x81); modrm_r11(0, disp); u32(static_cast<std::uint32_t>(imm));
    }

    /* Register-register forms (all 64-bit unless noted). */
    void rr_op(u_byte opc, u_byte reg, u_byte rm) {          /* generic REX.W op /r */
        rex(true, reg, false, rm >= 8); u8(opc);
        u8(static_cast<u_byte>(0xC0 | ((reg & 7) << 3) | (rm & 7)));
    }
    void mov_rr(u_byte dst, u_byte src)  { rr_op(0x8B, dst, src); }
    void add_rr(u_byte dst, u_byte src)  { rr_op(0x03, dst, src); }
    void sub_rr(u_byte dst, u_byte src)  { rr_op(0x2B, dst, src); }
    void and_rr(u_byte dst, u_byte src)  { rr_op(0x23, dst, src); }
    void or_rr(u_byte dst, u_byte src)   { rr_op(0x0B, dst, src); }
    void xor_rr(u_byte dst, u_byte src)  { rr_op(0x33, dst, src); }
    void imul_rr(u_byte dst, u_byte src) {
        rex(true, dst, false, src >= 8); u8(0x0F); u8(0xAF);
        u8(static_cast<u_byte>(0xC0 | ((dst & 7) << 3) | (src & 7)));
    }
    void not_r(u_byte r) { rex(true, 2, false, r >= 8); u8(0xF7); u8(static_cast<u_byte>(0xC0 | (2 << 3) | (r & 7))); }
    void shift_ri(u_byte kind, u_byte r, u_byte n) {          /* kind: 4=shl 5=shr 7=sar */
        rex(true, kind, false, r >= 8); u8(0xC1); u8(static_cast<u_byte>(0xC0 | (kind << 3) | (r & 7))); u8(n);
    }
    void and_ri32(u_byte r, std::int32_t imm) {
        rex(true, 4, false, r >= 8); u8(0x81); u8(static_cast<u_byte>(0xC0 | (4 << 3) | (r & 7)));
        u32(static_cast<std::uint32_t>(imm));
    }
    void and_ri32_32(u_byte r, std::uint32_t imm) {           /* 32-bit and (zero-extends) */
        rex(false, 4, false, r >= 8); u8(0x81); u8(static_cast<u_byte>(0xC0 | (4 << 3) | (r & 7)));
        u32(imm);
    }
    void cmp_rr(u_byte a, u_byte b) { rr_op(0x3B, a, b); }
    void cmp_ri32_32(u_byte r, std::uint32_t imm) {           /* 32-bit cmp reg, imm32 */
        rex(false, 7, false, r >= 8); u8(0x81); u8(static_cast<u_byte>(0xC0 | (7 << 3) | (r & 7)));
        u32(imm);
    }
    void test_rr(u_byte a, u_byte b) { rr_op(0x85, b, a); }   /* test rm(a), reg(b) */
    void mov_r32_r32(u_byte dst, u_byte src) {                /* zero-extends to 64 */
        rex(false, dst, false, src >= 8); u8(0x8B);
        u8(static_cast<u_byte>(0xC0 | ((dst & 7) << 3) | (src & 7)));
    }
    void movzx_rr(u_byte dst, u_byte src, u_byte size) {      /* size 1/2: movzx; 4: mov r32 */
        if (size == 4) { mov_r32_r32(dst, src); return; }
        rex(true, dst, false, src >= 8); u8(0x0F); u8(size == 1 ? 0xB6 : 0xB7);
        u8(static_cast<u_byte>(0xC0 | ((dst & 7) << 3) | (src & 7)));
    }
    void movsx_load(u_byte reg, std::int32_t disp, u_byte size) { load_anchor(reg, disp, size, true); }
    /* lea dst, [a + b] */
    void lea_sum(u_byte dst, u_byte a, u_byte b) {
        rex(true, dst, b >= 8, a >= 8); u8(0x8D);
        u8(static_cast<u_byte>(((dst & 7) << 3) | 0x04));
        u8(static_cast<u_byte>(((b & 7) << 3) | (a & 7)));
    }
    /* lea dst, [a + disp8] */
    void lea_disp8(u_byte dst, u_byte a, std::int8_t d) {
        rex(true, dst, false, a >= 8); u8(0x8D);
        u8(static_cast<u_byte>(0x40 | ((dst & 7) << 3) | (a & 7)));
        u8(static_cast<u_byte>(d));
    }
    /* Guest-memory access through a resolved block pointer: [base + idx]. */
    void load_mem(u_byte reg, u_byte base, u_byte idx, u_byte size, bool sx) {
        switch (size) {
            case 1: rex(true, reg, idx >= 8, base >= 8); u8(0x0F); u8(sx ? 0xBE : 0xB6); modrm_base_idx(reg, base, idx); break;
            case 2: rex(true, reg, idx >= 8, base >= 8); u8(0x0F); u8(sx ? 0xBF : 0xB7); modrm_base_idx(reg, base, idx); break;
            case 4:
                if (sx) { rex(true, reg, idx >= 8, base >= 8); u8(0x63); modrm_base_idx(reg, base, idx); }
                else    { rex(false, reg, idx >= 8, base >= 8); u8(0x8B); modrm_base_idx(reg, base, idx); }
                break;
            default: rex(true, reg, idx >= 8, base >= 8); u8(0x8B); modrm_base_idx(reg, base, idx); break;
        }
    }
    void store_mem(u_byte reg, u_byte base, u_byte idx, u_byte size) {
        switch (size) {
            case 1: rex(false, reg, idx >= 8, base >= 8); u8(0x88); modrm_base_idx(reg, base, idx); break;
            case 2: u8(0x66); rex(false, reg, idx >= 8, base >= 8); u8(0x89); modrm_base_idx(reg, base, idx); break;
            case 4: rex(false, reg, idx >= 8, base >= 8); u8(0x89); modrm_base_idx(reg, base, idx); break;
            default: rex(true, reg, idx >= 8, base >= 8); u8(0x89); modrm_base_idx(reg, base, idx); break;
        }
    }
    /* Conditional jump with a rel32 placeholder; cc is the x86 condition code
       (0x4 = jz/je, 0x5 = jnz, 0x7 = ja, ...). Returns the patch site. */
    u_byte* jcc_placeholder(u_byte cc) {
        u8(0x0F); u8(static_cast<u_byte>(0x80 | cc));
        u_byte* site = buf; u32(0);
        return site;
    }
    /* Width-sized reg-reg ALU op (sets host flags at exactly the operation width).
       opc64 is the 16/32/64-bit opcode (e.g. 0x3B cmp, 0x2B sub, 0x03 add, 0x23 and,
       0x85 test); the 8-bit form is opc64 - 1 except TEST (0x84). */
    void alu_rr_w(u_byte opc64, u_byte w, u_byte reg, u_byte rm) {
        u_byte opc8 = (opc64 == 0x85) ? 0x84 : static_cast<u_byte>(opc64 - 1);
        if (w == 2) { u8(0x66); }
        rex(w == 8, reg, false, rm >= 8);
        u8(w == 1 ? opc8 : opc64);
        u8(static_cast<u_byte>(0xC0 | ((reg & 7) << 3) | (rm & 7)));
    }
    /* Width-sized add/sub reg, imm8 (INC/DEC fusion). ext: 0 = add, 5 = sub. */
    void addsub_ri8_w(u_byte ext, u_byte w, u_byte r, u_byte imm) {
        if (w == 2) { u8(0x66); }
        rex(w == 8, ext, false, r >= 8);
        u8(w == 1 ? 0x80 : 0x83);
        u8(static_cast<u_byte>(0xC0 | (ext << 3) | (r & 7)));
        u8(imm);
    }
    /* setcc r8 (low byte). cc is the x86 condition code. */
    void setcc_r(u_byte cc, u_byte r) {
        rex(false, 0, false, r >= 8);
        u8(0x0F); u8(static_cast<u_byte>(0x90 | cc));
        u8(static_cast<u_byte>(0xC0 | (r & 7)));
    }
    /* lea dst, [base + idx*scale] (flag-free accumulate). */
    void lea_scaled(u_byte dst, u_byte base_r, u_byte idx, u_byte scale_log) {
        rex(true, dst, idx >= 8, base_r >= 8); u8(0x8D);
        u8(static_cast<u_byte>(((dst & 7) << 3) | 0x04));
        u8(static_cast<u_byte>((scale_log << 6) | ((idx & 7) << 3) | (base_r & 7)));
    }
    /* mov byte [r11+disp], imm8. */
    void store_imm8_anchor(std::int32_t disp, u_byte imm) {
        rex(false, 0, false, true); u8(0xC6); modrm_r11(0, disp); u8(imm);
    }
    /* test byte [reg], imm8. */
    void test_m8_imm(u_byte base_r, u_byte imm) {
        rex(false, 0, false, base_r >= 8); u8(0xF6);
        u8(static_cast<u_byte>(((0) << 3) | (base_r & 7)));
        u8(imm);
    }
    /* test al, imm8. */
    void test_al_imm(u_byte imm) { u8(0xA8); u8(imm); }
    /* cmp byte [r11+disp], imm8. */
    void cmp8_anchor(std::int32_t disp, u_byte imm) {
        rex(false, 0, false, true); u8(0x80); modrm_r11(7, disp); u8(imm);
    }
    /* add reg, imm32 (64-bit). */
    void add_ri32(u_byte r, std::int32_t imm) {
        rex(true, 0, false, r >= 8); u8(0x81);
        u8(static_cast<u_byte>(0xC0 | (0 << 3) | (r & 7)));
        u32(static_cast<std::uint32_t>(imm));
    }

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
    u_word key = 0;                 /* (physical entry << 2) | (privilege << 1) | paged */
    u_word entry_va = 0;
    u_word page = 0;                /* physical page number of the entry (blocks never span pages) */
    jit_fn code = nullptr;
    u_word instr_count = 0;
    bool paged = false;             /* compiled while Sv48 (MODE==1) was active (maize-341) */
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
    std::uint64_t emitted_inline = 0;    /* static: instructions emitted as inline templates */
    std::uint64_t emitted_thunk = 0;     /* static: instructions emitted as thunk calls */
    std::uint64_t uncovered_end[256] = {};   /* static: block-ending uncovered opcode histogram */
    std::uint64_t dispatch_hit = 0;      /* dispatcher entries that ran a compiled block */
    std::uint64_t dispatch_miss = 0;     /* dispatcher entries that fell back to the interpreter */
    std::unordered_map<u_word, std::uint64_t> miss_pcs;   /* diagnostic: where misses land */
};
jit_state g_jit;

const u_word JIT_NEVER_COMPILE = ~u_word{0};

inline bool jit_mode_paged() { return (control_regs[0].w0 & 0xF) == 1; }

/* Block key folds in BOTH privilege and paging mode (maize-341): a bare block and a
   paged block at the same physical entry are distinct compiled objects with different
   memory templates, so they must never collide. */
inline u_word jit_block_key(u_word pc) {
    return (translate(pc, access_kind::fetch) << 2)
         | (privilege_flag ? u_word{2} : u_word{0})
         | (jit_mode_paged() ? u_word{1} : u_word{0});
}

/* =====================================================================================
 * Fault-safe memory for paged blocks (maize-341). Under Sv48 any guest memory access can
 * page-fault, and raise_page_fault throws page_fault_redirect (a C++ exception) which must
 * NEVER unwind through an emitted (non-C++) frame. So a paged block routes every memory op
 * through a C++ wrapper whose own try/catch absorbs the throw: by the time the throw
 * happens the fault is already DELIVERED (deliver_vectored ran, RP points at the handler,
 * the saved RF carries materialized flags), so catching it and returning leaves clean
 * handler state; the block returns to the dispatcher, which resumes at RP. Args ride
 * globals so the wrapper dodges the 4-register-arg call limit. */
void* jit_fs_fn = nullptr;
u_word jit_fs_a[3] = { 0, 0, 0 };
u_word jit_fs_n = 0;    /* u_word (not int) so the emitter's 8-byte store_const is safe */
bool jit_pending_fault = false;
void jit_faultsafe_call() {
    try {
        switch (jit_fs_n) {
            case 0: reinterpret_cast<void(*)()>(jit_fs_fn)(); break;
            case 1: reinterpret_cast<void(*)(u_word)>(jit_fs_fn)(jit_fs_a[0]); break;
            case 2: reinterpret_cast<void(*)(u_word, u_word)>(jit_fs_fn)(jit_fs_a[0], jit_fs_a[1]); break;
            case 3: reinterpret_cast<void(*)(u_word, u_word, u_word)>(jit_fs_fn)(jit_fs_a[0], jit_fs_a[1], jit_fs_a[2]); break;
        }
    } catch (page_fault_redirect&) {
        jit_pending_fault = true;
    }
}

/* =====================================================================================
 * Differential-check store journal. In --jit-check mode the memory write seams record
 * every (address, value, width) a covered store produces, once for the compiled run and
 * once for the interpreter's oracle run, so the two write streams can be compared
 * byte-for-byte (spec item 8: "the bytes written to memory must match").
 * ===================================================================================== */

/* Code-page presence bitmap, indexed by (physical page number & 0xFFFFF). Nonzero
 * means "a compiled block MAY live on a page hashing here" (collisions give safe false
 * positives). The interpreter's write seams use it as a cheap pre-filter and compiled
 * stores probe it inline; the exact answer stays in g_jit.code_pages. */
u_byte jit_page_bitmap[std::size_t(1) << 20] = {};
inline std::size_t jit_bitmap_slot(u_word pageno) { return static_cast<std::size_t>(pageno & 0xFFFFF); }

/* Central indirect-transfer probe table (docs/design/jit.md 3.2b): a direct-mapped
 * CACHE over g_jit.table, keyed by the block key, probed INLINE at every indirect
 * exit (RET, JMP/CALL through a register) so the hot return path stays in compiled
 * code instead of round-tripping through the dispatcher's hash map (measured: 30M
 * dispatcher entries per doom_bench run before this existed). Insert overwrites on
 * collision (the evicted block stays reachable through the dispatcher); the empty
 * sentinel is all-ones, which no reachable key equals. */
const u_word JIT_PROBE_BITS = 16;
const u_word JIT_PROBE_MULT = 0x9E3779B97F4A7C15ull;
u_word jit_probe_keys[std::size_t(1) << JIT_PROBE_BITS];
const void* jit_probe_code[std::size_t(1) << JIT_PROBE_BITS] = {};
inline std::size_t jit_probe_slot(u_word key) {
    return static_cast<std::size_t>((key * JIT_PROBE_MULT) >> (64 - JIT_PROBE_BITS));
}
void jit_probe_reset() {
    std::memset(jit_probe_keys, 0xFF, sizeof jit_probe_keys);
    std::memset(jit_probe_code, 0, sizeof jit_probe_code);
}
inline void jit_probe_insert(u_word key, const void* code) {
    std::size_t s = jit_probe_slot(key);
    jit_probe_keys[s] = key;
    jit_probe_code[s] = code;
}
inline void jit_probe_remove(u_word key) {
    std::size_t s = jit_probe_slot(key);
    if (jit_probe_keys[s] == key) {
        jit_probe_keys[s] = ~u_word{0};
        jit_probe_code[s] = nullptr;
    }
}

/* maize-346: fault-safe VA->PA translate for the paged probe tail. A paged block's
   indirect exit (RET/JMP/CALL) or cross-guest-page direct branch consults the physical-
   keyed probe, which needs the target's physical address; the fast (cache-hit) translate
   runs inline in emitted code, and only a fast-page miss lands here through
   jit_faultsafe_call. translate() can raise a fetch page fault (the target VA is unmapped
   or non-executable, a legitimate guest scenario); jit_faultsafe_call's try/catch absorbs
   the throw and sets jit_pending_fault, so no page_fault_redirect unwinds the emitted
   frame. current_instr_pc is set to the target so the FAULT-class saved PC matches the
   dispatcher's own block-entry fetch fault (jit_dispatch sets it before its key translate).
   On success the physical address returns via jit_probe_pa. */
u_word jit_probe_pa = 0;
void jit_probe_translate() {
    current_instr_pc = regs::rp.w0;
    jit_probe_pa = translate(regs::rp.w0, access_kind::fetch);
}

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
    /* Bitmap pre-filter: almost every store misses it with two byte loads. */
    if (jit_page_bitmap[jit_bitmap_slot(first)] == 0
        && jit_page_bitmap[jit_bitmap_slot(last)] == 0) { return; }
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
/* JMP through a register address: RP = load8(full register), mirroring LBL_jmp_regAddr. */
void jit_jmp_regaddr(u_word desc) {
    copy_regaddr_reg(jit_reg(static_cast<u_byte>(desc)), subreg_enum::w0, regs::rp, subreg_enum::w0);
}
/* JMP through an immediate address (literal baked): RP = load8(addr_lit). */
void jit_jmp_immaddr(u_word addr_lit) {
    reg target; target.w0 = 0;
    mm.read(translate(addr_lit, access_kind::load), target, 8, 0);
    regs::rp.w0 = target.w0;
}
/* CALL through a register value (the DOOM colfunc/spanfunc shape): push the baked
   return address, then RP = zext(encoded subregister), mirroring LBL_call_regVal's
   push-then-read order exactly. */
void jit_call_regval(u_word desc, u_word ret_addr) {
    jit_call_push(ret_addr);
    copy_regval_reg_zext(jit_reg(static_cast<u_byte>(desc)), jit_sub(static_cast<u_byte>(desc)),
                         regs::rp, subreg_enum::w0);
}
/* CALL through a register address: resolve the target FIRST (fault-atomic), then push,
   then jump, mirroring LBL_call_regAddr. */
void jit_call_regaddr(u_word desc, u_word ret_addr) {
    reg target; target.w0 = 0;
    copy_regaddr_reg(jit_reg(static_cast<u_byte>(desc)), jit_sub(static_cast<u_byte>(desc)),
                     target, subreg_enum::w0);
    jit_call_push(ret_addr);
    regs::rp.w0 = target.w0;
}
/* CALL through an immediate address (literal baked): double-dereference then push. */
void jit_call_immaddr(u_word addr_lit, u_word ret_addr) {
    reg target; target.w0 = 0;
    mm.read(translate(addr_lit, access_kind::load), target, 8, 0);
    jit_call_push(ret_addr);
    regs::rp.w0 = target.w0;
}

/* maize-341: the set of semantic thunks that touch guest memory (and so can page-fault
   under Sv48). A paged block emits these through the fault-safe path. Any thunk NOT here
   operates on registers only. */
bool jit_thunk_is_mem(void* fn) {
    return fn == reinterpret_cast<void*>(&jit_alu_mr)   || fn == reinterpret_cast<void*>(&jit_alu_mr_nw)
        || fn == reinterpret_cast<void*>(&jit_alu_imr)  || fn == reinterpret_cast<void*>(&jit_alu_imr_nw)
        || fn == reinterpret_cast<void*>(&jit_ld_rr)    || fn == reinterpret_cast<void*>(&jit_ldz_rr)
        || fn == reinterpret_cast<void*>(&jit_ld_ir)    || fn == reinterpret_cast<void*>(&jit_ldz_ir)
        || fn == reinterpret_cast<void*>(&jit_st_rr)    || fn == reinterpret_cast<void*>(&jit_st_ir)
        || fn == reinterpret_cast<void*>(&jit_push_reg) || fn == reinterpret_cast<void*>(&jit_push_imm)
        || fn == reinterpret_cast<void*>(&jit_pop)      || fn == reinterpret_cast<void*>(&jit_call_push)
        || fn == reinterpret_cast<void*>(&jit_ret_pop)  || fn == reinterpret_cast<void*>(&jit_jmp_regaddr)
        || fn == reinterpret_cast<void*>(&jit_jmp_immaddr) || fn == reinterpret_cast<void*>(&jit_call_regval)
        || fn == reinterpret_cast<void*>(&jit_call_regaddr) || fn == reinterpret_cast<void*>(&jit_call_immaddr);
}

/* Per-block-exit boundary events: advance the perf counter, the instruction-tick timer,
   and the input poll by the block's instruction count, then report whether the compiled
   chain must break (deliverable interrupt pending, or the machine stopped). Interrupt
   and timer granularity coarsens from one instruction to one block, bounded by the
   512-instruction cap (docs/design/jit.md 3.3). In differential-check mode the oracle
   interpreter run owns all of this state, so the compiled side only tallies coverage. */
/* Out-of-line halves of the inlined block-exit events: the (rare) armed-timer tick
   and the (stride-crossing) input poll. */
void jit_timer_n(u_word n) {
    if (active_timer_ptr == nullptr) { return; }
    advance_active_timer(*active_timer_ptr, n);   // maize-357: one O(1) bulk subtract, was a per-tick loop
}
void jit_input_tick() {
    if (active_input_ptr != nullptr) { active_input_ptr->on_input_tick(); }
}

u_word jit_boundary_events(u_word n) {
    g_jit.covered_instr += n;
    if (jit_check_mode) { return 1; }   /* always return to the dispatcher (no chaining) */
    if (perf_count_enabled) { perf_insn_count += n; }
    if (active_timer_ptr != nullptr && (active_timer_ptr->control_reg.w0 & 0x1) != 0) {
        advance_active_timer(*active_timer_ptr, n);   // maize-357: one O(1) bulk subtract, was a per-tick loop
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
 * Inline templates (decision 9801: the J1+J3 merge). Each covered hot shape compiles
 * to straight-line host code operating on [r11+disp32] VM state; the semantics are the
 * SAME formulas the interpreter's helpers use, verified end to end by --jit-check.
 * Shapes not handled here fall back to the semantic thunks above.
 * ===================================================================================== */

static_assert(offsetof(pending_flags_t, dirty) == 0
           && offsetof(pending_flags_t, op_kind) == 1
           && offsetof(pending_flags_t, width) == 2
           && offsetof(pending_flags_t, dst_before) == 8
           && offsetof(pending_flags_t, src) == 16
           && offsetof(pending_flags_t, result) == 24,
           "maize-330: inline flag staging depends on pending_flags_t's layout");

/* A guest subregister as a host-addressable byte range: on a little-endian host every
   subregister is exactly `size` bytes at a byte offset inside the register's u_word. */
struct gsub { std::int32_t disp; u_byte size; };
inline gsub jit_gsub(u_byte desc) {
    reg* r = reg_map[(desc & opflag_reg) >> 4];
    u_byte sr = desc & opflag_subreg;
    gsub g;
    g.size = subreg_size_map[sr];
    g.disp = jit_disp(reinterpret_cast<u_byte*>(&r->w0) + (subreg_offset_map[sr] / 8));
    return g;
}
/* Inline-eligible operand: a real subregister of a register the templates may touch
   directly. RF stays thunk-only (materialize-on-read + the commit_reg_w0 privilege
   mask); RP is never a plain operand in compiled code. */
inline bool jit_inl_desc(u_byte desc) {
    u_byte ri_ = (desc & opflag_reg) >> 4;
    if (ri_ == 0xC || ri_ == 0xE) { return false; }
    u_byte sz = subreg_size_map[desc & opflag_subreg];
    return sz == 1 || sz == 2 || sz == 4 || sz == 8;
}

/* Emit a thunk call and re-establish the anchor register (thunks clobber r11). */
inline void jit_call_thunk(jit_emitter& e, void* fn, int argc, const u_word* argv) {
    e.call_fn(fn, argc, argv);
    if (jit_anchor_ok) { e.mov_ri(11, reinterpret_cast<u_word>(jit_anchor)); }
}

/* True while jit_compile is emitting a paged (Sv48) block; memory templates consult it
   to route through the fault-safe path. */
bool jit_compiling_paged = false;

/* Emit the paged-block fault check: if the last fault-safe call recorded a page fault,
   return to the dispatcher (the fault is already delivered, RP is at the handler);
   otherwise fall through. Uses r10 as a scratch address register. */
void jit_emit_fault_check(jit_emitter& e) {
    e.mov_ri(10, reinterpret_cast<u_word>(&jit_pending_fault));
    e.u8(0x41); e.u8(0x80); e.u8(0x3A); e.u8(0x00);   /* cmp byte [r10], 0 */
    e.u8(0x74); e.u8(0x01);                            /* jz +1 (skip the ret) */
    e.u8(0xC3);                                        /* ret to dispatcher */
}

/* Emit a memory-op thunk. In a bare block this is a plain thunk call (cannot fault). In a
   paged block it routes through jit_faultsafe_call (args via globals), sets the faulting
   guest PC, and checks for a delivered fault. */
void jit_emit_mem_thunk(jit_emitter& e, void* fn, int n, const u_word* args, u_word pc, bool paged) {
    if (!paged) { jit_call_thunk(e, fn, n, args); return; }
    e.store_const(&current_instr_pc, pc);
    e.store_const(&jit_fs_fn, reinterpret_cast<u_word>(fn));
    for (int i = 0; i < n; ++i) { e.store_const(&jit_fs_a[i], args[i]); }
    e.store_const(&jit_fs_n, static_cast<u_word>(n));
    jit_call_thunk(e, reinterpret_cast<void*>(&jit_faultsafe_call), 0, nullptr);
    jit_emit_fault_check(e);
}

/* Truncate reg to `size` bytes zero-extended, in place (no-op for size 8). */
inline void jit_emit_trunc(jit_emitter& e, u_byte r, u_byte size) {
    if (size != 8) { e.movzx_rr(r, r, size); }
}

/* Stage pending flags: header (dirty|op|width), then dst_before / src / result from
   host registers or constants. reg==0xFF with a constant means "store the imm". */
inline void jit_emit_stage_hdr(jit_emitter& e, u_byte opk, u_byte width) {
    std::uint32_t hdr = 1u | (static_cast<std::uint32_t>(opk) << 8) | (static_cast<std::uint32_t>(width) << 16);
    e.store_imm32_dword_anchor(jit_disp(&pending_flags), hdr);
}
inline void jit_emit_stage_field(jit_emitter& e, const void* field, u_byte reg_or_ff, u_word imm) {
    std::int32_t d = jit_disp(field);
    if (reg_or_ff != 0xFF) { e.store_anchor(reg_or_ff, d, 8); return; }
    if (static_cast<u_word>(static_cast<std::int64_t>(static_cast<std::int32_t>(imm))) == imm) {
        e.store_imm32_anchor(d, static_cast<std::int32_t>(imm));
    } else {
        e.mov_ri(10, imm);                        /* r10 scratch */
        e.store_anchor(10, d, 8);
    }
}

/* maize-341: inline Sv48 fast-page translate for a paged block. Transforms addr_reg from
   a guest VA to a physical address in place using the maize-317 per-access-kind fast-page
   cache, exactly mirroring translate()'s inline fast path (key = ((VPN & mask) << 1 |
   privilege) + 1, compared to fast_pages[kind].key; hit -> pa_page | offset). A miss goes
   to slow_sites (the fault-safe thunk, which runs translate_slow and may fault). A
   fast-page HIT proves the page is mapped with permission for this kind, so the subsequent
   L1-hit access cannot fault. The block's mode is fixed (paged), so no runtime MODE test
   is needed. Uses rdx and r10 as scratch (addr_reg is rax or rcx at every call site).
   Clobbers rdx, r10, and addr_reg. */
inline void jit_emit_fastpage(jit_emitter& e, u_byte addr_reg, access_kind kind,
                              std::vector<u_byte*>& slow_sites) {
    std::size_t k = static_cast<std::size_t>(kind);
    e.mov_rr(2, addr_reg);                                  /* rdx = va */
    e.shift_ri(5, 2, 12);                                   /* rdx = vpn (full) */
    e.mov_ri(10, vpn_tag_mask);
    e.rr_op(0x23, 2, 10);                                   /* and rdx, r10 (vpn & mask) */
    e.shift_ri(4, 2, 1);                                    /* rdx <<= 1 */
    e.load_anchor(10, jit_disp(&regs::rf.w0), 8, false);    /* r10 = rf */
    e.shift_ri(5, 10, 32);
    e.and_ri32_32(10, 1);                                   /* r10 = privilege (RF bit 32) */
    e.or_rr(2, 10);                                         /* rdx = (vpn<<1)|priv */
    e.add_ri32(2, 1);                                       /* rdx = key */
    e.cmp_anchor(2, jit_disp(&fast_pages[k].key));
    slow_sites.push_back(e.jcc_placeholder(0x5));           /* jne slow */
    e.load_anchor(10, jit_disp(&fast_pages[k].pa_page), 8, false);   /* r10 = pa_page */
    e.and_ri32(addr_reg, 0xFFF);                            /* addr_reg = va offset */
    e.or_rr(addr_reg, 10);                                  /* addr_reg = pa_page | offset = PA */
}

/* The guest-memory fast path: resolve `addr_reg` (a host reg holding the guest VA) to a
   host pointer in r8 with the in-page offset in rdx, or branch to `slow_sites` on any
   miss. Under paging it first runs the inline fast-page translate (VA -> PA); in Bare mode
   addr_reg is already the PA. Mirrors set_cache_address's hit path plus the page-bound
   check for the access width. Clobbers rdx, r8, r9, r10. */
inline void jit_emit_l1_probe(jit_emitter& e, u_byte addr_reg, u_byte width,
                              access_kind kind, std::vector<u_byte*>& slow_sites,
                              bool already_pa = false) {
    /* Under paging, translate VA -> PA inline first (unless the caller already did, which
       the store templates do so their code-page bitmap probe keys on the physical page). */
    if (jit_compiling_paged && !already_pa) { jit_emit_fastpage(e, addr_reg, kind, slow_sites); }
    /* rdx = (addr >> 12) & l1_mask */
    e.mov_rr(2, addr_reg);
    e.shift_ri(5, 2, 12);
    e.and_ri32_32(2, static_cast<std::uint32_t>(memory_module::jit_l1_mask()));
    /* r10 = l1_base[slot]; r8 = l1_ptr[slot] */
    e.load_anchor_idx8(10, 2, jit_disp(mm.jit_l1_base_data()));
    e.load_anchor_idx8(8, 2, jit_disp(mm.jit_l1_ptr_data()));
    /* r9 = addr & ~0xFFF; hit iff base matches and the slot is populated */
    e.mov_rr(9, addr_reg);
    e.and_ri32(9, -4096);
    e.cmp_rr(10, 9);
    slow_sites.push_back(e.jcc_placeholder(0x5));      /* jne slow */
    e.test_rr(8, 8);
    slow_sites.push_back(e.jcc_placeholder(0x4));      /* jz slow (never-touched slot) */
    /* rdx = in-page offset; width > 1 must not cross the block end */
    e.mov_r32_r32(2, addr_reg);
    e.and_ri32_32(2, 0xFFF);
    if (width > 1) {
        e.cmp_ri32_32(2, 4096u - width);
        slow_sites.push_back(e.jcc_placeholder(0x7));  /* ja slow */
    }
}

/* Maize condition index -> x86 condition code. The predicate formulas are identical
   (eval_condition's table IS the x86 flag algebra over the same C/N/V/Z definitions),
   so a width-sized host compare drives the same branch decision. Index 10 (P, the
   FCMP unordered bit) has no integer-compare equivalent and is never fused. */
inline int jit_cc_for_cond(u_word idx) {
    switch (idx) {
        case 0: return 0x4;   /* Z  -> e  */
        case 1: return 0x5;   /* NZ -> ne */
        case 2: return 0xC;   /* LT -> l  */
        case 3: return 0x2;   /* B  -> b  */
        case 4: return 0xF;   /* GT -> g  */
        case 5: return 0x7;   /* A  -> a  */
        case 6: return 0xD;   /* GE -> ge */
        case 7: return 0xE;   /* LE -> le */
        case 8: return 0x6;   /* BE -> be */
        case 9: return 0x3;   /* AE -> ae */
        default: return -1;
    }
}

/* Try to emit `op` at `pc` as an inline template. Returns true (and sets len_out)
   when handled; false falls back to the thunk/uncovered paths. */
bool jit_try_inline(jit_emitter& e, u_byte op, u_word pc, u_word& len_out) {
    if (!jit_anchor_ok) { return false; }
    u_byte base = op & 0x3F;
    u_byte mode = op & opcode_flag;


    /* ---- CP / CPZ, register source ---- */
    if (op == instr::cp_regVal_reg || op == instr::cpz_regVal_reg) {
        u_byte d1 = jg8(pc + 1), d2 = jg8(pc + 2);
        if (!jit_inl_desc(d1) || !jit_inl_desc(d2)) { return false; }
        gsub s = jit_gsub(d1), d = jit_gsub(d2);
        e.load_anchor(0, s.disp, s.size, op == instr::cp_regVal_reg);
        e.store_anchor(0, d.disp, d.size);
        len_out = 3;
        return true;
    }
    /* ---- CP / CPZ, immediate source (value baked, CP pre-sign-extended) ---- */
    if (op == instr::cp_immVal_reg || op == instr::cpz_immVal_reg) {
        u_byte immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
        u_byte d2 = jg8(pc + 2);
        if (!jit_inl_desc(d2)) { return false; }
        u_word raw = jgN(pc + 3, immsz);
        u_word v = (op == instr::cp_immVal_reg) ? jit_signext(raw, immsz) : raw;
        if (jit_inject_miscompile) { v += 1; }         /* AC-4 net proof, same knob as the thunk */
        gsub d = jit_gsub(d2);
        if (d.size == 8 && static_cast<u_word>(static_cast<std::int64_t>(static_cast<std::int32_t>(v))) == v) {
            e.store_imm32_anchor(d.disp, static_cast<std::int32_t>(v));
        } else {
            e.mov_ri(0, v);
            e.store_anchor(0, d.disp, d.size);
        }
        len_out = 3u + immsz;
        return true;
    }
    /* ---- LD / LDZ through a register address ---- */
    if (op == instr::ld_regAddr_reg || op == instr::ldz_regAddr_reg) {
        u_byte d1 = jg8(pc + 1), d2 = jg8(pc + 2);
        if (!jit_inl_desc(d1) || !jit_inl_desc(d2)) { return false; }
        gsub a = jit_gsub(d1), d = jit_gsub(d2);
        std::vector<u_byte*> slow;
        e.load_anchor(1, a.disp, a.size, false);       /* rcx = guest address */
        jit_emit_l1_probe(e, 1, d.size, access_kind::load, slow);
        e.load_mem(0, 8, 2, d.size, false);            /* rax = zext(loaded bytes) */
        if (op == instr::ld_regAddr_reg) {
            e.store_anchor(0, d.disp, d.size);         /* merge into the dst field */
        } else {
            reg* dr = reg_map[(d2 & opflag_reg) >> 4];
            e.store_anchor(0, jit_disp(&dr->w0), 8);   /* LDZ: full-register zext */
        }
        u_byte* done = e.jump_placeholder();
        u_byte* slow_lbl = e.buf;
        for (u_byte* s : slow) { jit_patch_branch(s, slow_lbl); }
        {
            u_word args[2] = { d1, d2 };
            jit_emit_mem_thunk(e,
                (op == instr::ld_regAddr_reg) ? reinterpret_cast<void*>(&jit_ld_rr)
                                              : reinterpret_cast<void*>(&jit_ldz_rr), 2, args, pc, jit_compiling_paged);
        }
        jit_patch_branch(done, e.buf);
        len_out = 3;
        return true;
    }
    /* ---- ST, register value to register address ---- */
    if (op == instr::st_regVal_regAddr) {
        u_byte d1 = jg8(pc + 1), d2 = jg8(pc + 2);
        if (!jit_inl_desc(d1) || !jit_inl_desc(d2)) { return false; }
        gsub s = jit_gsub(d1), a = jit_gsub(d2);
        std::vector<u_byte*> slow;
        e.load_anchor(0, s.disp, s.size, false);       /* rax = value bytes */
        e.load_anchor(1, a.disp, a.size, false);       /* rcx = guest address (VA) */
        /* Paged: translate VA -> PA first so the code-page bitmap probe below keys on the
           physical page (the bitmap and the write seams are physical). */
        if (jit_compiling_paged) { jit_emit_fastpage(e, 1, access_kind::store, slow); }
        /* Code-page bitmap probe: a store that might hit compiled code goes slow
           (the thunk lands in the write seams, which invalidate). */
        e.mov_rr(9, 1);
        e.shift_ri(5, 9, 12);
        e.and_ri32_32(9, 0xFFFFF);
        e.cmp8_anchor_idx1(9, jit_disp(jit_page_bitmap), 0);
        slow.push_back(e.jcc_placeholder(0x5));        /* jne slow */
        if (jit_check_mode) {
            /* Differential runs journal every store; route through the seams. */
            e.cmp64_anchor_imm8(jit_disp(&jit_journal), 0);
            slow.push_back(e.jcc_placeholder(0x5));
        }
        jit_emit_l1_probe(e, 1, s.size, access_kind::store, slow, /*already_pa=*/jit_compiling_paged);
        e.store_mem(0, 8, 2, s.size);
        u_byte* done = e.jump_placeholder();
        u_byte* slow_lbl = e.buf;
        for (u_byte* sst : slow) { jit_patch_branch(sst, slow_lbl); }
        {
            u_word args[2] = { d1, d2 };
            jit_emit_mem_thunk(e, reinterpret_cast<void*>(&jit_st_rr), 2, args, pc, jit_compiling_paged);
        }
        jit_patch_branch(done, e.buf);
        len_out = 3;
        return true;
    }
    /* ---- Integer ALU, register or immediate source ---- */
    {
        bool is_row_family = base == 0x31 || base == 0x32
            || (base >= instr::setcc_base && base <= instr::setcc_base + 2)
            || (base >= instr::jcc_base && base <= instr::jcc_base + 2)
            || base == 0x27 || base == 0x29 || base == 0x24 || base == 0x22
            || base == 0x33 || base == 0x39 || base == 0x3A || base == 0x15 || base == 0x28;
        bool alu_ok = !is_row_family
            && (base == instr::add_opcode || base == instr::sub_opcode || base == instr::cmp_opcode
             || base == instr::and_opcode || base == instr::or_opcode || base == instr::xor_opcode
             || base == instr::nor_opcode || base == instr::nand_opcode
             || base == instr::test_opcode || base == instr::mul_opcode);
        bool shift_ok = !is_row_family
            && (base == instr::shl_opcode || base == instr::shr_opcode || base == instr::sar_opcode);
        bool arith = base == instr::add_opcode || base == instr::sub_opcode
                  || base == instr::cmp_opcode || base == instr::mul_opcode;
        bool writeback = base != instr::cmp_opcode && base != instr::test_opcode;

        if ((alu_ok || shift_ok) && (mode == opcode_flag_srcReg || mode == opcode_flag_srcImm)) {
            bool immform = (mode == opcode_flag_srcImm);
            u_byte dst_desc = jg8(pc + 2);
            if (!jit_inl_desc(dst_desc)) { return false; }
            gsub d = jit_gsub(dst_desc);
            u_byte w = d.size;
            u_byte immsz = 0;
            u_word imm_sx = 0;
            u_byte src_desc = 0;
            if (immform) {
                immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
                imm_sx = jit_signext(jgN(pc + 3, immsz), immsz);
            } else {
                src_desc = jg8(pc + 1);
                if (!jit_inl_desc(src_desc)) { return false; }
            }
            u_word ilen = immform ? (3u + immsz) : 3u;

            if (shift_ok) {
                if (!immform) { return false; }        /* register counts keep the thunk's edge ladder */
                /* Effective count: the sign-extended immediate cast to the operation
                   width, exactly as alu_shl/shr/sar receive it. */
                u_word n_eff;
                switch (w) {
                    case 1: n_eff = static_cast<u_byte>(imm_sx); break;
                    case 2: n_eff = static_cast<u_qword>(imm_sx); break;
                    case 4: n_eff = static_cast<u_hword>(imm_sx); break;
                    default: n_eff = imm_sx; break;
                }
                u_word bits = static_cast<u_word>(w) * 8;
                if (n_eff == 0) { len_out = ilen; return true; }   /* value unchanged, no staging */
                e.load_anchor(1, d.disp, w, false);    /* rcx = dst_before (zext) */
                if (base == instr::sar_opcode) {
                    /* Arithmetic shift of the 64-bit sign-extended value, count clamped
                       to 63, then width-truncate: identical low bits for n < bits and
                       the correct sign fill for n >= bits (alu_sar's two branches). */
                    e.load_anchor(2, d.disp, w, true); /* rdx = sign-extended value */
                    u_byte sh = static_cast<u_byte>(n_eff < 63 ? n_eff : 63);
                    e.shift_ri(7, 2, sh);
                } else if (n_eff >= bits) {
                    e.xor_rr(2, 2);                    /* SHL/SHR count >= width: zero */
                } else {
                    e.mov_rr(2, 1);
                    e.shift_ri(base == instr::shl_opcode ? 4 : 5, 2, static_cast<u_byte>(n_eff));
                }
                jit_emit_trunc(e, 2, w);
                e.store_anchor(2, d.disp, w);
                jit_emit_stage_hdr(e, base, w);
                jit_emit_stage_field(e, &pending_flags.dst_before, 1, 0);
                jit_emit_stage_field(e, &pending_flags.src, 0xFF, n_eff);
                jit_emit_stage_field(e, &pending_flags.result, 2, 0);
                len_out = ilen;
                return true;
            }

            /* Plain two-operand ALU. rax = src (sign-extended), rcx = dst_before
               (width-zext), rdx = width-truncated result; staging mirrors the
               alu_* helpers exactly. */
            if (immform) {
                if (static_cast<u_word>(static_cast<std::int64_t>(static_cast<std::int32_t>(imm_sx))) == imm_sx) {
                    e.rex(true, 0, false, false); e.u8(0xC7); e.u8(0xC0);
                    e.u32(static_cast<std::uint32_t>(imm_sx));  /* mov rax, imm32 (sx) */
                } else {
                    e.mov_ri(0, imm_sx);
                }
            } else {
                gsub s = jit_gsub(src_desc);
                e.load_anchor(0, s.disp, s.size, true);
            }
            e.load_anchor(1, d.disp, w, false);
            switch (base) {
                case instr::add_opcode: e.lea_sum(2, 1, 0); break;
                case instr::sub_opcode:
                case instr::cmp_opcode: e.mov_rr(2, 1); e.sub_rr(2, 0); break;
                case instr::and_opcode:
                case instr::test_opcode: e.mov_rr(2, 1); e.and_rr(2, 0); break;
                case instr::or_opcode:  e.mov_rr(2, 1); e.or_rr(2, 0); break;
                case instr::xor_opcode: e.mov_rr(2, 1); e.xor_rr(2, 0); break;
                case instr::nor_opcode: e.mov_rr(2, 1); e.or_rr(2, 0); e.not_r(2); break;
                case instr::nand_opcode: e.mov_rr(2, 1); e.and_rr(2, 0); e.not_r(2); break;
                case instr::mul_opcode: e.mov_rr(2, 1); e.imul_rr(2, 0); break;
                default: return false;
            }
            jit_emit_trunc(e, 2, w);
            if (writeback) { e.store_anchor(2, d.disp, w); }
            jit_emit_stage_hdr(e, base, w);
            if (arith) {
                jit_emit_stage_field(e, &pending_flags.dst_before, 1, 0);
                if (immform) {
                    u_word src_tr;
                    switch (w) {
                        case 1: src_tr = static_cast<u_byte>(imm_sx); break;
                        case 2: src_tr = static_cast<u_qword>(imm_sx); break;
                        case 4: src_tr = static_cast<u_hword>(imm_sx); break;
                        default: src_tr = imm_sx; break;
                    }
                    jit_emit_stage_field(e, &pending_flags.src, 0xFF, src_tr);
                } else {
                    jit_emit_trunc(e, 0, w);
                    jit_emit_stage_field(e, &pending_flags.src, 0, 0);
                }
            } else {
                jit_emit_stage_field(e, &pending_flags.dst_before, 0xFF, 0);
                jit_emit_stage_field(e, &pending_flags.src, 0xFF, 0);
            }
            jit_emit_stage_field(e, &pending_flags.result, 2, 0);
            len_out = ilen;
            return true;
        }
    }
    /* ---- LEA, register and immediate source: flag-neutral add at the second
       operand's width, landed in the third operand's field (mirrors jit_lea_rr /
       jit_lea_ir minus the run_alu staging dance, which nets to no flag effect). ---- */
    if (op == instr::lea_regVal_regreg || op == instr::lea_immVal_regreg) {
        bool immform = (op == instr::lea_immVal_regreg);
        u_byte immsz = 0;
        u_byte d1 = 0;
        if (immform) {
            immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
        } else {
            d1 = jg8(pc + 1);
            if (!jit_inl_desc(d1)) { return false; }
        }
        u_byte d2 = jg8(pc + 2), d3 = jg8(pc + 3);
        if (!jit_inl_desc(d2) || !jit_inl_desc(d3)) { return false; }
        gsub s2 = jit_gsub(d2), d = jit_gsub(d3);
        if (immform) {
            u_word v = jit_signext(jgN(pc + 4, immsz), immsz);
            if (static_cast<u_word>(static_cast<std::int64_t>(static_cast<std::int32_t>(v))) == v) {
                e.rex(true, 0, false, false); e.u8(0xC7); e.u8(0xC0);
                e.u32(static_cast<std::uint32_t>(v));
            } else {
                e.mov_ri(0, v);
            }
        } else {
            gsub s1 = jit_gsub(d1);
            e.load_anchor(0, s1.disp, s1.size, true);
        }
        e.load_anchor(1, s2.disp, s2.size, true);
        e.lea_sum(2, 1, 0);
        jit_emit_trunc(e, 2, s2.size);        /* op width = the second operand's size */
        e.store_anchor(2, d.disp, d.size);
        len_out = immform ? (4u + immsz) : 4u;
        return true;
    }
    /* ---- PUSH register / POP: the L1-hit stack access inline (RS commit order
       mirrors the fault-atomic thunks). Under paging these stay on the fault-safe thunk
       path (maize-341): they write the translated register back to RS, which the in-place
       VA->PA fast-page translate would corrupt, and they are not hot enough to justify the
       extra register bookkeeping. LD/ST (the hot spill pattern) do get the inline
       fast-page above. ---- */
    if (op == instr::push_regVal && !jit_compiling_paged) {
        u_byte d1 = jg8(pc + 1);
        if (!jit_inl_desc(d1)) { return false; }
        gsub s = jit_gsub(d1);
        std::vector<u_byte*> slow;
        e.load_anchor(0, s.disp, s.size, false);      /* rax = value bytes */
        e.load_anchor(1, jit_disp(&regs::rs.w0), 8, false);
        e.lea_disp8(1, 1, static_cast<std::int8_t>(-static_cast<int>(s.size)));
        e.mov_rr(9, 1);
        e.shift_ri(5, 9, 12);
        e.and_ri32_32(9, 0xFFFFF);
        e.cmp8_anchor_idx1(9, jit_disp(jit_page_bitmap), 0);
        slow.push_back(e.jcc_placeholder(0x5));
        if (jit_check_mode) {
            e.cmp64_anchor_imm8(jit_disp(&jit_journal), 0);
            slow.push_back(e.jcc_placeholder(0x5));
        }
        jit_emit_l1_probe(e, 1, s.size, access_kind::store, slow);   /* bare-only path */
        e.store_mem(0, 8, 2, s.size);
        e.store_anchor(1, jit_disp(&regs::rs.w0), 8);
        u_byte* done = e.jump_placeholder();
        u_byte* slow_lbl = e.buf;
        for (u_byte* sst : slow) { jit_patch_branch(sst, slow_lbl); }
        {
            u_word args[1] = { d1 };
            jit_call_thunk(e, reinterpret_cast<void*>(&jit_push_reg), 1, args);
        }
        jit_patch_branch(done, e.buf);
        len_out = 2;
        return true;
    }
    if (op == instr::pop_opcode && !jit_compiling_paged) {
        u_byte d1 = jg8(pc + 1);
        if (!jit_inl_desc(d1)) { return false; }
        gsub d = jit_gsub(d1);
        std::vector<u_byte*> slow;
        e.load_anchor(1, jit_disp(&regs::rs.w0), 8, false);
        jit_emit_l1_probe(e, 1, d.size, access_kind::load, slow);
        e.load_mem(0, 8, 2, d.size, false);
        e.store_anchor(0, d.disp, d.size);
        e.load_anchor(1, jit_disp(&regs::rs.w0), 8, false);
        e.lea_disp8(1, 1, static_cast<std::int8_t>(d.size));
        e.store_anchor(1, jit_disp(&regs::rs.w0), 8);
        u_byte* done = e.jump_placeholder();
        u_byte* slow_lbl = e.buf;
        for (u_byte* sst : slow) { jit_patch_branch(sst, slow_lbl); }
        {
            u_word args[1] = { d1 };
            jit_call_thunk(e, reinterpret_cast<void*>(&jit_pop), 1, args);
        }
        jit_patch_branch(done, e.buf);
        len_out = 2;
        return true;
    }
    /* ---- INC / DEC (ADD/SUB with src = 1; NOT/NEG keep the run_alu thunk) ---- */
    if (op == instr::inc_opcode || op == instr::dec_opcode) {
        u_byte d1 = jg8(pc + 1);
        if (!jit_inl_desc(d1)) { return false; }
        gsub d = jit_gsub(d1);
        e.load_anchor(1, d.disp, d.size, false);
        e.lea_disp8(2, 1, op == instr::inc_opcode ? 1 : -1);
        jit_emit_trunc(e, 2, d.size);
        e.store_anchor(2, d.disp, d.size);
        jit_emit_stage_hdr(e, op == instr::inc_opcode ? alu_uop_inc : alu_uop_dec, d.size);
        jit_emit_stage_field(e, &pending_flags.dst_before, 1, 0);
        jit_emit_stage_field(e, &pending_flags.src, 0xFF, 1);
        jit_emit_stage_field(e, &pending_flags.result, 2, 0);
        len_out = 2;
        return true;
    }
    return false;
}

/* =====================================================================================
 * Block compilation
 * ===================================================================================== */

/* Emit the block-exit event fast path inline (counters, timer-enabled probe, input
   stride probe, running/IRQ check), with helper calls only on the armed branches.
   Bit-for-bit the same effects as jit_boundary_events. Emits jumps to `brk_sites`
   wherever the chain must break (the caller lands them on a `ret`). Requires the
   anchor (r11) live; clobbers rax, rcx, r10 (and volatiles on the rare helper
   paths, which re-establish r11). */
void jit_emit_events_inline(jit_emitter& e, u_word n, std::vector<u_byte*>& brk_sites) {
    e.add_mem_imm32(jit_disp(&g_jit.covered_instr), static_cast<std::int32_t>(n));
    e.add_mem_imm32(jit_disp(&perf_insn_count), static_cast<std::int32_t>(n));
    if (active_timer_ptr != nullptr) {
        e.mov_ri(10, reinterpret_cast<u_word>(&active_timer_ptr->control_reg.w0));
        e.test_m8_imm(10, 0x1);
        u_byte* no_timer = e.jcc_placeholder(0x4);         /* jz: disabled (the norm) */
        {
            u_word args[1] = { n };
            jit_call_thunk(e, reinterpret_cast<void*>(&jit_timer_n), 1, args);
        }
        jit_patch_branch(no_timer, e.buf);
    }
    if (active_input_ptr != nullptr) {
        e.load_anchor(0, jit_disp(&input_tick_ctr_), 8, false);
        e.mov_rr(1, 0);
        e.add_ri32(1, static_cast<std::int32_t>(n));
        e.store_anchor(1, jit_disp(&input_tick_ctr_), 8);
        e.shift_ri(5, 0, 14);
        e.shift_ri(5, 1, 14);
        e.cmp_rr(0, 1);
        u_byte* no_input = e.jcc_placeholder(0x4);         /* je: same stride */
        jit_call_thunk(e, reinterpret_cast<void*>(&jit_input_tick), 0, nullptr);
        jit_patch_branch(no_input, e.buf);
    }
    /* RF's high dword: bit 1 = interrupts enabled (bit 33), bit 3 = running (35). */
    e.load_anchor(0, jit_disp(reinterpret_cast<const u_byte*>(&regs::rf.w0) + 4), 4, false);
    e.test_al_imm(0x08);
    brk_sites.push_back(e.jcc_placeholder(0x4));           /* jz: not running */
    e.test_al_imm(0x02);
    u_byte* no_irq = e.jcc_placeholder(0x4);               /* jz: interrupts masked */
    e.cmp8_anchor(jit_disp(&irq_pending), 0);
    brk_sites.push_back(e.jcc_placeholder(0x5));           /* jne: deliverable IRQ */
    jit_patch_branch(no_irq, e.buf);
}

/* Emit block-exit boundary events, leaving the anchor (r11) live and pushing every
   chain-break site (running-cleared / deliverable-IRQ, plus the rax!=0 break on the
   thunk path) into brk_sites. Requires jit_anchor_ok. Shared by every probe-consulting
   exit tail (bare + paged, indirect + cross-page) so they emit identical event effects. */
void jit_emit_exit_events(jit_emitter& e, u_word n, std::vector<u_byte*>& brk_sites) {
    if (jit_no_inline_events) {
        u_word args[1] = { n };
        e.call_fn(reinterpret_cast<void*>(&jit_boundary_events), 1, args);
        e.mov_ri(11, reinterpret_cast<u_word>(jit_anchor));
        e.test_rr(0, 0);
        brk_sites.push_back(e.jcc_placeholder(0x5));      /* jnz (rax != 0) -> dispatcher */
    } else {
        jit_emit_events_inline(e, n, brk_sites);
    }
}

/* maize-346: paged variant of the probe tail (r11 anchor live). RP holds the target VA;
   translate it VA->PA inline through the maize-341 fault-safe fast-page machinery, form
   the paged physical key ((pa<<2)|(priv<<1)|1), and probe the same direct-mapped cache the
   bare tail uses. A fast-page HIT stays entirely in emitted code with no call; a MISS routes
   through jit_probe_translate under jit_faultsafe_call. On a translate fault the fault is
   already delivered (jit_pending_fault set, RP at the handler), so we ret to the dispatcher,
   which resumes there exactly as an interpreted transfer's fetch fault would. On a clean
   slow translate the physical address returns via jit_probe_pa and rejoins the key+probe.
   Every probe miss and each brk site lands on the trailing ret (back to the dispatcher). */
void jit_emit_probe_tail_paged(jit_emitter& e, std::vector<u_byte*>& brk_sites) {
    e.load_anchor(0, jit_disp(&regs::rp.w0), 8, false);   /* rax = target VA */
    std::vector<u_byte*> xlate_slow;
    jit_emit_fastpage(e, 0, access_kind::fetch, xlate_slow);   /* HIT: rax = PA (clobbers rdx, r10) */
    u_byte* have_pa = e.buf;                               /* convergence: rax = target PA */
    /* key = (pa << 2) | (privilege << 1) | 1. pa<<2 clears bits 0-1 and priv<<1 sets only
       bit 1, so bit 0 is zero and adding 1 sets the paged mode bit with no carry. */
    e.mov_rr(1, 0);
    e.shift_ri(4, 1, 2);                                   /* rcx = pa << 2 */
    e.load_anchor(2, jit_disp(&regs::rf.w0), 8, false);
    e.shift_ri(5, 2, 31);                                  /* privilege (RF bit 32) -> bit 1 */
    e.and_ri32_32(2, 2);
    e.or_rr(1, 2);
    e.add_ri32(1, 1);                                      /* rcx = key (paged mode bit set) */
    e.mov_ri(2, JIT_PROBE_MULT);
    e.imul_rr(2, 1);
    e.shift_ri(5, 2, static_cast<u_byte>(64 - JIT_PROBE_BITS));
    e.cmp_anchor_idx8(1, 2, jit_disp(jit_probe_keys));
    u_byte* miss = e.jcc_placeholder(0x5);                 /* jne -> dispatcher */
    e.load_anchor_idx8(0, 2, jit_disp(jit_probe_code));
    e.jmp_rax();
    u_byte* out = e.buf;
    for (u_byte* s : brk_sites) { jit_patch_branch(s, out); }
    jit_patch_branch(miss, out);
    e.ret();
    /* Fast-page miss: rax still holds the target VA (jit_emit_fastpage only rewrites the
       address register on its hit branch). Fault-safe slow translate, then rejoin. */
    u_byte* slow_lbl = e.buf;
    for (u_byte* s : xlate_slow) { jit_patch_branch(s, slow_lbl); }
    e.store_const(&jit_fs_fn, reinterpret_cast<u_word>(&jit_probe_translate));
    e.store_const(&jit_fs_n, 0);
    jit_call_thunk(e, reinterpret_cast<void*>(&jit_faultsafe_call), 0, nullptr);
    jit_emit_fault_check(e);                               /* fault delivered -> ret to dispatcher */
    e.load_anchor(0, jit_disp(&jit_probe_pa), 8, false);  /* rax = translated PA */
    u_byte* back = e.jump_placeholder();
    jit_patch_branch(back, have_pa);
}

/* maize-346: emit boundary events then the paged probe tail. RP must already hold the
   target VA (the indirect-transfer thunk sets it, or jit_emit_edge stores it for a
   cross-page direct branch). Caller guarantees jit_anchor_ok, !jit_check_mode,
   !jit_no_probe, jit_compiling_paged. */
void jit_emit_paged_probe_exit(jit_emitter& e, u_word n) {
    std::vector<u_byte*> brk_sites;
    jit_emit_exit_events(e, n, brk_sites);
    jit_emit_probe_tail_paged(e, brk_sites);
}

/* Emit a direct exit to a known guest target and record its chainable edge:
   set RP; run boundary events; if the chain must break, return to the dispatcher;
   otherwise take the patchable jump (initially to a local return; later chained
   straight to the successor block). In check mode the edge is never chained. */
void jit_emit_edge(jit_emitter& e, jit_block& b, u_word target_va, u_word n) {
    /* maize-341: a paged block chains a direct edge ONLY when the target is in the same
       guest page as the block entry. A virtual page maps atomically to one physical page,
       so a same-page branch is same-physical-page and stays valid under ANY SATP: whenever
       the source physical block is entered, the target physical block is reached by the
       identical mapping (and if a remap points the virtual page elsewhere, the source is a
       different physical block and is never entered). Cross-page targets translate under
       the live SATP, which can change, so those return to the dispatcher to recompute the
       physical key. This captures the hot in-page loop while staying correct. */
    if (b.paged && (target_va & ~static_cast<u_word>(0xFFF)) != (b.entry_va & ~static_cast<u_word>(0xFFF))) {
        /* maize-346: cross-guest-page paged direct branch. A hard cross-page chain would be
           valid only while the target's VA->PA mapping holds; an SATP switch changes the
           correct target and the unlink machinery only re-chains on target delete+recompile,
           so a hard chain would die permanently on the first context switch. Route through
           the physical-keyed paged probe tail instead: it re-translates every exit and bakes
           in no mapping, so it is self-recovering across SATP changes. (Same-guest-page paged
           branches fall through to the hard chain below: one virtual page maps to one physical
           page, so that chain stays valid under any SATP, the J2 argument.) */
        e.store_const(&regs::rp.w0, target_va);
        if (jit_anchor_ok && !jit_check_mode && !jit_no_probe) {
            jit_emit_paged_probe_exit(e, n);
        } else {
            u_word args[1] = { n };
            e.call_fn(reinterpret_cast<void*>(&jit_boundary_events), 1, args);
            e.ret();
        }
        return;
    }
    /* Set RP with an absolute-addressed store (no dependency on the anchor register
       being live at the block boundary), call the boundary-events helper, and either
       break to the dispatcher (rax != 0) or take the patchable chain jump. The optional
       fully-inline events path is behind MAIZE_JIT_INLINE_EVENTS. */
    if (jit_anchor_ok && !jit_check_mode && !jit_no_inline_events) {
        e.store_const(&regs::rp.w0, target_va);
        std::vector<u_byte*> brk;
        jit_emit_events_inline(e, n, brk);
        u_byte* jmp_site = e.jump_placeholder();
        u_byte* brk_lbl = e.buf;
        for (u_byte* s : brk) { jit_patch_branch(s, brk_lbl); }
        e.ret();
        u_byte* stub = e.buf;
        e.ret();
        jit_patch_branch(jmp_site, stub);
        exit_edge ed;
        ed.patch_site = jmp_site;
        ed.stub = stub;
        ed.target_key = jit_block_key(target_va);
        ed.chained = false;
        b.edges.push_back(ed);
        return;
    }
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

/* Emit the central-table probe tail (r11 live): compute the block key from RP+RF,
   probe the direct-mapped cache, jmp to a hit; every miss and each brk site lands on
   the trailing ret (back to the dispatcher). */
void jit_emit_probe_tail(jit_emitter& e, std::vector<u_byte*>& brk_sites) {
    /* Bare-block probe: key = (pa << 2) | (privilege << 1) | 0 (bare mode bit), matching
       jit_block_key's 2-bit-tag format (maize-341). RP is the physical address in bare
       mode. Paged blocks never reach here (they skip the probe). */
    e.load_anchor(0, jit_disp(&regs::rp.w0), 8, false);   /* rax = target PA (bare) */
    e.mov_rr(1, 0);
    e.shift_ri(4, 1, 2);                                   /* rcx = pa << 2 */
    e.load_anchor(2, jit_disp(&regs::rf.w0), 8, false);
    e.shift_ri(5, 2, 31);                                 /* privilege (RF bit 32) -> bit 1 */
    e.and_ri32_32(2, 2);
    e.or_rr(1, 2);                                         /* rcx = key (mode bit 0 = bare) */
    e.mov_ri(2, JIT_PROBE_MULT);
    e.imul_rr(2, 1);
    e.shift_ri(5, 2, static_cast<u_byte>(64 - JIT_PROBE_BITS));
    e.cmp_anchor_idx8(1, 2, jit_disp(jit_probe_keys));
    u_byte* miss = e.jcc_placeholder(0x5);                 /* jne -> dispatcher */
    e.load_anchor_idx8(0, 2, jit_disp(jit_probe_code));
    e.jmp_rax();
    u_byte* out = e.buf;
    for (u_byte* s : brk_sites) { jit_patch_branch(s, out); }
    jit_patch_branch(miss, out);
    e.ret();
}

void jit_emit_indirect_exit(jit_emitter& e, u_word n) {
    /* Check mode and the no-anchor fallback always round-trip the dispatcher. */
    if (jit_check_mode || !jit_anchor_ok) {
        u_word args[1] = { n };
        e.call_fn(reinterpret_cast<void*>(&jit_boundary_events), 1, args);
        e.ret();
        return;
    }
    /* maize-346: paged blocks now consult the probe too, via the fault-safe paged tail
       (RP is a VA it translates inline); bare blocks read RP as already-physical. */
    if (jit_compiling_paged && !jit_no_probe) {
        jit_emit_paged_probe_exit(e, n);
        return;
    }
    std::vector<u_byte*> brk_sites;
    jit_emit_exit_events(e, n, brk_sites);
    if (jit_no_probe || jit_compiling_paged) {
        u_byte* out = e.buf;
        for (u_byte* s : brk_sites) { jit_patch_branch(s, out); }
        e.ret();
        return;
    }
    jit_emit_probe_tail(e, brk_sites);
}

/* Fuse a flag-producing ALU instruction with an IMMEDIATELY following Jcc into a
   width-sized host compare + a flag-free RF materialization + a native conditional
   branch (decision 9801; the dominant DEC+JNZ / CMP+Jcc loop enders). The emitted
   post-state is EXACTLY the interpreter's post-Jcc state: eval_condition materializes
   the staged flags, so RF carries concrete C/N/V/Z and the pending descriptor is
   clean. Host CF/OF/SF/ZF at the operation width equal Maize's C/V/N/Z by the same
   formulas materialize_flags computes (sub-family: borrow/overflow/sign/zero; add
   family: carry/overflow/sign/zero; logic family: sign/zero with C=V=0). Ops whose
   flags are NOT the host's (MUL's recomputed overflow, NOR/NAND's post-NOT result,
   shifts) are never fused. Returns the fused block-end state via `done`. */
bool jit_try_fuse(jit_emitter& e, jit_block& b, u_word pc, u_word count) {
    if (!jit_anchor_ok) { return false; }
    u_byte op = jg8(pc);
    u_byte base = op & 0x3F;
    u_byte mode = op & opcode_flag;

    bool is_unary = (op == instr::inc_opcode || op == instr::dec_opcode);
    bool is_row_family = base == 0x31 || base == 0x32
        || (base >= instr::setcc_base && base <= instr::setcc_base + 2)
        || (base >= instr::jcc_base && base <= instr::jcc_base + 2)
        || base == 0x27 || base == 0x29 || base == 0x24 || base == 0x22
        || base == 0x33 || base == 0x39 || base == 0x3A || base == 0x15 || base == 0x28;

    u_byte opc64 = 0;            /* host reg-reg opcode for the width op */
    bool writeback = true;
    if (!is_unary) {
        if (is_row_family) { return false; }
        if (mode != opcode_flag_srcReg && mode != opcode_flag_srcImm) { return false; }
        switch (base) {
            case instr::add_opcode:  opc64 = 0x03; break;
            case instr::sub_opcode:  opc64 = 0x2B; break;
            case instr::cmp_opcode:  opc64 = 0x3B; writeback = false; break;
            case instr::and_opcode:  opc64 = 0x23; break;
            case instr::or_opcode:   opc64 = 0x0B; break;
            case instr::xor_opcode:  opc64 = 0x33; break;
            case instr::test_opcode: opc64 = 0x85; writeback = false; break;
            default: return false;
        }
    }
    bool logic_family = (base == instr::and_opcode || base == instr::or_opcode
                      || base == instr::xor_opcode || base == instr::test_opcode) && !is_unary;

    /* Decode the ALU instruction's shape. */
    u_byte dst_desc;
    u_byte src_desc = 0;
    u_byte immsz = 0;
    u_word imm_sx = 0;
    u_word alu_len;
    if (is_unary) {
        dst_desc = jg8(pc + 1);
        alu_len = 2;
    } else if (mode == opcode_flag_srcImm) {
        immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
        imm_sx = jit_signext(jgN(pc + 3, immsz), immsz);
        dst_desc = jg8(pc + 2);
        alu_len = 3u + immsz;
    } else {
        src_desc = jg8(pc + 1);
        dst_desc = jg8(pc + 2);
        if (!jit_inl_desc(src_desc)) { return false; }
        alu_len = 3;
    }
    if (!jit_inl_desc(dst_desc)) { return false; }
    gsub d = jit_gsub(dst_desc);
    u_byte w = d.size;

    /* The Jcc that must immediately follow. */
    u_word jpc = pc + alu_len;
    if ((jpc & ~static_cast<u_word>(0xFFF)) != (pc & ~static_cast<u_word>(0xFFF))) { return false; }
    u_byte jop = jg8(jpc);
    u_byte jbase = jop & 0x3F;
    if (jbase < instr::jcc_base || jbase > instr::jcc_base + 2) { return false; }
    u_word cond_idx = static_cast<u_word>(((jop & opcode_flag) >> 6) * 3 + (jbase - instr::jcc_base));
    int cc = jit_cc_for_cond(cond_idx);
    if (cc < 0) { return false; }
    u_byte jimmsz = static_cast<u_byte>(1u << (jg8(jpc + 1) & opflag_imm_size));
    u_word target = jgN(jpc + 2, jimmsz);
    u_word fall = jpc + 2 + jimmsz;

    /* ---- Emission. Register plan: rax = src, rcx = dst_before (both dead after the
       compute), rdx = masked RF accumulator (flag-free lea/movzx assembly), r8 =
       result then the C setcc, r9 = N, r10 = V, al = Z. ---- */
    if (is_unary) {
        e.load_anchor(1, d.disp, w, false);
    } else if (mode == opcode_flag_srcImm) {
        if (static_cast<u_word>(static_cast<std::int64_t>(static_cast<std::int32_t>(imm_sx))) == imm_sx) {
            e.rex(true, 0, false, false); e.u8(0xC7); e.u8(0xC0);
            e.u32(static_cast<std::uint32_t>(imm_sx));
        } else {
            e.mov_ri(0, imm_sx);
        }
        e.load_anchor(1, d.disp, w, false);
    } else {
        gsub s = jit_gsub(src_desc);
        e.load_anchor(0, s.disp, s.size, true);
        e.load_anchor(1, d.disp, w, false);
    }
    /* Masked RF BEFORE the flag-producing compute (and/or clobber flags). */
    e.load_anchor(2, jit_disp(&regs::rf.w0), 8, false);
    e.and_ri32(2, static_cast<std::int32_t>(~(bit_carryout | bit_negative | bit_overflow | bit_zero)));
    /* The width-sized compute (host flags = Maize flags). */
    if (is_unary) {
        e.mov_rr(8, 1);
        e.addsub_ri8_w(op == instr::inc_opcode ? 0 : 5, w, 8, 1);
        e.store_anchor(8, d.disp, w);
    } else if (writeback) {
        e.mov_rr(8, 1);
        e.alu_rr_w(opc64, w, 8, 0);
        e.store_anchor(8, d.disp, w);
    } else {
        e.alu_rr_w(opc64, w, 1, 0);
    }
    /* Materialize RF from the live host flags (setcc/movzx/lea are flag-free). */
    if (!logic_family) {
        e.setcc_r(0x2, 8);                       /* setb  r8b : C */
        e.setcc_r(0x0, 10);                      /* seto r10b : V */
    }
    e.setcc_r(0x8, 9);                           /* sets  r9b : N */
    e.setcc_r(0x4, 0);                           /* sete   al : Z */
    if (!logic_family) {
        e.movzx_rr(1, 8, 1);  e.lea_scaled(2, 2, 1, 0);   /* += C      */
        e.movzx_rr(1, 10, 1); e.lea_scaled(2, 2, 1, 2);   /* += V << 2 */
    }
    e.movzx_rr(1, 9, 1);  e.lea_scaled(2, 2, 1, 1);       /* += N << 1 */
    e.movzx_rr(1, 0, 1);                                  /* rcx = Z */
    e.lea_scaled(1, 1, 1, 0);                             /* rcx = 2Z */
    e.lea_scaled(2, 2, 1, 3);                             /* += Z << 4 */
    e.store_anchor(2, jit_disp(&regs::rf.w0), 8);
    e.store_imm8_anchor(jit_disp(&pending_flags), 0);     /* dirty = false */
    /* Branch on the SAME host flags (everything since the compute was flag-free). */
    u_byte* taken_site = e.jcc_placeholder(static_cast<u_byte>(cc));
    jit_emit_edge(e, b, fall, count + 2);                 /* fall-through */
    jit_patch_branch(taken_site, e.buf);
    jit_emit_edge(e, b, target, count + 2);               /* taken */
    return true;
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
    b->paged = jit_mode_paged();
    b->edges.reserve(2);
    jit_compiling_paged = b->paged;   /* memory templates route fault-safe when set (maize-341) */

    /* Block prologue: establish the anchor register for the inline templates. */
    if (jit_anchor_ok) { e.mov_ri(11, reinterpret_cast<u_word>(jit_anchor)); }

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

        /* ---- Fused compare-and-branch first: an ALU op immediately followed by Jcc
           becomes a width-sized host compare + native branch, ending the block. ---- */
        if (jit_try_fuse(e, *b, pc, count)) {
            g_jit.emitted_inline += 2;
            count += 2;
            break;
        }

        /* ---- Inline templates next (decision 9801): the hot straight-line shapes
           compile to direct host code; everything they decline falls through to the
           thunk paths below. ---- */
        {
            u_word inl_len = 0;
            if (jit_try_inline(e, op, pc, inl_len)) {
                g_jit.emitted_inline += 1;
                pc += inl_len;
                count += 1;
                if (e.overflow) { break; }
                continue;
            }
        }

        /* ---- Block-ending covered transfers. ---- */
        if (op == instr::jmp_immVal) {
            u_byte immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
            u_word target = jgN(pc + 2, immsz);
            jit_emit_edge(e, *b, target, count + 1);
            count += 1; break;
        }
        if (op == instr::jmp_regVal || op == instr::jmp_regAddr) {
            u_byte d = jg8(pc + 1);
            if (jit_desc_is_rp(d)) { jit_emit_edge(e, *b, pc, count); break; }
            u_word args[1] = { d };
            if (op == instr::jmp_regVal) {
                e.call_fn(reinterpret_cast<void*>(&jit_jmp_regval), 1, args);   /* reg -> rp, no memory */
            } else {
                jit_emit_mem_thunk(e, reinterpret_cast<void*>(&jit_jmp_regaddr), 1, args, pc, b->paged);
            }
            jit_emit_indirect_exit(e, count + 1);
            count += 1; break;
        }
        if (op == instr::jmp_immAddr) {
            u_byte immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
            u_word addr_lit = jgN(pc + 2, immsz);
            u_word args[1] = { addr_lit };
            jit_emit_mem_thunk(e, reinterpret_cast<void*>(&jit_jmp_immaddr), 1, args, pc, b->paged);
            jit_emit_indirect_exit(e, count + 1);
            count += 1; break;
        }
        if (op == instr::call_regVal || op == instr::call_regAddr) {
            u_byte d = jg8(pc + 1);
            if (jit_desc_is_rp(d)) { jit_emit_edge(e, *b, pc, count); break; }
            u_word ret_addr = pc + 2;
            u_word args[2] = { d, ret_addr };
            jit_emit_mem_thunk(e, op == instr::call_regVal ? reinterpret_cast<void*>(&jit_call_regval)
                                                           : reinterpret_cast<void*>(&jit_call_regaddr),
                               2, args, pc, b->paged);
            jit_emit_indirect_exit(e, count + 1);
            count += 1; break;
        }
        if (op == instr::call_immAddr) {
            u_byte immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
            u_word addr_lit = jgN(pc + 2, immsz);
            u_word ret_addr = pc + 2 + immsz;
            u_word args[2] = { addr_lit, ret_addr };
            jit_emit_mem_thunk(e, reinterpret_cast<void*>(&jit_call_immaddr), 2, args, pc, b->paged);
            jit_emit_indirect_exit(e, count + 1);
            count += 1; break;
        }
        if (op == instr::call_immVal) {
            u_byte immsz = static_cast<u_byte>(1u << (jg8(pc + 1) & opflag_imm_size));
            u_word target = jgN(pc + 2, immsz);
            u_word ret_addr = pc + 2 + immsz;
            if (b->paged) {
                /* Paged: the push can fault; route through the fault-safe path. */
                u_word args[1] = { ret_addr };
                jit_emit_mem_thunk(e, reinterpret_cast<void*>(&jit_call_push), 1, args, pc, true);
            } else if (jit_anchor_ok) {
                /* Inline the return-address push (the L1-hit stack store), mirroring
                   jit_call_push's fault-atomic order: RS commits after the store. */
                std::vector<u_byte*> slow;
                e.load_anchor(0, jit_disp(&regs::rs.w0), 8, false);
                e.lea_disp8(0, 0, -8);                     /* rax = rs - 8 */
                e.mov_rr(9, 0);
                e.shift_ri(5, 9, 12);
                e.and_ri32_32(9, 0xFFFFF);
                e.cmp8_anchor_idx1(9, jit_disp(jit_page_bitmap), 0);
                slow.push_back(e.jcc_placeholder(0x5));
                if (jit_check_mode) {
                    e.cmp64_anchor_imm8(jit_disp(&jit_journal), 0);
                    slow.push_back(e.jcc_placeholder(0x5));
                }
                jit_emit_l1_probe(e, 0, 8, access_kind::store, slow);   /* bare-only (paged uses the fault-safe thunk above) */
                e.mov_ri(1, ret_addr);
                e.store_mem(1, 8, 2, 8);
                e.store_anchor(0, jit_disp(&regs::rs.w0), 8);
                u_byte* done = e.jump_placeholder();
                u_byte* slow_lbl = e.buf;
                for (u_byte* s : slow) { jit_patch_branch(s, slow_lbl); }
                {
                    u_word args[1] = { ret_addr };
                    jit_call_thunk(e, reinterpret_cast<void*>(&jit_call_push), 1, args);
                }
                jit_patch_branch(done, e.buf);
            } else {
                u_word args[1] = { ret_addr };
                e.call_fn(reinterpret_cast<void*>(&jit_call_push), 1, args);
            }
            jit_emit_edge(e, *b, target, count + 1);
            count += 1; break;
        }
        if (op == instr::ret_opcode) {
            if (b->paged) {
                /* Paged: the pop can fault; route through the fault-safe path. */
                jit_emit_mem_thunk(e, reinterpret_cast<void*>(&jit_ret_pop), 0, nullptr, pc, true);
            } else if (jit_anchor_ok) {
                /* Inline the return-address pop (the L1-hit stack load). */
                std::vector<u_byte*> slow;
                e.load_anchor(0, jit_disp(&regs::rs.w0), 8, false);
                jit_emit_l1_probe(e, 0, 8, access_kind::load, slow);   /* bare-only (paged uses the fault-safe thunk above) */
                e.load_mem(1, 8, 2, 8, false);
                e.store_anchor(1, jit_disp(&regs::rp.w0), 8);
                e.load_anchor(0, jit_disp(&regs::rs.w0), 8, false);
                e.lea_disp8(0, 0, 8);
                e.store_anchor(0, jit_disp(&regs::rs.w0), 8);
                u_byte* done = e.jump_placeholder();
                u_byte* slow_lbl = e.buf;
                for (u_byte* s : slow) { jit_patch_branch(s, slow_lbl); }
                jit_call_thunk(e, reinterpret_cast<void*>(&jit_ret_pop), 0, nullptr);
                jit_patch_branch(done, e.buf);
            } else {
                e.call_fn(reinterpret_cast<void*>(&jit_ret_pop), 0, nullptr);
            }
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
            g_jit.uncovered_end[op] += 1;
            jit_emit_edge(e, *b, pc, count);
            break;
        }
        if (thunk != nullptr) {
            if (b->paged && jit_thunk_is_mem(thunk)) {
                jit_emit_mem_thunk(e, thunk, nargs, a, pc, true);
            } else {
                jit_call_thunk(e, thunk, nargs, a);
            }
        }
        g_jit.emitted_thunk += 1;
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
    jit_page_bitmap[jit_bitmap_slot(b->page)] = 1;
    /* Insert into the indirect-transfer probe. maize-346: paged blocks are now probed too
       (the block key is physical-keyed and folds in the paged bit, so an SATP change cannot
       make a hit point at the wrong block; jit_invalidate_page already evicts paged entries
       by physical page). The paged probe tail translates the target VA->PA at every exit and
       matches this physical key. Check mode still skips the probe (every check run is one
       block). Paged blocks also chain their same-guest-page direct edges (maize-341). */
    if (!jit_check_mode) { jit_probe_insert(b->key, reinterpret_cast<const void*>(b->code)); }
    g_jit.blocks_compiled += 1;

    /* Chaining (skipped wholesale in check mode: every check run must be exactly one
       block so the oracle window matches). Paged blocks only ever pushed same-page edges,
       so the pass runs for them too. */
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
        jit_probe_remove(key);
        delete b;
    }
    /* Clear the bitmap slot unless another live code page hashes to it. */
    {
        bool shared = false;
        for (auto const& kv : g_jit.code_pages) {
            if (jit_bitmap_slot(kv.first) == jit_bitmap_slot(pageno)) { shared = true; break; }
        }
        if (!shared) { jit_page_bitmap[jit_bitmap_slot(pageno)] = 0; }
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
#if MAIZE_JIT_BACKEND
    std::memset(jit_page_bitmap, 0, sizeof jit_page_bitmap);
    jit_probe_reset();
#endif
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

        u_word pc = regs::rp.w0;
        /* maize-341: set the fault PC BEFORE the key computation, whose fetch-translate can
           itself page-fault under Sv48; a block-entry fetch fault then reports this PC.
           jit_block_key / jit_compile run in pure C++ frames (this is inside tick() inside
           run()), so run()'s existing catch(page_fault_redirect) covers a fault raised here. */
        current_instr_pc = pc;
        u_word key = jit_block_key(pc);
        auto it = g_jit.table.find(key);
        jit_block* b = (it != g_jit.table.end()) ? it->second : nullptr;

        if (b == nullptr) {
            u_word& c = g_jit.counter[key];
            if (c == JIT_NEVER_COMPILE) { g_jit.dispatch_miss += 1; ++g_jit.miss_pcs[pc]; return; }
            if (++c < jit_hotness_threshold) { g_jit.dispatch_miss += 1; ++g_jit.miss_pcs[pc]; return; }
            if (g_jit.arena.remaining() < 64 * 1024) { jit_flush_all(); }
            b = jit_compile(pc);
            if (b == nullptr) {
                if (jit_last_compile_uncoverable) { g_jit.counter[key] = JIT_NEVER_COMPILE; }
                g_jit.dispatch_miss += 1;
                return;
            }
        }
        g_jit.dispatch_hit += 1;
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
            jit_pending_fault = false;
            b->code();
            bool jit_faulted = jit_pending_fault;   /* a compiled memory op page-faulted (maize-341) */
            jit_pending_fault = false;
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
            /* The oracle can page-fault at the same instruction the compiled block did;
               catch it here so the throw does not unwind past this differential frame
               (maize-341). deliver_vectored has already run, so the post-fault globals are
               the handler-entry state, which must match the compiled tier's. */
            bool int_faulted = false;
            try {
                tick();
            } catch (page_fault_redirect&) {
                int_faulted = true;
            }
            jit_active_dispatch = saved_dispatch;
            jit_step_on = false;
            jit_journal = nullptr;
            materialize_flags();
            arch_snapshot int_end; jit_snapshot(int_end);

            if (jit_faulted != int_faulted) {
                std::cerr << "maize: JIT MISCOMPILE block 0x" << std::hex << pc << std::dec
                          << ": fault divergence (jit " << (jit_faulted ? "faulted" : "did not")
                          << ", interp " << (int_faulted ? "faulted" : "did not") << ")" << std::endl;
                std::cerr << "maize: aborting on JIT miscompile (--jit-check)" << std::endl;
                std::exit(2);
            }
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
        jit_pending_fault = false;
        b->code();
        if (jit_pending_fault) {
            /* A compiled memory op page-faulted; the fault is already fully delivered (RP
               at the handler, trap frame pushed, flags materialized into the saved RF), so
               hand back to the interpreter to run the handler. The block stopped at the
               faulting instruction. */
            jit_pending_fault = false;
            return;
        }
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
    /* Anchor for the inline templates: every piece of VM state a template touches must
       sit within disp32 of &regs::r0.w0 (always true for module globals). If anything
       falls outside, the inline tier stands down and the thunk tier runs alone. */
    jit_anchor = reinterpret_cast<u_byte*>(&regs::r0.w0);
    jit_anchor_ok = true;
    {
        auto jchk = [](const void* p) { if (!jit_disp_fits(p)) { jit_anchor_ok = false; } };
        for (int i = 0; i < 16; ++i) { jchk(&reg_map[i]->w0); }
        jchk(&pending_flags);
        jchk(&pending_flags.result);
        jchk(mm.jit_l1_ptr_data());
        jchk(mm.jit_l1_ptr_data() + memory_module::jit_l1_mask());
        jchk(mm.jit_l1_base_data());
        jchk(mm.jit_l1_base_data() + memory_module::jit_l1_mask());
        jchk(jit_page_bitmap);
        jchk(jit_page_bitmap + (std::size_t(1) << 20) - 1);
        jchk(&fast_pages[0].key);            /* maize-341: inline fast-page translate */
        jchk(&fast_pages[2].pa_page);
        jchk(&regs::rf.w0);
        jchk(jit_probe_keys);
        jchk(jit_probe_keys + (std::size_t(1) << JIT_PROBE_BITS) - 1);
        jchk(jit_probe_code);
        jchk(jit_probe_code + (std::size_t(1) << JIT_PROBE_BITS) - 1);
        jchk(&jit_probe_pa);                 /* maize-346: paged probe tail reads it anchor-relative */
        jchk(&jit_journal);
    }
    if (!jit_anchor_ok) {
        std::cerr << "maize: JIT inline templates disabled (VM state exceeds the anchor's"
                     " 2 GiB reach); running the call-threaded tier" << std::endl;
    }
    jit_probe_reset();
#if defined(_WIN32)
    if (GetEnvironmentVariableA("MAIZE_JIT_MISCOMPILE", nullptr, 0) > 0) { jit_inject_miscompile = true; }
    if (GetEnvironmentVariableA("MAIZE_JIT_NOPROBE", nullptr, 0) > 0) { jit_no_probe = true; }
    if (GetEnvironmentVariableA("MAIZE_JIT_INLINE_EVENTS", nullptr, 0) > 0) { jit_no_inline_events = false; }
#else
    if (std::getenv("MAIZE_JIT_MISCOMPILE") != nullptr) { jit_inject_miscompile = true; }
    if (std::getenv("MAIZE_JIT_NOPROBE") != nullptr) { jit_no_probe = true; }
    if (std::getenv("MAIZE_JIT_INLINE_EVENTS") != nullptr) { jit_no_inline_events = false; }
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
    out << "  emission mix        : " << g_jit.emitted_inline << " inline / "
        << g_jit.emitted_thunk << " thunk (static)\n";
    out << "  dispatcher          : " << g_jit.dispatch_hit << " hits / "
        << g_jit.dispatch_miss << " misses\n";
    {
        /* Diagnostic: the pcs where dispatcher misses concentrate. */
        std::vector<std::pair<std::uint64_t, u_word>> tops;
        for (auto const& kv : g_jit.miss_pcs) { tops.emplace_back(kv.second, kv.first); }
        std::sort(tops.begin(), tops.end(), std::greater<>());
        for (std::size_t i = 0; i < tops.size() && i < 8; ++i) {
            out << "  miss pc             : 0x" << std::hex << tops[i].second << std::dec
                << " x" << tops[i].first << "\n";
        }
    }
    {
        /* Top block-ending uncovered opcodes (static): what fragments the blocks. */
        for (int rank = 0; rank < 6; ++rank) {
            int best = -1;
            std::uint64_t bc = 0;
            for (int i = 0; i < 256; ++i) {
                if (g_jit.uncovered_end[i] > bc) { bc = g_jit.uncovered_end[i]; best = i; }
            }
            if (best < 0 || bc == 0) { break; }
            out << "  uncovered ender     : opcode 0x" << std::hex << best << std::dec
                << " x" << bc << "\n";
            g_jit.uncovered_end[best] = 0;
        }
    }
    if (perf_count_enabled && perf_insn_count > 0) {
        out << "  covered fraction    : "
            << (g_jit.covered_instr * 1000 / perf_insn_count) / 10.0
            << "% of executed instructions\n";
    }
    if (g_jit.check_blocks > 0) {
        out << "  differential checks : " << g_jit.check_blocks << " block executions verified\n";
    }
}
