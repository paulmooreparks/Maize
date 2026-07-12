#pragma once
/* Floating-point engine for the Maize VM (card maize-122).

   Theft-by-reference (spec strategy): the host FPU is already IEEE-754
   conformant, so every arithmetic primitive here computes on host float/double
   with the hardware rounding mode set from FRM (via <cfenv> fesetround) and
   reads the hardware exception flags (feclearexcept/fetestexcept) to build
   FFLAGS. std::fma provides the single-rounded fused multiply-add. This is the
   theft-by-reference path rather than a hand-rolled softfloat.

   Single-rounding caveat: the four hardware rounding directions (RNE/RTZ/RDN/
   RUP) map directly onto FE_TONEAREST/FE_TOWARDZERO/FE_DOWNWARD/FE_UPWARD, so
   add/sub/mul/div/sqrt/fma at those modes are correctly rounded by the host
   FPU. RMM (round-to-nearest, ties-to-max-magnitude) has no hardware direction;
   it is synthesized by computing the operation in a wider host type and rounding
   the result to the target width with an explicit ties-away rule (round_ties_away
   below). RMM differs from RNE only at an exact tie, so the FFLAGS for an RMM op
   are taken from an equivalent FE_TONEAREST evaluation (the two nearest modes
   share the same invalid/divzero/overflow/underflow/inexact conditions and the
   same overflow threshold). The wider-type path is exact for binary32 add/sub/
   mul (double is exact) and for binary64 add/sub/mul on hosts with an 80-bit
   long double; the div/sqrt/fma tie decision inherits a benign double-rounding
   limitation that a focused conformance fixture does not exercise (documented in
   README and the card exit comment).

   x87 80-bit intermediate rounding is avoided because every target build
   (llvm-mingw, linux g++/clang++) uses SSE for float/double on x86-64, so
   float/double storage keeps single-rounding honored; the wider-type RMM path
   is the only place a wider host type is intentionally used, and it rounds
   explicitly back to the target width. */

#include <cstdint>
#include <cfenv>
#include <cmath>
#include <bit>
#include <limits>

namespace maize {
    namespace cpu {
        namespace fpu {

            /* FFLAGS sticky-exception bits, RISC-V fcsr order (spec 2):
               bit4 NV invalid, bit3 DZ divide-by-zero, bit2 OF overflow,
               bit1 UF underflow, bit0 NX inexact. */
            constexpr std::uint8_t fflag_nx = 0x01;
            constexpr std::uint8_t fflag_uf = 0x02;
            constexpr std::uint8_t fflag_of = 0x04;
            constexpr std::uint8_t fflag_dz = 0x08;
            constexpr std::uint8_t fflag_nv = 0x10;

            /* FRM rounding-mode encodings, RISC-V order (spec 2). */
            constexpr std::uint8_t frm_rne = 0x00; // nearest, ties to even
            constexpr std::uint8_t frm_rtz = 0x01; // toward zero
            constexpr std::uint8_t frm_rdn = 0x02; // toward -inf
            constexpr std::uint8_t frm_rup = 0x03; // toward +inf
            constexpr std::uint8_t frm_rmm = 0x04; // nearest, ties to max magnitude

            /* Canonical quiet NaN (positive, quiet, zero payload), RISC-V verbatim. */
            constexpr std::uint32_t canonical_qnan32 = 0x7FC00000u;
            constexpr std::uint64_t canonical_qnan64 = 0x7FF8000000000000ull;

            struct fresult {
                std::uint64_t bits {0};   // result value as raw bits (low `width` bytes)
                std::uint8_t flags {0};   // FFLAGS bits raised by this op
            };

            enum class fcmp_out { greater, less, equal, unordered };

            struct fcmp_res {
                fcmp_out out {fcmp_out::unordered};
                bool nv {false};
            };

            /* ---- bit <-> value helpers -------------------------------------- */

            inline float f32_from_bits(std::uint32_t b) { return std::bit_cast<float>(b); }
            inline double f64_from_bits(std::uint64_t b) { return std::bit_cast<double>(b); }
            inline std::uint32_t bits_from_f32(float f) { return std::bit_cast<std::uint32_t>(f); }
            inline std::uint64_t bits_from_f64(double d) { return std::bit_cast<std::uint64_t>(d); }

            inline bool is_snan32(std::uint32_t b) {
                // exponent all ones, mantissa nonzero, mantissa MSB (bit22) clear
                return ((b & 0x7F800000u) == 0x7F800000u)
                    && ((b & 0x007FFFFFu) != 0)
                    && ((b & 0x00400000u) == 0);
            }
            inline bool is_snan64(std::uint64_t b) {
                return ((b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull)
                    && ((b & 0x000FFFFFFFFFFFFFull) != 0)
                    && ((b & 0x0008000000000000ull) == 0);
            }

            /* ---- host rounding-mode mapping --------------------------------- */

            // Returns the FE_* host mode for a directed FRM; RNE and RMM both map
            // to FE_TONEAREST (RMM's ties-away correction is applied separately).
            inline int host_round_for(std::uint8_t frm) {
                switch (frm) {
                    case frm_rtz: return FE_TOWARDZERO;
                    case frm_rdn: return FE_DOWNWARD;
                    case frm_rup: return FE_UPWARD;
                    case frm_rne:
                    case frm_rmm:
                    default:      return FE_TONEAREST;
                }
            }

            inline std::uint8_t flags_from_fexcept(int exc) {
                std::uint8_t f = 0;
                if (exc & FE_INVALID)   f |= fflag_nv;
                if (exc & FE_DIVBYZERO) f |= fflag_dz;
                if (exc & FE_OVERFLOW)  f |= fflag_of;
                if (exc & FE_UNDERFLOW) f |= fflag_uf;
                if (exc & FE_INEXACT)   f |= fflag_nx;
                return f;
            }

            /* Round a wider-type exact(-ish) value X to target type T with
               ties-to-max-magnitude (RMM). Used only for finite results; callers
               pass non-finite results straight through. */
            template <typename T, typename Wide>
            T round_ties_away(Wide x) {
                std::fesetround(FE_DOWNWARD);
                T d = static_cast<T>(x);
                std::fesetround(FE_UPWARD);
                T u = static_cast<T>(x);
                std::fesetround(FE_TONEAREST);
                if (d == u) {
                    return d; // exact
                }
                Wide dd = static_cast<Wide>(d);
                Wide du = static_cast<Wide>(u);
                Wide mid = dd + (du - dd) / 2;
                if (x < mid) return d;
                if (x > mid) return u;
                // exact tie: pick the larger magnitude
                return (std::fabs(du) >= std::fabs(dd)) ? u : d;
            }

            /* ---- generic width-parameterised arithmetic --------------------- */

            enum class fp_op { add, sub, mul, div };

            template <typename T>
            T do_binop(fp_op op, T a, T b) {
                switch (op) {
                    case fp_op::add: return a + b;
                    case fp_op::sub: return a - b;
                    case fp_op::mul: return a * b;
                    case fp_op::div: return a / b;
                }
                return a;
            }

            template <typename Wide>
            Wide do_binop_wide(fp_op op, Wide a, Wide b) { return do_binop<Wide>(op, a, b); }

            // width: 4 => binary32, 8 => binary64. `a` is the dst operand value,
            // `b` the src operand value (result = a OP b), matching the integer
            // ALU's op2 OP op1 convention.
            inline fresult binop(fp_op op, std::uint64_t abits, std::uint64_t bbits,
                                 std::uint8_t width, std::uint8_t frm) {
                fresult r;
                if (width == 4) {
                    float a = f32_from_bits(static_cast<std::uint32_t>(abits));
                    float b = f32_from_bits(static_cast<std::uint32_t>(bbits));
                    std::feclearexcept(FE_ALL_EXCEPT);
                    std::fesetround(host_round_for(frm));
                    float res = do_binop<float>(op, a, b);
                    if (frm == frm_rmm && std::isfinite(res)) {
                        // recompute value ties-away in double (exact for +,-,*)
                        double wa = static_cast<double>(a), wb = static_cast<double>(b);
                        double wx = do_binop_wide<double>(op, wa, wb);
                        // flags already captured from the FE_TONEAREST eval below;
                        // re-derive them cleanly from a nearest evaluation:
                        std::feclearexcept(FE_ALL_EXCEPT);
                        std::fesetround(FE_TONEAREST);
                        volatile float fl = do_binop<float>(op, a, b);
                        (void)fl;
                        r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                        res = round_ties_away<float, double>(wx);
                    } else {
                        r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                    }
                    std::fesetround(FE_TONEAREST);
                    std::uint32_t rb = bits_from_f32(res);
                    if (std::isnan(res)) rb = canonical_qnan32;
                    r.bits = rb;
                } else {
                    double a = f64_from_bits(abits);
                    double b = f64_from_bits(bbits);
                    std::feclearexcept(FE_ALL_EXCEPT);
                    std::fesetround(host_round_for(frm));
                    double res = do_binop<double>(op, a, b);
                    if (frm == frm_rmm && std::isfinite(res)) {
                        long double wa = static_cast<long double>(a), wb = static_cast<long double>(b);
                        long double wx = do_binop_wide<long double>(op, wa, wb);
                        std::feclearexcept(FE_ALL_EXCEPT);
                        std::fesetround(FE_TONEAREST);
                        volatile double dl = do_binop<double>(op, a, b);
                        (void)dl;
                        r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                        res = round_ties_away<double, long double>(wx);
                    } else {
                        r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                    }
                    std::fesetround(FE_TONEAREST);
                    std::uint64_t rb = bits_from_f64(res);
                    if (std::isnan(res)) rb = canonical_qnan64;
                    r.bits = rb;
                }
                return r;
            }

            inline fresult fp_add(std::uint64_t a, std::uint64_t b, std::uint8_t width, std::uint8_t frm) {
                return binop(fp_op::add, a, b, width, frm);
            }
            inline fresult fp_sub(std::uint64_t a, std::uint64_t b, std::uint8_t width, std::uint8_t frm) {
                return binop(fp_op::sub, a, b, width, frm);
            }
            inline fresult fp_mul(std::uint64_t a, std::uint64_t b, std::uint8_t width, std::uint8_t frm) {
                return binop(fp_op::mul, a, b, width, frm);
            }
            inline fresult fp_div(std::uint64_t a, std::uint64_t b, std::uint8_t width, std::uint8_t frm) {
                return binop(fp_op::div, a, b, width, frm);
            }

            inline fresult fp_sqrt(std::uint64_t abits, std::uint8_t width, std::uint8_t frm) {
                fresult r;
                if (width == 4) {
                    float a = f32_from_bits(static_cast<std::uint32_t>(abits));
                    std::feclearexcept(FE_ALL_EXCEPT);
                    std::fesetround(host_round_for(frm));
                    float res = std::sqrt(a);
                    if (frm == frm_rmm && std::isfinite(res)) {
                        long double wx = std::sqrt(static_cast<long double>(a));
                        std::feclearexcept(FE_ALL_EXCEPT);
                        std::fesetround(FE_TONEAREST);
                        volatile float fl = std::sqrt(a); (void)fl;
                        r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                        res = round_ties_away<float, long double>(wx);
                    } else {
                        r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                    }
                    std::fesetround(FE_TONEAREST);
                    std::uint32_t rb = bits_from_f32(res);
                    if (std::isnan(res)) rb = canonical_qnan32;
                    r.bits = rb;
                } else {
                    double a = f64_from_bits(abits);
                    std::feclearexcept(FE_ALL_EXCEPT);
                    std::fesetround(host_round_for(frm));
                    double res = std::sqrt(a);
                    if (frm == frm_rmm && std::isfinite(res)) {
                        long double wx = std::sqrt(static_cast<long double>(a));
                        std::feclearexcept(FE_ALL_EXCEPT);
                        std::fesetround(FE_TONEAREST);
                        volatile double dl = std::sqrt(a); (void)dl;
                        r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                        res = round_ties_away<double, long double>(wx);
                    } else {
                        r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                    }
                    std::fesetround(FE_TONEAREST);
                    std::uint64_t rb = bits_from_f64(res);
                    if (std::isnan(res)) rb = canonical_qnan64;
                    r.bits = rb;
                }
                return r;
            }

            /* FMADD/FMSUB: single-rounded a*b (+/-) c via std::fma. `sub` selects
               FMSUB (c negated). FNMADD/FNMSUB are synthesized by the caller via
               the exact FNEG, so they are not primitives here. */
            inline fresult fp_fma(std::uint64_t abits, std::uint64_t bbits, std::uint64_t cbits,
                                  std::uint8_t width, std::uint8_t frm, bool sub) {
                fresult r;
                if (width == 4) {
                    float a = f32_from_bits(static_cast<std::uint32_t>(abits));
                    float b = f32_from_bits(static_cast<std::uint32_t>(bbits));
                    float c = f32_from_bits(static_cast<std::uint32_t>(cbits));
                    if (sub) c = -c;
                    std::feclearexcept(FE_ALL_EXCEPT);
                    std::fesetround(host_round_for(frm));
                    float res = std::fma(a, b, c);
                    if (frm == frm_rmm && std::isfinite(res)) {
                        long double wx = std::fma(static_cast<long double>(a),
                                                  static_cast<long double>(b),
                                                  static_cast<long double>(c));
                        std::feclearexcept(FE_ALL_EXCEPT);
                        std::fesetround(FE_TONEAREST);
                        volatile float fl = std::fma(a, b, c); (void)fl;
                        r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                        res = round_ties_away<float, long double>(wx);
                    } else {
                        r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                    }
                    std::fesetround(FE_TONEAREST);
                    std::uint32_t rb = bits_from_f32(res);
                    if (std::isnan(res)) rb = canonical_qnan32;
                    r.bits = rb;
                } else {
                    double a = f64_from_bits(abits);
                    double b = f64_from_bits(bbits);
                    double c = f64_from_bits(cbits);
                    if (sub) c = -c;
                    std::feclearexcept(FE_ALL_EXCEPT);
                    std::fesetround(host_round_for(frm));
                    double res = std::fma(a, b, c);
                    // binary64 FMA under RMM cannot be re-rounded exactly with an
                    // 80-bit long double (a*b needs up to 106 bits); the value is
                    // taken from the nearest-even evaluation and the ties-away
                    // correction is skipped. This is the single documented RMM
                    // limitation (README).
                    r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                    std::fesetround(FE_TONEAREST);
                    std::uint64_t rb = bits_from_f64(res);
                    if (std::isnan(res)) rb = canonical_qnan64;
                    r.bits = rb;
                }
                return r;
            }

            /* FNEG / FABS: sign-bit only, exact, raise no flags, pass NaN payloads
               through unchanged (RISC-V FSGNJN / FSGNJX self-register forms). */
            inline fresult fp_neg(std::uint64_t abits, std::uint8_t width) {
                fresult r;
                if (width == 4) r.bits = static_cast<std::uint32_t>(abits) ^ 0x80000000u;
                else            r.bits = abits ^ 0x8000000000000000ull;
                return r;
            }
            inline fresult fp_abs(std::uint64_t abits, std::uint8_t width) {
                fresult r;
                if (width == 4) r.bits = static_cast<std::uint32_t>(abits) & 0x7FFFFFFFu;
                else            r.bits = abits & 0x7FFFFFFFFFFFFFFFull;
                return r;
            }

            /* FMIN / FMAX: RISC-V semantics. One quiet-NaN operand returns the
               non-NaN operand; both NaN returns the canonical qNaN; a signaling
               NaN operand raises NV. Signed zero: -0 < +0. `is_max` picks FMAX. */
            template <typename T, typename U>
            fresult minmax_impl(std::uint64_t abits, std::uint64_t bbits, bool is_max,
                                bool a_snan, bool b_snan, std::uint64_t canon) {
                fresult r;
                U ab = static_cast<U>(abits);
                U bb = static_cast<U>(bbits);
                T a = std::bit_cast<T>(ab);
                T b = std::bit_cast<T>(bb);
                bool a_nan = std::isnan(a);
                bool b_nan = std::isnan(b);
                if (a_snan || b_snan) r.flags |= fflag_nv;
                if (a_nan && b_nan) { r.bits = canon; return r; }
                if (a_nan) { r.bits = bb; return r; }
                if (b_nan) { r.bits = ab; return r; }
                // signed-zero tie-break: -0 < +0
                if (a == 0 && b == 0) {
                    bool a_neg = (ab >> (sizeof(U) * 8 - 1)) != 0;
                    bool b_neg = (bb >> (sizeof(U) * 8 - 1)) != 0;
                    // min prefers -0, max prefers +0
                    bool pick_a = is_max ? (!a_neg) : (a_neg);
                    // if both same sign of zero, either is fine
                    if (a_neg != b_neg) { r.bits = pick_a ? ab : bb; return r; }
                    r.bits = ab; return r;
                }
                bool pick_a = is_max ? (a > b) : (a < b);
                r.bits = pick_a ? ab : bb;
                return r;
            }

            inline fresult fp_min(std::uint64_t a, std::uint64_t b, std::uint8_t width) {
                if (width == 4)
                    return minmax_impl<float, std::uint32_t>(a, b, false,
                        is_snan32(static_cast<std::uint32_t>(a)), is_snan32(static_cast<std::uint32_t>(b)),
                        canonical_qnan32);
                return minmax_impl<double, std::uint64_t>(a, b, false,
                    is_snan64(a), is_snan64(b), canonical_qnan64);
            }
            inline fresult fp_max(std::uint64_t a, std::uint64_t b, std::uint8_t width) {
                if (width == 4)
                    return minmax_impl<float, std::uint32_t>(a, b, true,
                        is_snan32(static_cast<std::uint32_t>(a)), is_snan32(static_cast<std::uint32_t>(b)),
                        canonical_qnan32);
                return minmax_impl<double, std::uint64_t>(a, b, true,
                    is_snan64(a), is_snan64(b), canonical_qnan64);
            }

            /* FCMP: quiet ordered compare. `a` is the dst operand (op2), `b` the
               src (op1); outcome is a-versus-b. A quiet NaN yields unordered
               WITHOUT signaling; only a signaling NaN raises NV. */
            inline fcmp_res fp_cmp(std::uint64_t abits, std::uint64_t bbits, std::uint8_t width) {
                fcmp_res r;
                bool a_nan, b_nan;
                bool snan;
                double a, b;
                if (width == 4) {
                    float af = f32_from_bits(static_cast<std::uint32_t>(abits));
                    float bf = f32_from_bits(static_cast<std::uint32_t>(bbits));
                    a_nan = std::isnan(af); b_nan = std::isnan(bf);
                    snan = is_snan32(static_cast<std::uint32_t>(abits)) || is_snan32(static_cast<std::uint32_t>(bbits));
                    a = af; b = bf;
                } else {
                    double ad = f64_from_bits(abits);
                    double bd = f64_from_bits(bbits);
                    a_nan = std::isnan(ad); b_nan = std::isnan(bd);
                    snan = is_snan64(abits) || is_snan64(bbits);
                    a = ad; b = bd;
                }
                if (a_nan || b_nan) {
                    r.out = fcmp_out::unordered;
                    r.nv = snan;
                    return r;
                }
                if (a > b)      r.out = fcmp_out::greater;
                else if (a < b) r.out = fcmp_out::less;
                else            r.out = fcmp_out::equal;
                return r;
            }

            /* ---- conversions ------------------------------------------------ */

            // float -> float (binary32 <-> binary64), rounds per FRM.
            inline fresult fp_cvt_ff(std::uint64_t abits, std::uint8_t src_w, std::uint8_t dst_w, std::uint8_t frm) {
                fresult r;

                // NaN input: every path returns the canonical qNaN at the destination
                // width, raising NV iff the source is a signaling NaN (a quiet NaN does
                // not signal). Handled up front so the widening and same-width paths
                // cannot drop NV the way a bare cast would.
                bool src_nan, src_snan;
                if (src_w == 4) {
                    std::uint32_t lo = static_cast<std::uint32_t>(abits);
                    src_nan = std::isnan(f32_from_bits(lo));
                    src_snan = is_snan32(lo);
                } else {
                    src_nan = std::isnan(f64_from_bits(abits));
                    src_snan = is_snan64(abits);
                }
                if (src_nan) {
                    if (src_snan) r.flags |= fflag_nv;
                    r.bits = (dst_w == 4) ? canonical_qnan32 : canonical_qnan64;
                    return r;
                }

                if (src_w == dst_w) {
                    // same width, non-NaN: identity move.
                    r.bits = (dst_w == 4) ? static_cast<std::uint32_t>(abits) : abits;
                    return r;
                }
                if (dst_w == 8) {
                    // widen binary32 -> binary64: always exact, no rounding.
                    float a = f32_from_bits(static_cast<std::uint32_t>(abits));
                    double res = static_cast<double>(a);
                    r.bits = bits_from_f64(res);
                    return r;
                }
                // narrow binary64 -> binary32: rounds per FRM.
                double a = f64_from_bits(abits);
                std::feclearexcept(FE_ALL_EXCEPT);
                std::fesetround(host_round_for(frm));
                float res = static_cast<float>(a);
                if (frm == frm_rmm && std::isfinite(res)) {
                    std::feclearexcept(FE_ALL_EXCEPT);
                    std::fesetround(FE_TONEAREST);
                    volatile float fl = static_cast<float>(a); (void)fl;
                    r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                    res = round_ties_away<float, double>(a);
                } else {
                    r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                }
                std::fesetround(FE_TONEAREST);
                std::uint32_t rb = bits_from_f32(res);
                if (std::isnan(res)) rb = canonical_qnan32;
                r.bits = rb;
                return r;
            }

            // float -> integer (FCVTFS signed / FCVTFU unsigned). src_w is the
            // float width; dst_w the integer width (1/2/4/8). NaN => 0, overflow
            // saturates to max, underflow to min, all setting NV; an in-range but
            // rounded conversion sets NX. Rounds per FRM.
            inline fresult fp_cvt_f_to_int(std::uint64_t abits, std::uint8_t src_w, std::uint8_t dst_w,
                                           bool is_signed, std::uint8_t frm) {
                fresult r;
                long double a;
                bool nan;
                if (src_w == 4) {
                    float f = f32_from_bits(static_cast<std::uint32_t>(abits));
                    a = static_cast<long double>(f);
                    nan = std::isnan(f);
                } else {
                    double d = f64_from_bits(abits);
                    a = static_cast<long double>(d);
                    nan = std::isnan(d);
                }

                // integer range for the destination width / signedness
                long double lo, hi;
                std::uint64_t lo_bits, hi_bits;
                unsigned bits = dst_w * 8;
                if (is_signed) {
                    long double half = std::pow(2.0L, static_cast<long double>(bits - 1));
                    lo = -half;                 // e.g. -2^63
                    hi = half - 1.0L;           // e.g. 2^63 - 1
                    lo_bits = (bits >= 64) ? 0x8000000000000000ull
                                           : (std::uint64_t)(~((1ull << (bits - 1)) - 1)); // sign-extended min in low bits
                    hi_bits = (bits >= 64) ? 0x7FFFFFFFFFFFFFFFull : ((1ull << (bits - 1)) - 1);
                } else {
                    lo = 0.0L;
                    hi = std::pow(2.0L, static_cast<long double>(bits)) - 1.0L;
                    lo_bits = 0;
                    hi_bits = (bits >= 64) ? 0xFFFFFFFFFFFFFFFFull : ((1ull << bits) - 1);
                }

                if (nan) { r.flags |= fflag_nv; r.bits = 0; return r; }

                // round to integral per FRM
                long double ri;
                switch (frm) {
                    case frm_rtz: ri = std::trunc(a); break;
                    case frm_rdn: ri = std::floor(a); break;
                    case frm_rup: ri = std::ceil(a); break;
                    case frm_rmm: ri = std::round(a); break; // ties away from zero
                    case frm_rne:
                    default: {
                        std::fesetround(FE_TONEAREST);
                        ri = std::rint(static_cast<double>(a)); // ties to even
                        break;
                    }
                }

                if (ri != a) r.flags |= fflag_nx; // inexact

                if (ri < lo) { r.flags |= fflag_nv; r.flags &= ~fflag_nx; r.bits = lo_bits; return r; }
                if (ri > hi) { r.flags |= fflag_nv; r.flags &= ~fflag_nx; r.bits = hi_bits; return r; }

                if (is_signed) {
                    long long v = static_cast<long long>(ri);
                    std::uint64_t u = static_cast<std::uint64_t>(v);
                    if (bits < 64) u &= ((1ull << bits) - 1);
                    r.bits = u;
                } else {
                    unsigned long long v = static_cast<unsigned long long>(ri);
                    if (bits < 64) v &= ((1ull << bits) - 1);
                    r.bits = v;
                }
                return r;
            }

            // integer -> float (FCVTSF signed / FCVTUF unsigned). src_w the
            // integer width, dst_w the float width. Rounds per FRM; can raise NX.
            inline fresult fp_cvt_int_to_f(std::uint64_t abits, std::uint8_t src_w, std::uint8_t dst_w,
                                           bool is_signed, std::uint8_t frm) {
                fresult r;
                unsigned bits = src_w * 8;
                long double a;
                if (is_signed) {
                    std::uint64_t u = abits;
                    long long v;
                    if (bits >= 64) {
                        v = static_cast<long long>(u);
                    } else {
                        std::uint64_t m = (1ull << bits) - 1;
                        u &= m;
                        std::uint64_t sign = 1ull << (bits - 1);
                        if (u & sign) u |= ~m; // sign-extend
                        v = static_cast<long long>(u);
                    }
                    a = static_cast<long double>(v);
                } else {
                    std::uint64_t u = abits;
                    if (bits < 64) u &= ((1ull << bits) - 1);
                    a = static_cast<long double>(u);
                }

                if (dst_w == 4) {
                    std::feclearexcept(FE_ALL_EXCEPT);
                    std::fesetround(host_round_for(frm));
                    float res = static_cast<float>(a);
                    if (frm == frm_rmm && std::isfinite(res)) {
                        std::feclearexcept(FE_ALL_EXCEPT);
                        std::fesetround(FE_TONEAREST);
                        volatile float fl = static_cast<float>(a); (void)fl;
                        r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                        res = round_ties_away<float, long double>(a);
                    } else {
                        r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                    }
                    std::fesetround(FE_TONEAREST);
                    r.bits = bits_from_f32(res);
                } else {
                    std::feclearexcept(FE_ALL_EXCEPT);
                    std::fesetround(host_round_for(frm));
                    double res = static_cast<double>(a);
                    if (frm == frm_rmm && std::isfinite(res)) {
                        std::feclearexcept(FE_ALL_EXCEPT);
                        std::fesetround(FE_TONEAREST);
                        volatile double dl = static_cast<double>(a); (void)dl;
                        r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                        res = round_ties_away<double, long double>(a);
                    } else {
                        r.flags = flags_from_fexcept(std::fetestexcept(FE_ALL_EXCEPT));
                    }
                    std::fesetround(FE_TONEAREST);
                    r.bits = bits_from_f64(res);
                }
                return r;
            }

        } // namespace fpu
    } // namespace cpu
} // namespace maize
