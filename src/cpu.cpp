#include "maize_cpu.h"
#include "maize_cpu.h"
#include "maize_sys.h"
#include "fpu.h"
#include <sstream>
#include <exception>
#include <atomic>
#include <cassert>
#include <cstring>

/* This isn't classic OOP. My primary concern is performance; readability is secondary. Readability
and maintainability are still important, though, because I want this to become something
that I can use to write any byte code implementation. */

namespace maize {
    namespace cpu {

        namespace {
                                            //       6         5         4         3         2         1         0
                                            //    3210987654321098765432109876543210987654321098765432109876543210
            const u_word bit_carryout =           0b0000000000000000000000000000000000000000000000000000000000000001;
            const u_word bit_negative =           0b0000000000000000000000000000000000000000000000000000000000000010;
            const u_word bit_overflow =           0b0000000000000000000000000000000000000000000000000000000000000100;
            const u_word bit_parity =             0b0000000000000000000000000000000000000000000000000000000000001000;
            const u_word bit_zero =               0b0000000000000000000000000000000000000000000000000000000000010000;
            const u_word bit_sign =               0b0000000000000000000000000000000000000000000000000000000000100000;
            const u_word bit_reserved =           0b0000000000000000000000000000000000000000000000000000000001000000;
            const u_word bit_privilege =          0b0000000000000000000000000000000100000000000000000000000000000000;
            const u_word bit_interrupt_enabled =  0b0000000000000000000000000000001000000000000000000000000000000000;
            const u_word bit_interrupt_set =      0b0000000000000000000000000000010000000000000000000000000000000000;
            const u_word bit_running =            0b0000000000000000000000000000100000000000000000000000000000000000;
            const u_word bit_syscall_guest =      0b0000000000000000000000000001000000000000000000000000000000000000;

            /* card maize-180 (§9 RF-write closure): the privileged RF.H1 flag bits
               (bits 32..36). README "Flags": RF.H1 holds the privilege, interrupt-
               enabled, interrupt-set, and running flags, which may only be set in
               privileged mode; the syscall-guest selector (set only by the privileged
               SETSYSG/CLRSYSG) belongs to the same protected set. A user-mode guest
               write to RF retains these bits' current values (see commit_reg_w0). */
            const u_word rf_privileged_mask =
                bit_privilege | bit_interrupt_enabled | bit_interrupt_set
                | bit_running | bit_syscall_guest;

            constexpr u_word subreg_sign_bit[] = {
                0x0000000000000080,
                0x0000000000008000,
                0x0000000000800000,
                0x0000000080000000,
                0x0000008000000000,
                0x0000800000000000,
                0x0080000000000000,
                0x8000000000000000,
                0x0000000000008000,
                0x0000000080000000,
                0x0000800000000000,
                0x8000000000000000,
                0x0000000080000000,
                0x8000000000000000,
                0x8000000000000000
            };

            constexpr u_word subreg_neg_bits[] = {
                0xFFFFFFFFFFFFFF00,
                0xFFFFFFFFFFFFFF00,
                0xFFFFFFFFFFFFFF00,
                0xFFFFFFFFFFFFFF00,
                0xFFFFFFFFFFFFFF00,
                0xFFFFFFFFFFFFFF00,
                0xFFFFFFFFFFFFFF00,
                0xFFFFFFFFFFFFFF00,
                0xFFFFFFFFFFFF0000,
                0xFFFFFFFFFFFF0000,
                0xFFFFFFFFFFFF0000,
                0xFFFFFFFFFFFF0000,
                0xFFFFFFFF00000000,
                0xFFFFFFFF00000000,
                0x0000000000000000
            };

            /* Maps the value of a subreg_enum to the size, in bytes, of the corresponding subregister.
               opflag_subreg is a 4-bit field (0x0F), so a decoded-but-undefined subreg encoding of
               15 is reachable on the VM hot path even though no assembler ever emits it. Sized to 16
               (not 15) so index 15 lands in-bounds; the trailing element is left off the initializer
               list and value-initializes to 0, exactly matching what the old
               std::unordered_map::operator[] returned on a miss for that key (card-115 review
               follow-forward: OOB read on subreg field 0x0F). */
            constexpr std::array<u_byte, 16> subreg_size_map {
                1, // b0
                1, // b1
                1, // b2
                1, // b3
                1, // b4
                1, // b5
                1, // b6
                1, // b7
                2, // q0
                2, // q1
                2, // q2
                2, // q3
                4, // h0
                4, // h1
                8  // w0
                   // [15] unused/undefined subreg encoding: value-initializes to 0 (old map's miss default)
            };

            /* Maps the value of a subreg_enum to the offset of the corresponding subregister in the
               register's 64-bit value. Sized to 16 for the same 4-bit-field reachability reason as
               subreg_size_map above; index 15 value-initializes to 0. */
            constexpr std::array<maize::u_byte, 16> subreg_offset_map {
                 0, // b0
                 8, // b1
                16, // b2
                24, // b3
                32, // b4
                40, // b5
                48, // b6
                56, // b7
                 0, // q0
                16, // q1
                32, // q2
                48, // q3
                 0, // h0
                32, // h1
                 0  // w0
                    // [15] unused/undefined subreg encoding: value-initializes to 0 (old map's miss default)
            };

            /* Sized to 16 for the same 4-bit-field reachability reason as subreg_size_map above;
               index 15 value-initializes to 0. */
            constexpr std::array<maize::u_byte, 16> subreg_index_map {
                0, // b0
                1, // b1
                2, // b2
                3, // b3
                4, // b4
                5, // b5
                6, // b6
                7, // b7
                0, // q0
                2, // q1
                4, // q2
                6, // q3
                0, // h0
                4, // h1
                0  // w0
                   // [15] unused/undefined subreg encoding: value-initializes to 0 (old map's miss default)
            };

            /* Maps an instruction's immediate byte-size to a subreg_enum for the subregister that
               will contain the immediate value. Indexed directly by the byte-size value (not by
               subreg_enum). opflag_imm_size is a 3-bit field (0x07); live call sites compute the
               byte-size as 1 << flag, so a decoded-but-undefined imm-size flag of 4..7 yields a
               byte-size of 16/32/64/128, all reachable on the VM hot path even though no assembler
               ever emits them. Sized to 129 (covers every 1 << flag value for flag in 0..7, plus the
               two dead call sites that index directly by the raw 0..7 flag) rather than 9; indices
               2,3,5..8,10..128 are left off the initializer list and value-initialize to
               subreg_enum{} = b0, exactly matching what the old std::unordered_map::operator[]
               returned on a miss for those keys (card-115 review follow-forward: OOB read on
               imm-size flags 4..7). */
            constexpr std::array<subreg_enum, 129> imm_size_subreg_map {
                subreg_enum::b0, // [0] unused
                subreg_enum::b0, // [1] 1-byte immediate size
                subreg_enum::q0, // [2] 2-byte immediate size
                subreg_enum::b0, // [3] unused
                subreg_enum::h0, // [4] 4-byte immediate size
                subreg_enum::b0, // [5] unused
                subreg_enum::b0, // [6] unused
                subreg_enum::b0, // [7] unused
                subreg_enum::w0  // [8] 8-byte immediate size
                                 // [9..128] unused/undefined (incl. 16/32/64/128 from imm-size flags
                                 // 4..7): value-initialize to subreg_enum{} = b0 (old map's miss default)
            };

            /* Maps a subreg_enum value to a mask for the value of that register. Sized to 16 for
               the same 4-bit-field reachability reason as subreg_size_map above; index 15
               value-initializes to subreg_mask_enum{} (numeric 0, no named enumerator holds this
               value), exactly matching the old map's miss default (NOT subreg_mask_enum::b0,
               which is 0xFF and would be the wrong default). */
            constexpr std::array<subreg_mask_enum, 16> subreg_mask_map {
                subreg_mask_enum::b0,
                subreg_mask_enum::b1,
                subreg_mask_enum::b2,
                subreg_mask_enum::b3,
                subreg_mask_enum::b4,
                subreg_mask_enum::b5,
                subreg_mask_enum::b6,
                subreg_mask_enum::b7,
                subreg_mask_enum::q0,
                subreg_mask_enum::q1,
                subreg_mask_enum::q2,
                subreg_mask_enum::q3,
                subreg_mask_enum::h0,
                subreg_mask_enum::h1,
                subreg_mask_enum::w0
                                    // [15] unused/undefined subreg encoding: value-initializes to
                                    // subreg_mask_enum{} = 0 (old map's miss default)
            };

            struct reg_op_info {
                reg_op_info() = default;
                reg_op_info(bus* pbus, reg* preg, subreg_mask_enum mask, u_byte offset) :
                    pbus(pbus), preg(preg), mask(mask), offset(offset) {
                }
                reg_op_info(const reg_op_info&) = default;
                reg_op_info(reg_op_info&&) = default;
                reg_op_info& operator=(const reg_op_info&) = default;

                bus* pbus {nullptr};
                reg* preg {nullptr};
                subreg_mask_enum mask {subreg_mask_enum::w0};
                u_byte offset {0};
            };

            std::vector<reg_op_info> bus_enable_array;
            std::vector<reg_op_info> bus_set_array;
            std::vector<std::pair<std::pair<reg*, subreg_enum>, int8_t>> increment_array;
            std::map<u_qword, device*> devices;
        
            // (maize-86) T is one of the unsigned width types (u_byte/u_qword/u_hword/u_word); note
            // that testing a < 0 / b < 0 against an unsigned type makes the "negative" branches below
            // dead and leaves the V/C meaning for unsigned MUL debatable. That is a separate M2
            // flag-semantics question, deliberately out of scope here; this card only removes the
            // b == 0 divide-by-zero crash.
            template <typename T>
            static constexpr bool is_mul_overflow(const T &a, const T &b) {
                if (b == 0) {
                    return false;
                }
                return ((b >= 0) && (a >= 0) && (a > std::numeric_limits<T>::max() / b))
                    || ((b < 0) && (a < 0) && (a < std::numeric_limits<T>::max() / b));
            }

            template <typename T>
            static constexpr bool is_mul_underflow(const T &a, const T &b) {
                if (b == 0) {
                    return false;
                }
                return ((b >= 0) && (a < 0) && (a < std::numeric_limits<T>::min() / b))
                    || ((b < 0) && (a >= 0) && (a > std::numeric_limits<T>::min() / b));
            }

        }

        memory_module::~memory_module() {
            for (auto &[base, ptr] : memory_map) {
                delete[] ptr;
            }

            memory_map.clear();
        }

        u_hword memory_module::write_byte(reg_value address, u_byte value) {
            size_t idx {block_size - set_cache_address(address)};
            cache[idx] = value;
            return sizeof(u_byte);
        }

        // Block-aware little-endian byte-store loop shared by every multi-byte
        // write helper. It mirrors the read family (see read() below): when the
        // whole store fits in the currently cached block it stays in a fast
        // in-block loop; when it straddles a 256-byte block boundary it re-resolves
        // the cache block via set_cache_address(++address) on every byte so the
        // remaining bytes land in the NEXT block instead of wrapping 0xFF -> 0x00
        // back into the same block. (maize-42)
        u_hword memory_module::write_bytes(u_word address, u_word value, size_t count) {
            size_t written {0};

            do {
                size_t rem {set_cache_address(address)};
                size_t idx {block_size - rem};

                if (rem >= count) {
                    /* Fast in-block path: store the low `count` bytes of value with one sized
                       store instead of the per-byte loop. Little-endian host assumption as in
                       read(): the byte loop stores LSB-first, matching a fixed-size memcpy. */
                    switch (count) {
                        case 8: std::memcpy(cache + idx, &value, 8); break;
                        case 4: { std::uint32_t t = static_cast<std::uint32_t>(value); std::memcpy(cache + idx, &t, 4); break; }
                        case 2: { std::uint16_t t = static_cast<std::uint16_t>(value); std::memcpy(cache + idx, &t, 2); break; }
                        case 1: cache[idx] = static_cast<u_byte>(value); break;
                        default: {
                            u_word v = value;
                            for (size_t k = 0; k < count; ++k) {
                                cache[idx + k] = static_cast<u_byte>(v & 0xff);
                                v >>= 8;
                            }
                            break;
                        }
                    }
                    written += count;
                    count = 0;
                }
                else {
                    while (count) {
                        cache[idx] = value & 0xff;
                        value >>= 0x08;
                        idx = block_size - set_cache_address(++address);
                        --count;
                        ++written;
                    }
                }

            } while (count);

            return written;
        }

        u_hword memory_module::write_qword(reg_value address, u_qword value) {
            return write_bytes(address.w0, value, sizeof(u_qword));
        }

        u_hword memory_module::write_hword(reg_value address, u_hword value) {
            return write_bytes(address.w0, value, sizeof(u_hword));
        }

        u_hword memory_module::write_word(reg_value address, u_word value) {
            return write_bytes(address.w0, value, sizeof(u_word));
        }

        size_t memory_module::read(reg_value address, u_hword count, std::vector<u_byte> &retval) {
            size_t read_count {0};

            if (count) {
                retval.clear();
                retval.reserve(count);

                do {
                    auto rem = set_cache_address(address);
                    size_t idx = block_size - rem;

                    if (rem >= count) {
                        while (count && idx < block_size) {
                            retval.push_back(cache[idx]);
                            ++idx;
                            --count;
                            ++read_count;
                        }
                    }
                    else {
                        while (count) {
                            retval.push_back(cache[idx]);
                            idx = block_size - set_cache_address(++address.w0);
                            --count;
                            ++read_count;
                        }
                    }

                } while (count);
            }

            return read_count;
        }

        size_t memory_module::read(u_word address, reg_value &reg, subreg_enum subreg) {
            auto count = subreg_size_map[static_cast<size_t>(subreg)];
            size_t dst_idx {subreg_index_map[static_cast<size_t>(subreg)]};
            return read(address, reg, count, dst_idx);
        }

        size_t memory_module::read(reg_value const &address, reg_value &reg, subreg_enum subreg) {
            auto count = subreg_size_map[static_cast<size_t>(subreg)];
            size_t dst_idx {subreg_index_map[static_cast<size_t>(subreg)]};
            return read(address.w0, reg, count, dst_idx);
        }

        size_t memory_module::read(reg_value const &address, reg_value &reg, size_t count, size_t dst_idx) {
            return read(address.w0, reg, count, dst_idx);
        }

        size_t memory_module::read(u_word address, reg_value &reg, size_t count, size_t dst_idx) {
            size_t read_count {0};

            do {
                size_t rem {set_cache_address(address)};
                size_t idx {block_size - rem};

                if (rem >= count) {
                    /* Fast in-block path: the whole read (count in {1,2,4,8} for a subreg)
                       fits in the current block, so pull it with one sized load and merge it
                       into the destination subregister field in a single masked write instead
                       of the per-byte proxy RMW. Assumes a little-endian host (x86-64/ARM64):
                       guest memory is little-endian and reg storage is host-native, so a
                       fixed-size memcpy load matches the byte-loop semantics exactly. */
                    u_word val;
                    switch (count) {
                        case 8: std::memcpy(&val, cache + idx, 8); break;
                        case 4: { std::uint32_t t; std::memcpy(&t, cache + idx, 4); val = t; break; }
                        case 2: { std::uint16_t t; std::memcpy(&t, cache + idx, 2); val = t; break; }
                        case 1: val = cache[idx]; break;
                        default: {
                            val = 0;
                            for (size_t k = 0; k < count; ++k) {
                                val |= static_cast<u_word>(cache[idx + k]) << (k * 8);
                            }
                            break;
                        }
                    }

                    if (dst_idx == 0 && count >= 8) {
                        reg.w0 = val;
                    }
                    else {
                        u_word mask = (count >= 8) ? ~u_word {0}
                                                   : ((u_word {1} << (count * 8)) - 1);
                        mask <<= (dst_idx * 8);
                        reg.w0 = (reg.w0 & ~mask) | ((val << (dst_idx * 8)) & mask);
                    }

                    read_count += count;
                    count = 0;
                }
                else {
                    while (count) {
                        reg[dst_idx] = cache[idx];
                        idx = block_size - set_cache_address(++address);
                        ++dst_idx;
                        --count;
                        ++read_count;
                    }
                }

            } while (count);

            return read_count;
        }

        std::vector<u_byte> memory_module::read(reg_value address, u_word count) {
            std::vector<u_byte> retval;

            if (count) {
                retval.reserve(count);

                do {
                    auto rem = set_cache_address(address);
                    size_t idx = block_size - rem;

                    if (rem >= count) {
                        while (count && idx < block_size) {
                            retval.push_back(cache[idx]);
                            ++idx;
                            --count;
                        }
                    }
                    else {
                        while (count) {
                            retval.push_back(cache[idx]);
                            idx = block_size - set_cache_address(++address.w0);
                            --count;
                        }
                    }

                } while (count);
            }

            return retval;
        }

        void memory_module::read_into(u_word address, u_byte* dst, size_t count) {
            size_t done {0};
            while (count) {
                size_t rem {set_cache_address(address)};   // bytes to end of the current block
                size_t idx {block_size - rem};
                size_t n {rem < count ? rem : count};
                std::memcpy(dst + done, cache + idx, n);
                done += n;
                address += n;
                count -= n;
            }
        }

        u_byte memory_module::read_byte(u_word address) {
            /* A single byte always fits within its block, so no straddle handling. */
            size_t idx {block_size - set_cache_address(address)};
            return cache[idx];
        }

        u_word memory_module::last_block() const {
            /* Highest allocated block base. Tracked incrementally since the backing store
               is now an unordered_map (no ordered rbegin()). */
            return highest_block_;
        }

        /* Out-of-line slow path for set_cache_address (in the header): an L1 block-cache miss
           (first touch of a block, or a slot eviction). Consults the hash map, allocates the
           block on first touch, and fills the L1 slot. Kept out of line so the inlined fast
           path stays small. */
        void memory_module::resolve_block_miss(u_word base, size_t slot) {
            u_byte* blk;
            auto it = memory_map.find(base);
            if (it == memory_map.end()) {
                blk = new u_byte[block_size] {0};
                memory_map.emplace(base, blk);
                if (!any_block_ || base > highest_block_) {
                    highest_block_ = base;
                    any_block_ = true;
                }
            }
            else {
                blk = it->second;
            }
            l1_base[slot] = base;
            l1_ptr[slot] = blk;
            cache = blk;
        }

        void reg::increment(u_byte value, subreg_enum subreg) {
            // increment_array.push_back(std::make_pair(std::make_pair(this, subreg), value));
        }

        void reg::decrement(u_byte value, subreg_enum subreg) {
            // increment_array.push_back(std::make_pair(std::make_pair(this, subreg), -value));
        }

        namespace regs {
            // The CPU's general registers are defined here
            reg r0;
            reg r1;
            reg r2;
            reg r3;
            reg r4;
            reg r5;
            reg r6;
            reg r7;
            reg r8;
            reg r9;
            reg rt;
            reg rv;
            reg rf; // flags register
            reg ri; // instruction register (decoder-internal; not operand-addressable, maize-41)
            reg rb; // base pointer register (BP); operand slot $D (maize-41)
            reg rp {0x0000000000000000}; // program execution register (PC); full 64-bit (maize-41)
            reg rs; // stack register (SP); full 64-bit (maize-41)
            reg fcsr {0x0000000000000000}; // FP control/status: FRM(bits7-5)+FFLAGS(bits4-0); reset RNE, flags clear (maize-122)
        }

        /* Sv48 address translation + software-TLB control (card maize-194). translate()
           itself is defined inline in the anonymous namespace, just above the choke-point
           helpers, so its Bare-mode identity fast path inlines into every translated
           memory-access site (maize-194 review #2605, nit/perf 5). Its out-of-line slow
           path (translate_slow: TLB probe + Sv48 walk) and the two TLB-flush helpers are
           forward-declared here so the inline fast path and the choke points resolve them
           via enclosing-namespace lookup; their definitions live below deliver_vectored
           (raise_page_fault vectors a cause-8 fault through it). access_kind classifies the
           access for the CR2 FAULT_ERR error code and the leaf permission check (X for
           fetch, R for load, W for store). */
        enum class access_kind : u_byte { fetch = 0, load = 1, store = 2 };
        u_word translate_slow(u_word va, access_kind kind);
        void tlb_flush_all();
        void tlb_flush_va(u_word va);

        namespace {
            flag<bit_carryout> carryout_flag {regs::rf};
            flag<bit_negative> negative_flag {regs::rf};
            flag<bit_overflow> overflow_flag {regs::rf};
            flag<bit_parity> parity_flag {regs::rf};
            flag<bit_zero> zero_flag {regs::rf};
            flag<bit_sign> sign_flag {regs::rf};
            flag<bit_privilege> privilege_flag {regs::rf};
            flag<bit_interrupt_enabled> interrupt_enabled_flag {regs::rf};
            /* card maize-24 (D9 Shape B): SET routes SYS through cause 7 to the guest
               handler; CLEAR (boot default) keeps the native sys::call provider. */
            flag<bit_syscall_guest> syscall_guest_flag {regs::rf};
            flag<bit_interrupt_set> interrupt_set_flag {regs::rf};
            flag<bit_running> running_flag {regs::rf};

            /* card maize-197: lazy / on-demand condition-flag computation. An ALU op no
               longer computes Z/N/C/V eagerly; run_alu records a cheap descriptor of the
               operation here (op kind, width, and the operands/result already computed
               locally) and sets dirty = true. materialize_flags() resolves that descriptor
               into concrete RF bits the first time a consumer actually reads the flags
               (Jcc / SETcc, an ADC/SBB carry-in read, a trap-frame save, or RF named as a
               generic instruction operand). Flag VALUES are bit-identical to the old eager
               path; only the moment of computation moves. Stored as a standalone struct,
               NOT as fields on arithmetic_logic_unit, so maize-198's staging-bank refactor
               leaves it untouched (recorded decision). */
            struct pending_flags_t {
                bool dirty = false;    // Z/N/C/V in regs::rf.w0 are stale until resolved
                u_byte op_kind = 0;    // discriminator: (alu.b0() & opflag_code) or an alu_uop_*
                u_byte width = 0;      // op_size: 1, 2, 4, or 8 bytes
                u_word dst_before = 0; // pre-op destination, low `width` bytes zero-extended
                u_word src = 0;        // pre-op source (or shift count for SHL/SHR/SAR)
                u_word result = 0;     // post-op result, byte-identical to alu.op2_reg.w0
            };
            pending_flags_t pending_flags;

            /* Record a deferred flag computation (card maize-197). Every flag-writing
               run_alu case calls this in place of the old eager Z/N/C/V store. */
            void stage_pending_flags(u_byte op_kind, u_byte width, u_word dst_before, u_word src, u_word result) {
                pending_flags.dirty = true;
                pending_flags.op_kind = op_kind;
                pending_flags.width = width;
                pending_flags.dst_before = dst_before;
                pending_flags.src = src;
                pending_flags.result = result;
            }

            /* Resolve any pending deferred flags into concrete RF bits (card maize-197).
               A no-op when nothing is pending. Declared here so the operand accessors and
               eval_condition can call it; DEFINED below the alu_uop_* selectors it switches
               on. Every flag CONSUMER must call this before reading Z/N/C/V (or any raw RF
               bit, since RF's low bits ARE those flags). */
            void materialize_flags();

            /* Map an instruction's register-flag nybble value to an actual register reference. */
            std::array<reg*, 16> reg_map {
                &regs::r0,
                &regs::r1,
                &regs::r2,
                &regs::r3,
                &regs::r4,
                &regs::r5,
                &regs::r6,
                &regs::r7,
                &regs::r8,
                &regs::r9,
                &regs::rt,
                &regs::rv,
                &regs::rf,
                &regs::rb, // slot $D: base pointer (BP); RI is decoder-internal, not operand-addressable (maize-41)
                &regs::rp,
                &regs::rs
            };

            size_t op1_imm_size_flag() {
                return regs::ri.b1() & opflag_imm_size;
            }

            u_byte op1_imm_size() {
                return u_byte(1) << (regs::ri.b1() & opflag_imm_size);
            }

            u_byte op1_reg_flag() {
                return regs::ri.b1() & opflag_reg;
            }

            u_byte op1_reg_index() {
                return (regs::ri.b1() & opflag_reg) >> 4;
            }

            u_byte op1_subreg_index() {
                return regs::ri.b1() & opflag_subreg;
            }

            subreg_enum op1_subreg_flag() {
                return static_cast<subreg_enum>(regs::ri.b1() & opflag_subreg);
            }

            u_byte op1_subreg_size() {
                return subreg_size_map[static_cast<size_t>(static_cast<subreg_enum>(regs::ri.b1() & opflag_subreg))];
            }

            reg& op1_reg() {
                reg& result = *reg_map[(regs::ri.b1() & opflag_reg) >> 4];
                /* card maize-197: RF is slot $C, addressable as a generic operand. If
                   this instruction names RF, resolve any deferred flags before the
                   handler reads (or overwrites) it, so RF's low bits carry concrete
                   Z/N/C/V. Mirrors the commit_reg_w0 pointer-compare precedent (maize-180). */
                if (&result == static_cast<reg*>(&regs::rf)) {
                    materialize_flags();
                }
                return result;
            }

            u_byte op2_imm_size_flag() {
                return regs::ri.b2() & opflag_imm_size;
            }

            u_byte op2_imm_size() {
                return 1 << (regs::ri.b2() & opflag_imm_size);
            }

            u_byte op2_reg_flag() {
                return regs::ri.b2() & opflag_reg;
            }

            u_byte op2_reg_index() {
                return (regs::ri.b2() & opflag_reg) >> 4;
            }

            u_byte op2_subreg_index() {
                return regs::ri.b2() & opflag_subreg;
            }

            subreg_enum op2_subreg_flag() {
                return static_cast<subreg_enum>(regs::ri.b2() & opflag_subreg);
            }

            u_byte op2_subreg_size() {
                return subreg_size_map[static_cast<size_t>(static_cast<subreg_enum>(regs::ri.b2() & opflag_subreg))];
            }

            reg& op2_reg() {
                reg& result = *reg_map[(regs::ri.b2() & opflag_reg) >> 4];
                /* card maize-197: materialize deferred flags when RF is named as this
                   operand (read or write destination). See op1_reg. */
                if (&result == static_cast<reg*>(&regs::rf)) {
                    materialize_flags();
                }
                return result;
            }

            u_byte op3_reg_flag() {
                return regs::ri.b3() & opflag_reg;
            }

            u_byte op3_reg_index() {
                return (regs::ri.b3() & opflag_reg) >> 4;
            }

            u_byte op3_subreg_index() {
                return regs::ri.b3() & opflag_subreg;
            }

            subreg_enum op3_subreg_flag() {
                return static_cast<subreg_enum>(regs::ri.b3() & opflag_subreg);
            }

            u_byte op3_subreg_size() {
                return subreg_size_map[static_cast<size_t>(static_cast<subreg_enum>(regs::ri.b3() & opflag_subreg))];
            }

            reg &op3_reg() {
                reg& result = *reg_map[(regs::ri.b3() & opflag_reg) >> 4];
                /* card maize-197: materialize deferred flags when RF is named as this
                   operand (read or write destination). See op1_reg. */
                if (&result == static_cast<reg*>(&regs::rf)) {
                    materialize_flags();
                }
                return result;
            }

            subreg_enum pc_src_imm_subreg_flag() {
                return imm_size_subreg_map[static_cast<size_t>(op1_imm_size_flag())];
            }

            subreg_enum pc_dst_imm_subreg_flag() {
                return imm_size_subreg_map[static_cast<size_t>(op2_imm_size_flag())];
            }

            /* Raw (zero-extended) read/write of a named subregister, no sign
               extension (card maize-122). FP operands carry IEEE-754 bit patterns,
               not integers, so the sign-extending copy_regval_reg would corrupt the
               upper bits of a narrow value; these move exactly the subregister's
               bits. Write preserves the rest of the destination register (the
               existing subregister merge semantics). */
            u_word read_subreg_bits(reg_value const &src, subreg_enum src_subreg) {
                auto off = subreg_offset_map[static_cast<size_t>(src_subreg)];
                auto mask = subreg_mask_map[static_cast<size_t>(src_subreg)];
                return (src.w0 & static_cast<u_word>(mask)) >> off;
            }

            /* card maize-198: sign-extending subregister read, the value-returning
               twin of copy_regval_reg's source handling. The in-place ALU handlers
               read the source operand of a register-register op directly (no staging
               bank), and the source must be sign-extended from its OWN encoded subreg
               width to a full word BEFORE the op body truncates it to the destination
               width (Decision D3): when src and dst widths differ (e.g. ADD R0.B0,
               R1.W0), copy_regval_reg used to sign-extend the narrow source, and that
               is real observable behavior. Uses the identical subreg_sign_bit[] /
               subreg_neg_bits[] tables copy_regval_reg uses, so the extended value is
               bit-identical. read_subreg_bits (zero-extending) is used for the
               destination read, where op_size always equals the destination's own
               width, so no extension hazard exists there. */
            u_word read_subreg_signext(reg_value const &src, subreg_enum src_subreg) {
                auto off = subreg_offset_map[static_cast<size_t>(src_subreg)];
                auto mask = subreg_mask_map[static_cast<size_t>(src_subreg)];
                u_word value = (src.w0 & static_cast<u_word>(mask)) >> off;
                if (value & subreg_sign_bit[static_cast<int>(src_subreg)]) {
                    value |= subreg_neg_bits[static_cast<int>(src_subreg)];
                }
                return value;
            }

            /* card maize-180 (§9 RF-write closure). Single choke point for every guest
               register-destination write: if the destination register is RF and the CPU
               is in user mode (privilege_flag clear), the privileged RF.H1 bits
               (rf_privileged_mask) RETAIN their current values and only the non-privileged
               (condition-flag) bits take the written value. This is the x86 POPF-in-user
               model: it closes the escalation vector where a user instruction naming RF as
               its destination (CP/LD/POP/CPZ/CLR/ALU write-back) would otherwise set the
               privilege bit raw and step straight into supervisor. Supervisor writes and
               every non-RF destination pass through unchanged (a pointer compare plus a
               flag read on the register-write hot path). Applied at write time, so IRET
               (privileged, only ever executed from supervisor) restores the full RF word
               normally, and the deliver_vectored trap frame (which never routes an RF write
               through these helpers) is unaffected. Every reg-write helper below commits
               its final value through here, so no RF-destination path bypasses the mask. */
            void commit_reg_w0(reg_value &dst, u_word new_w0) {
                if (&dst == static_cast<reg_value*>(&regs::rf) && !privilege_flag) {
                    new_w0 = (new_w0 & ~rf_privileged_mask) | (regs::rf.w0 & rf_privileged_mask);
                }
                dst.w0 = new_w0;
            }

            void write_subreg_bits(reg_value &dst, subreg_enum dst_subreg, u_word bits) {
                auto off = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];
                commit_reg_w0(dst, (~static_cast<u_word>(mask) & dst.w0) | ((bits << off) & static_cast<u_word>(mask)));
            }

            void clr_reg(reg_value &dst, subreg_enum dst_subreg) {
                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];

                u_word src_value = 0;
                commit_reg_w0(dst, (~static_cast<u_word>(dst_mask) & dst.w0) | (src_value << dst_offset) & static_cast<u_word>(dst_mask));
            }

            /* SETcc write (card maize-55): identical masked write to clr_reg, but
               src_value is the condition (0 or 1) instead of the constant 0. The
               named destination subregister field becomes 0/1 and the rest of the
               register is preserved. Flag-neutral: RF is never touched here. */
            void set_reg(reg_value &dst, subreg_enum dst_subreg, bool condition) {
                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];

                u_word src_value = condition ? 1 : 0;
                commit_reg_w0(dst, (~static_cast<u_word>(dst_mask) & dst.w0) | (src_value << dst_offset) & static_cast<u_word>(dst_mask));
            }

            bool cmp_regval_reg(reg_value const &src, subreg_enum src_subreg, reg_value &dst, subreg_enum dst_subreg) {
                auto src_offset = subreg_offset_map[static_cast<size_t>(src_subreg)];
                auto src_mask = subreg_mask_map[static_cast<size_t>(src_subreg)];

                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];

                u_word src_value = (src.w0 & static_cast<u_word>(src_mask)) >> src_offset;
                u_word dst_value = (dst.w0 & static_cast<u_word>(dst_mask)) >> dst_offset;

                return src_value == dst_value;
            }

            /* Data-movement primitives (copy / load / store) are flag-neutral: they never write
               C/N/V/Z. Only the ALU ops and the explicit CMP/TEST/CMPIND/TSTIND/SETCRY/CLRCRY
               set flags (card maize-4). copy_regval_reg still sign-extends the source, using a
               local rather than the negative flag. */

            void copy_regval_reg(reg_value const &src, subreg_enum src_subreg, reg_value &dst, subreg_enum dst_subreg) {
                auto src_offset = subreg_offset_map[static_cast<size_t>(src_subreg)];
                auto src_mask = subreg_mask_map[static_cast<size_t>(src_subreg)];

                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];

                u_word src_value = (src.w0 & static_cast<u_word>(src_mask)) >> src_offset;

                bool negative = (src_value & subreg_sign_bit[static_cast<int>(src_subreg)]) != 0;

                if (negative) {
                    src_value |= subreg_neg_bits[static_cast<int>(src_subreg)];
                }

                commit_reg_w0(dst, (~static_cast<u_word>(dst_mask) & dst.w0) | (src_value << dst_offset) & static_cast<u_word>(dst_mask));
            }

            void copy_regval_reg_zext(reg_value const &src, subreg_enum src_subreg, reg_value &dst, subreg_enum dst_subreg) {
                auto src_offset = subreg_offset_map[static_cast<size_t>(src_subreg)];
                auto src_mask = subreg_mask_map[static_cast<size_t>(src_subreg)];

                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];

                u_word src_value = (src.w0 & static_cast<u_word>(src_mask)) >> src_offset;

                commit_reg_w0(dst, (~static_cast<u_word>(dst_mask) & dst.w0) | (src_value << dst_offset) & static_cast<u_word>(dst_mask));
            }

            /* ---- Control-register file + software TLB + page-fault-delivery state
               (cards maize-180 / maize-194). Relocated ahead of the choke-point memory
               helpers below so translate()'s Bare-mode identity fast path can be DEFINED
               inline before its first call site (maize-194 review #2605, nit/perf 5). The
               ~150M-access/frame memory loop then inlines the single SATP.MODE check plus
               `return va`, emitting a call to the out-of-line translate_slow() only under
               Sv48 (MODE == 1), preserving the bare-mode zero-cost guarantee structurally
               rather than relying on the optimizer to inline an externally-linked
               definition that appears later in the file. */

            /* Control-register file (card maize-180, the D2 access mechanism). A small,
               privileged, flat-indexed register bank reached ONLY through MOVTCR / MOVFCR,
               distinct from device ports (port_write / port_read) and the syscall surface.
               CR0 SATP: MODE[3:0] + PPN[63:12]; CR1 FAULT_VA; CR2 FAULT_ERR. Reset 0. An
               undefined index (> 2) mirrors the unpopulated-port convention: writes
               discarded, reads yield 0, no trap. Reserved SATP bits [11:4] are forced to 0
               on write (cr_write). */
            static constexpr size_t control_reg_count = 3;
            reg control_regs[control_reg_count] {};

            /* Software TLB (card maize-194): 64-entry direct-mapped, indexed by the low 6
               bits of the VPN (VA >> 12), the same direct-mapped-by-low-bits shape as
               memory_module's L1 block cache; a miss overwrites its one candidate slot (no
               LRU). Only a successful level-0 4 KiB leaf is cached; superpage leaves are
               used but never installed (decision). */
            struct tlb_entry {
                bool valid = false;
                u_word tag = 0;   // VPN = VA[47:12] (VA[63:48] discarded, decision 8619)
                u_word ppn = 0;   // physical page base, low 12 bits already 0
                bool r = false, w = false, x = false, u = false;
            };
            static constexpr size_t tlb_size = 64;
            tlb_entry software_tlb[tlb_size] {};
            /* TLB tag = VA[47:12] only. VA[63:48] is ignored (decision 8619), so the tag
               must mask them off; otherwise two VAs differing only in the ignored high half
               would occupy distinct TLB entries instead of sharing one (maize-194 review
               #2605, nit 4). 36 bits spans VA[47:12]. The direct-mapped index uses the low
               6 bits of the VPN, which lie in VA[17:12] and are unaffected by this mask. */
            static constexpr u_word vpn_tag_mask = (u_word {1} << 36) - 1;

            /* Page fault is FAULT-class: the saved PC is the faulting instruction's own
               entry PC, captured at the top of MAIZE_NEXT() before the opcode fetch, not
               the advanced PC. delivering_trap guards the double-fault case: a fault raised
               from inside deliver_vectored's own trap-frame push halts deterministically
               instead of recursing. */
            u_word current_instr_pc = 0;
            bool delivering_trap = false;

            /* Bare-mode identity fast path for guest-address translation (card maize-194).
               Inline on the fetch/memory hot path: a single SATP.MODE check returns the VA
               unchanged in Bare mode (MODE != 1, including undefined MODE 2-15), paying no
               TLB probe and no walk. Sv48 mode (MODE == 1) delegates to the out-of-line
               translate_slow() (TLB probe, then walk on a miss; forward-declared above the
               choke points as `translate_slow`). */
            inline u_word translate(u_word va, access_kind kind) {
                if ((control_regs[0].w0 & 0xF) != 1) {
                    return va;   // Bare / undefined MODE: passthrough, zero added cost
                }
                return translate_slow(va, kind);
            }

            void copy_memval_reg(u_word address, size_t size, reg_value &op2_reg, subreg_enum dst_subreg, access_kind kind) {
                /* Read the size-byte immediate, then sign-extend it to the destination
                   subregister width (card maize-29). A narrow immediate copied into a wider
                   register fills the upper bytes by sign, matching register-to-register CP
                   (copy_regval_reg). This unifies every immediate-value site on one rule: CP
                   immediates, the ALU immediate operands (which previously merged, leaking stale
                   upper bytes into the operation), and stack/pointer loads. CPZ is the
                   zero-extending counterpart; an explicit narrower destination subregister is the
                   partial-write escape hatch.

                   The immediate is read into a plain u_word (LE host: read_into fills the low
                   `size` bytes of a zeroed word, so it is already zero-extended), avoiding a
                   128-byte reg_value temporary + a second copy on this hot immediate path. The
                   immediate source subreg is low-aligned (offset 0), so the source value is the
                   raw word. */
                u_word raw = 0;
                mm.read_into(translate(address, kind), reinterpret_cast<u_byte*>(&raw), size);
                subreg_enum src_subreg = imm_size_subreg_map[static_cast<size_t>(size)];
                if (raw & subreg_sign_bit[static_cast<int>(src_subreg)]) {
                    raw |= subreg_neg_bits[static_cast<int>(src_subreg)];
                }
                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];
                commit_reg_w0(op2_reg, (~static_cast<u_word>(dst_mask) & op2_reg.w0)
                    | ((raw << dst_offset) & static_cast<u_word>(dst_mask)));
            }

            void copy_memval_reg_zext(u_word address, size_t size, reg_value &op2_reg, subreg_enum dst_subreg, access_kind kind) {
                /* Zero-extending immediate value for CPZ (card maize-29): a narrow immediate fills
                   the destination's upper bytes with zero, the unsigned counterpart to
                   copy_memval_reg's sign-extension. read_into into a zeroed word is already the
                   zero-extended value; no reg_value temporary. */
                u_word raw = 0;
                mm.read_into(translate(address, kind), reinterpret_cast<u_byte*>(&raw), size);
                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];
                commit_reg_w0(op2_reg, (~static_cast<u_word>(dst_mask) & op2_reg.w0)
                    | ((raw << dst_offset) & static_cast<u_word>(dst_mask)));
            }

            void copy_regaddr_reg(reg_value const &src, subreg_enum src_subreg, reg_value &dst, subreg_enum dst_subreg) {
                /* LD reads from the address held in the source register. The number of bytes read
                   is the size of the destination subregister (card maize-29): `LD @Rn R0.B0` reads
                   one byte, `LD @Rn R0.H0` reads four. The bytes land in the destination field and
                   the rest of the register is preserved. Reading exactly the destination width
                   (rather than a fixed 8) avoids over-reading past the intended address. */
                auto src_offset = subreg_offset_map[static_cast<size_t>(src_subreg)];
                auto src_mask = subreg_mask_map[static_cast<size_t>(src_subreg)];

                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];

                u_word src_address = (static_cast<u_word>(src_mask) & src.w0) >> src_offset;
                reg src_data;
                src_data.w0 = 0;
                mm.read(translate(src_address, access_kind::load), src_data, subreg_size_map[static_cast<size_t>(dst_subreg)], 0);

                commit_reg_w0(dst, (~static_cast<u_word>(dst_mask) & dst.w0) | (src_data.w0 << dst_offset) & static_cast<u_word>(dst_mask));
            }

            void copy_memaddr_reg(u_word address, size_t size, reg_value &dst, subreg_enum dst_subreg) {

                /* `size` is the width of the immediate ADDRESS operand in the code stream
                   (op1_imm_size). Read exactly that many bytes, zero-extended: reading a fixed
                   8 bytes over-reads into the following instruction bytes and computes a garbage
                   address when the address is encoded in fewer than 8 bytes (card maize-40). */
                reg src_address;
                mm.read(translate(address, access_kind::fetch), src_address, size, 0);

                /* The load width is the destination subregister size, not a fixed 8 (card
                   maize-29): read exactly as many bytes from the target address as the destination
                   holds, so `LD @imm R0.B0` reads one byte and the rest of R0 is preserved. */
                reg src_data;
                src_data.w0 = 0;
                mm.read(translate(src_address.w0, access_kind::load), src_data, subreg_size_map[static_cast<size_t>(dst_subreg)], 0);

                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];

                commit_reg_w0(dst, (~static_cast<u_word>(dst_mask) & dst.w0) | (src_data.w0 << dst_offset) & static_cast<u_word>(dst_mask));

            }

            void copy_regaddr_reg_zext(reg_value const &src, subreg_enum src_subreg, reg_value &dst, subreg_enum dst_subreg) {
                /* LDZ regAddr form (card maize-204): same address computation and read-width
                   selection as LD's copy_regaddr_reg (read exactly dst_subreg's width from the
                   address held in src), but zero-extend the result into dst's FULL register
                   instead of landing only in the dst_subreg field. Equivalent to
                   LD dst.<width> followed by CPZ dst.<width> dst, in one instruction. */
                auto src_offset = subreg_offset_map[static_cast<size_t>(src_subreg)];
                auto src_mask = subreg_mask_map[static_cast<size_t>(src_subreg)];

                u_word src_address = (static_cast<u_word>(src_mask) & src.w0) >> src_offset;
                reg src_data;
                src_data.w0 = 0;
                mm.read(translate(src_address, access_kind::load), src_data, subreg_size_map[static_cast<size_t>(dst_subreg)], 0);

                /* src_data.w0 is already zero above the bytes just read, so it is the
                   zero-extended full-width value; write it to the whole register. */
                commit_reg_w0(dst, src_data.w0);
            }

            void copy_memaddr_reg_zext(u_word address, size_t size, reg_value &dst, subreg_enum dst_subreg) {
                /* LDZ immAddr form (card maize-204): same address-immediate decode as
                   copy_memaddr_reg, zero-extended write (see copy_regaddr_reg_zext). `size` is
                   the encoded ADDRESS immediate's width (op1_imm_size), not the load width. */
                reg src_address;
                mm.read(translate(address, access_kind::fetch), src_address, size, 0);

                reg src_data;
                src_data.w0 = 0;
                mm.read(translate(src_address.w0, access_kind::load), src_data, subreg_size_map[static_cast<size_t>(dst_subreg)], 0);

                commit_reg_w0(dst, src_data.w0);
            }

            /* Set PC (RP) from an immediate jump/branch target in the code stream at PC, encoded
               at op1_imm_size() width (maize-41). Read exactly that many bytes zero-extended into
               a fresh register, then replace PC. Honors the size flag so a target can reach the
               full 64-bit address space instead of the old hardcoded 4 bytes. */
            void jump_to_immediate() {
                u_byte imm_size = op1_imm_size();
                reg target;
                mm.read(translate(regs::rp.w0, access_kind::fetch), target, imm_size, 0);
                regs::rp.w0 = target.w0;
            }

            /* Shared condition machinery (card maize-64). The two high opcode bits
               select the condition "row" and the base slot selects the "column";
               decode_condition folds them into a single index that eval_condition
               maps to a flag predicate. This ONE predicate table drives BOTH Jcc
               and SETcc, so the flag formulas have a single source of truth (no
               copy-pasted per-condition expressions in the dispatch cases). */
            u_byte decode_condition(u_byte base) {
                u_byte row = (regs::ri.b0() & opcode_flag) >> 6;
                u_byte col = static_cast<u_byte>((regs::ri.b0() & 0x3F) - base);
                return static_cast<u_byte>(row * 3 + col);
            }

            bool eval_condition(u_byte cond) {
                /* card maize-197: the single flag CONSUMER shared by Jcc and SETcc.
                   Resolve any deferred flags before reading Z/N/C/V (and parity). */
                materialize_flags();
                switch (cond) {
                    case 0: return (bool)zero_flag;                                             // Z
                    case 1: return !zero_flag;                                                  // NZ
                    case 2: return (bool)negative_flag != (bool)overflow_flag;                  // LT
                    case 3: return (bool)carryout_flag;                                         // B  (unsigned <)
                    case 4: return !zero_flag && ((bool)negative_flag == (bool)overflow_flag);  // GT
                    case 5: return !carryout_flag && !zero_flag;                                // A  (unsigned >)
                    case 6: return (bool)negative_flag == (bool)overflow_flag;                  // GE
                    case 7: return zero_flag || ((bool)negative_flag != (bool)overflow_flag);   // LE
                    case 8: return carryout_flag || zero_flag;                                  // BE (unsigned <=)
                    case 9: return !carryout_flag;                                              // AE (unsigned >=)
                    case 10: return (bool)parity_flag;                                          // P  (unordered / NaN, maize-122)
                    default: {
                        std::stringstream err {};
                        err << "unallocated condition encoding: " << std::hex << static_cast<unsigned>(regs::ri.b0());
                        throw std::logic_error(err.str());
                    }
                }
            }

            /* ALU micro-op selectors for the packed unary family (card maize-64).
               The packed unary instruction bytes share their low-6 bits ($31 for
               INC/DEC/NOT/NEG, $32 for CLR/POP), so run_alu (which dispatches on
               alu.b0() & opflag_code) cannot decode them off the raw opcode. tick()
               translates the condition-style row bits to one of these low-6-unique
               selectors before calling run_alu. */
            const u_byte alu_uop_inc {0x31};
            const u_byte alu_uop_dec {0x32};
            const u_byte alu_uop_not {0x33};
            const u_byte alu_uop_neg {0x2A};

            /* card maize-197: resolve a deferred flag descriptor into concrete Z/N/C/V
               bits. This is the ONLY place a pending descriptor becomes RF bits. Every
               per-op / per-width formula below is a lift of the corresponding run_alu
               case's flag arithmetic, reading pending_flags fields (cast back to the
               operation width, so C++ integer promotion matches run_alu exactly) instead
               of alu.op1_reg / alu.op2_reg. Cases are grouped exactly as run_alu groups
               them: cmp/cmpind share sub; test/testind share and; inc shares add with
               src == 1; dec shares sub with src == 1; the pure z/n ops (logic, div, mod,
               not) share one path. `result` is the stored post-op value (byte-identical
               to what run_alu wrote to alu.op2_reg.w0), so z/n never need recomputation.
               No formula changes: bit-identical to the old eager store, just deferred. */
            void materialize_flags() {
                if (!pending_flags.dirty) {
                    return;
                }

                const u_byte alu_op = pending_flags.op_kind;
                const u_byte op_size = pending_flags.width;
                const u_word dst_before = pending_flags.dst_before;
                const u_word src = pending_flags.src;
                const u_word result = pending_flags.result;

                bool z = false, n = false, c = false, v = false;

                switch (alu_op) {
                    /* ADD family. INC is ADD with src == 1 (staged with src = 1), so it
                       shares this formula verbatim (card maize-1). */
                    case instr::add_opcode:
                    case alu_uop_inc: {
                        switch (op_size) {
                            case 1: { u_byte d = static_cast<u_byte>(dst_before), s = static_cast<u_byte>(src), r = static_cast<u_byte>(result);
                                z = r == 0; n = r & 0x80; c = r < d; v = (~(d ^ s) & (d ^ r)) & 0x80; break; }
                            case 2: { u_qword d = static_cast<u_qword>(dst_before), s = static_cast<u_qword>(src), r = static_cast<u_qword>(result);
                                z = r == 0; n = r & 0x8000; c = r < d; v = (~(d ^ s) & (d ^ r)) & 0x8000; break; }
                            case 4: { u_hword d = static_cast<u_hword>(dst_before), s = static_cast<u_hword>(src), r = static_cast<u_hword>(result);
                                z = r == 0; n = r & 0x80000000; c = r < d; v = (~(d ^ s) & (d ^ r)) & 0x80000000; break; }
                            case 8: { u_word d = dst_before, s = src, r = result;
                                z = r == 0; n = r & 0x8000000000000000; c = r < d; v = (~(d ^ s) & (d ^ r)) & 0x8000000000000000; break; }
                        }
                        break;
                    }

                    /* SUB family: CMP/CMPIND share sub's formula; DEC is SUB with src == 1
                       (staged with src = 1). Carry is the unsigned borrow (card maize-1). */
                    case instr::sub_opcode:
                    case instr::cmp_opcode:
                    case instr::cmpind_opcode:
                    case alu_uop_dec: {
                        switch (op_size) {
                            case 1: { u_byte d = static_cast<u_byte>(dst_before), s = static_cast<u_byte>(src), r = static_cast<u_byte>(result);
                                z = r == 0; n = r & 0x80; c = r > d; v = ((d ^ s) & (d ^ r)) & 0x80; break; }
                            case 2: { u_qword d = static_cast<u_qword>(dst_before), s = static_cast<u_qword>(src), r = static_cast<u_qword>(result);
                                z = r == 0; n = r & 0x8000; c = r > d; v = ((d ^ s) & (d ^ r)) & 0x8000; break; }
                            case 4: { u_hword d = static_cast<u_hword>(dst_before), s = static_cast<u_hword>(src), r = static_cast<u_hword>(result);
                                z = r == 0; n = r & 0x80000000; c = r > d; v = ((d ^ s) & (d ^ r)) & 0x80000000; break; }
                            case 8: { u_word d = dst_before, s = src, r = result;
                                z = r == 0; n = r & 0x8000000000000000; c = r > d; v = ((d ^ s) & (d ^ r)) & 0x8000000000000000; break; }
                        }
                        break;
                    }

                    /* ADC (card maize-6). sum1 = dst + src is recomputed at the op width;
                       result already bakes in the carry-in, so no stored carry is needed. */
                    case instr::adc_opcode: {
                        switch (op_size) {
                            case 1: { u_byte d = static_cast<u_byte>(dst_before), s = static_cast<u_byte>(src), r = static_cast<u_byte>(result); u_byte sum1 = d + s;
                                z = r == 0; n = r & 0x80; c = (sum1 < d) || (r < sum1); v = (~(d ^ s) & (d ^ r)) & 0x80; break; }
                            case 2: { u_qword d = static_cast<u_qword>(dst_before), s = static_cast<u_qword>(src), r = static_cast<u_qword>(result); u_qword sum1 = d + s;
                                z = r == 0; n = r & 0x8000; c = (sum1 < d) || (r < sum1); v = (~(d ^ s) & (d ^ r)) & 0x8000; break; }
                            case 4: { u_hword d = static_cast<u_hword>(dst_before), s = static_cast<u_hword>(src), r = static_cast<u_hword>(result); u_hword sum1 = d + s;
                                z = r == 0; n = r & 0x80000000; c = (sum1 < d) || (r < sum1); v = (~(d ^ s) & (d ^ r)) & 0x80000000; break; }
                            case 8: { u_word d = dst_before, s = src, r = result; u_word sum1 = d + s;
                                z = r == 0; n = r & 0x8000000000000000; c = (sum1 < d) || (r < sum1); v = (~(d ^ s) & (d ^ r)) & 0x8000000000000000; break; }
                        }
                        break;
                    }

                    /* SBB (card maize-6). diff1 = dst - src recomputed at the op width. */
                    case instr::sbb_opcode: {
                        switch (op_size) {
                            case 1: { u_byte d = static_cast<u_byte>(dst_before), s = static_cast<u_byte>(src), r = static_cast<u_byte>(result); u_byte diff1 = d - s;
                                z = r == 0; n = r & 0x80; c = (diff1 > d) || (r > diff1); v = ((d ^ s) & (d ^ r)) & 0x80; break; }
                            case 2: { u_qword d = static_cast<u_qword>(dst_before), s = static_cast<u_qword>(src), r = static_cast<u_qword>(result); u_qword diff1 = d - s;
                                z = r == 0; n = r & 0x8000; c = (diff1 > d) || (r > diff1); v = ((d ^ s) & (d ^ r)) & 0x8000; break; }
                            case 4: { u_hword d = static_cast<u_hword>(dst_before), s = static_cast<u_hword>(src), r = static_cast<u_hword>(result); u_hword diff1 = d - s;
                                z = r == 0; n = r & 0x80000000; c = (diff1 > d) || (r > diff1); v = ((d ^ s) & (d ^ r)) & 0x80000000; break; }
                            case 8: { u_word d = dst_before, s = src, r = result; u_word diff1 = d - s;
                                z = r == 0; n = r & 0x8000000000000000; c = (diff1 > d) || (r > diff1); v = ((d ^ s) & (d ^ r)) & 0x8000000000000000; break; }
                        }
                        break;
                    }

                    /* MUL (card maize-1): C mirrors V (signed overflow of the pre-op
                       operands). is_mul_overflow/underflow take (src, dst), matching the
                       (op1, op2) argument order run_alu passes. */
                    case instr::mul_opcode: {
                        switch (op_size) {
                            case 1: { u_byte d = static_cast<u_byte>(dst_before), s = static_cast<u_byte>(src), r = static_cast<u_byte>(result);
                                z = r == 0; n = r & 0x80; bool ovf = is_mul_overflow(s, d) || is_mul_underflow(s, d); c = ovf; v = ovf; break; }
                            case 2: { u_qword d = static_cast<u_qword>(dst_before), s = static_cast<u_qword>(src), r = static_cast<u_qword>(result);
                                z = r == 0; n = r & 0x8000; bool ovf = is_mul_overflow(s, d) || is_mul_underflow(s, d); c = ovf; v = ovf; break; }
                            case 4: { u_hword d = static_cast<u_hword>(dst_before), s = static_cast<u_hword>(src), r = static_cast<u_hword>(result);
                                z = r == 0; n = r & 0x80000000; bool ovf = is_mul_overflow(s, d) || is_mul_underflow(s, d); c = ovf; v = ovf; break; }
                            case 8: { u_word d = dst_before, s = src, r = result;
                                z = r == 0; n = r & 0x8000000000000000; bool ovf = is_mul_overflow(s, d) || is_mul_underflow(s, d); c = ovf; v = ovf; break; }
                        }
                        break;
                    }

                    /* MULW: signed wide multiply (card maize-7). Recompute the full 2w-bit
                       product from the pre-op operands; flags read the high half + range. */
                    case instr::mulw_opcode: {
                        switch (op_size) {
                            case 1: {
                                s_qword p = static_cast<s_qword>(static_cast<s_byte>(static_cast<u_byte>(dst_before))) * static_cast<s_qword>(static_cast<s_byte>(static_cast<u_byte>(src)));
                                u_qword up = static_cast<u_qword>(p);
                                u_word lo = up & 0xFF; u_word hi = (up >> 8) & 0xFF;
                                z = (lo == 0 && hi == 0); n = hi & 0x80; c = hi != 0; v = (p < -128 || p > 127); break;
                            }
                            case 2: {
                                s_hword p = static_cast<s_hword>(static_cast<s_qword>(static_cast<u_qword>(dst_before))) * static_cast<s_hword>(static_cast<s_qword>(static_cast<u_qword>(src)));
                                u_hword up = static_cast<u_hword>(p);
                                u_word lo = up & 0xFFFF; u_word hi = (up >> 16) & 0xFFFF;
                                z = (lo == 0 && hi == 0); n = hi & 0x8000; c = hi != 0; v = (p < -32768 || p > 32767); break;
                            }
                            case 4: {
                                s_word p = static_cast<s_word>(static_cast<s_hword>(static_cast<u_hword>(dst_before))) * static_cast<s_word>(static_cast<s_hword>(static_cast<u_hword>(src)));
                                u_word up = static_cast<u_word>(p);
                                u_word lo = up & 0xFFFFFFFF; u_word hi = (up >> 32) & 0xFFFFFFFF;
                                z = (lo == 0 && hi == 0); n = hi & 0x80000000; c = hi != 0; v = (p < INT32_MIN || p > INT32_MAX); break;
                            }
                            case 8: {
                                __int128 p = static_cast<__int128>(static_cast<s_word>(dst_before)) * static_cast<__int128>(static_cast<s_word>(src));
                                unsigned __int128 up = static_cast<unsigned __int128>(p);
                                u_word lo = static_cast<u_word>(up); u_word hi = static_cast<u_word>(up >> 64);
                                z = (lo == 0 && hi == 0); n = hi & 0x8000000000000000; c = hi != 0; v = (p < static_cast<__int128>(INT64_MIN) || p > static_cast<__int128>(INT64_MAX)); break;
                            }
                        }
                        break;
                    }

                    /* UMULW: unsigned wide multiply (card maize-7). V == C == (high half nonzero). */
                    case instr::umulw_opcode: {
                        switch (op_size) {
                            case 1: {
                                u_qword up = static_cast<u_qword>(static_cast<u_byte>(dst_before)) * static_cast<u_qword>(static_cast<u_byte>(src));
                                u_word lo = up & 0xFF; u_word hi = (up >> 8) & 0xFF;
                                z = (lo == 0 && hi == 0); n = hi & 0x80; c = hi != 0; v = hi != 0; break;
                            }
                            case 2: {
                                u_hword up = static_cast<u_hword>(static_cast<u_qword>(dst_before)) * static_cast<u_hword>(static_cast<u_qword>(src));
                                u_word lo = up & 0xFFFF; u_word hi = (up >> 16) & 0xFFFF;
                                z = (lo == 0 && hi == 0); n = hi & 0x8000; c = hi != 0; v = hi != 0; break;
                            }
                            case 4: {
                                u_word up = static_cast<u_word>(static_cast<u_hword>(dst_before)) * static_cast<u_word>(static_cast<u_hword>(src));
                                u_word lo = up & 0xFFFFFFFF; u_word hi = (up >> 32) & 0xFFFFFFFF;
                                z = (lo == 0 && hi == 0); n = hi & 0x80000000; c = hi != 0; v = hi != 0; break;
                            }
                            case 8: {
                                unsigned __int128 up = static_cast<unsigned __int128>(dst_before) * static_cast<unsigned __int128>(src);
                                u_word lo = static_cast<u_word>(up); u_word hi = static_cast<u_word>(up >> 64);
                                z = (lo == 0 && hi == 0); n = hi & 0x8000000000000000; c = hi != 0; v = hi != 0; break;
                            }
                        }
                        break;
                    }

                    /* Pure z/n ops: logic (AND/TEST/TSTIND, OR, NOR, NAND, XOR), signed &
                       unsigned DIV/MOD, and NOT all set only Z/N from the result and clear
                       C/V. `result` is stored zero-extended, so z/n read it directly. */
                    case instr::and_opcode:
                    case instr::test_opcode:
                    case instr::testind_opcode:
                    case instr::or_opcode:
                    case instr::nor_opcode:
                    case instr::nand_opcode:
                    case instr::xor_opcode:
                    case instr::div_opcode:
                    case instr::mod_opcode:
                    case instr::udiv_opcode:
                    case instr::umod_opcode:
                    case alu_uop_not: {
                        switch (op_size) {
                            case 1: { u_byte r = static_cast<u_byte>(result); z = r == 0; n = r & 0x80; break; }
                            case 2: { u_qword r = static_cast<u_qword>(result); z = r == 0; n = r & 0x8000; break; }
                            case 4: { u_hword r = static_cast<u_hword>(result); z = r == 0; n = r & 0x80000000; break; }
                            case 8: { u_word r = result; z = r == 0; n = r & 0x8000000000000000; break; }
                        }
                        break;
                    }

                    /* NEG (card maize-64): SUB with a zero minuend. C set iff x != 0; V set
                       only when x is the width's INT_MIN. dst_before stores x. */
                    case alu_uop_neg: {
                        switch (op_size) {
                            case 1: { u_byte x = static_cast<u_byte>(dst_before), r = static_cast<u_byte>(result);
                                z = r == 0; n = r & 0x80; c = r != 0; v = (x & r) & 0x80; break; }
                            case 2: { u_qword x = static_cast<u_qword>(dst_before), r = static_cast<u_qword>(result);
                                z = r == 0; n = r & 0x8000; c = r != 0; v = (x & r) & 0x8000; break; }
                            case 4: { u_hword x = static_cast<u_hword>(dst_before), r = static_cast<u_hword>(result);
                                z = r == 0; n = r & 0x80000000; c = r != 0; v = (x & r) & 0x80000000; break; }
                            case 8: { u_word x = dst_before, r = result;
                                z = r == 0; n = r & 0x8000000000000000; c = r != 0; v = (x & r) & 0x8000000000000000; break; }
                        }
                        break;
                    }

                    /* SHL (card maize-1). count == 0 leaves flags unaffected and is never
                       staged; the guard makes that defensive (RF untouched, no store). */
                    case instr::shl_opcode: {
                        if (src == 0) { pending_flags.dirty = false; return; }
                        switch (op_size) {
                            case 1: { u_byte dbf = static_cast<u_byte>(dst_before); u_word nn = src; const u_word bits = 8;
                                if (nn <= bits) { u_byte r = static_cast<u_byte>(result); z = r == 0; n = r & 0x80;
                                    c = (dbf >> (bits - nn)) & 1; v = (nn == 1) && (((dbf >> (bits - 1)) & 1) != ((dbf >> (bits - 2)) & 1)); }
                                else { z = true; } break; }
                            case 2: { u_qword dbf = static_cast<u_qword>(dst_before); u_word nn = src; const u_word bits = 16;
                                if (nn <= bits) { u_qword r = static_cast<u_qword>(result); z = r == 0; n = r & 0x8000;
                                    c = (dbf >> (bits - nn)) & 1; v = (nn == 1) && (((dbf >> (bits - 1)) & 1) != ((dbf >> (bits - 2)) & 1)); }
                                else { z = true; } break; }
                            case 4: { u_hword dbf = static_cast<u_hword>(dst_before); u_word nn = src; const u_word bits = 32;
                                if (nn <= bits) { u_hword r = static_cast<u_hword>(result); z = r == 0; n = r & 0x80000000;
                                    c = (dbf >> (bits - nn)) & 1; v = (nn == 1) && (((dbf >> (bits - 1)) & 1) != ((dbf >> (bits - 2)) & 1)); }
                                else { z = true; } break; }
                            case 8: { u_word dbf = dst_before; u_word nn = src; const u_word bits = 64;
                                if (nn <= bits) { u_word r = result; z = r == 0; n = r & 0x8000000000000000;
                                    c = (dbf >> (bits - nn)) & 1; v = (nn == 1) && (((dbf >> (bits - 1)) & 1) != ((dbf >> (bits - 2)) & 1)); }
                                else { z = true; } break; }
                        }
                        break;
                    }

                    /* SHR (card maize-1). Same count edge cases as SHL. */
                    case instr::shr_opcode: {
                        if (src == 0) { pending_flags.dirty = false; return; }
                        switch (op_size) {
                            case 1: { u_byte dbf = static_cast<u_byte>(dst_before); u_word nn = src; const u_word bits = 8;
                                if (nn <= bits) { u_byte r = static_cast<u_byte>(result); z = r == 0; n = r & 0x80;
                                    c = (dbf >> (nn - 1)) & 1; v = (nn == 1) && (((dbf >> (bits - 1)) & 1) != 0); }
                                else { z = true; } break; }
                            case 2: { u_qword dbf = static_cast<u_qword>(dst_before); u_word nn = src; const u_word bits = 16;
                                if (nn <= bits) { u_qword r = static_cast<u_qword>(result); z = r == 0; n = r & 0x8000;
                                    c = (dbf >> (nn - 1)) & 1; v = (nn == 1) && (((dbf >> (bits - 1)) & 1) != 0); }
                                else { z = true; } break; }
                            case 4: { u_hword dbf = static_cast<u_hword>(dst_before); u_word nn = src; const u_word bits = 32;
                                if (nn <= bits) { u_hword r = static_cast<u_hword>(result); z = r == 0; n = r & 0x80000000;
                                    c = (dbf >> (nn - 1)) & 1; v = (nn == 1) && (((dbf >> (bits - 1)) & 1) != 0); }
                                else { z = true; } break; }
                            case 8: { u_word dbf = dst_before; u_word nn = src; const u_word bits = 64;
                                if (nn <= bits) { u_word r = result; z = r == 0; n = r & 0x8000000000000000;
                                    c = (dbf >> (nn - 1)) & 1; v = (nn == 1) && (((dbf >> (bits - 1)) & 1) != 0); }
                                else { z = true; } break; }
                        }
                        break;
                    }

                    /* SAR (card maize-54). V is always 0 (an arithmetic shift can never
                       flip the sign). n>=bits saturates to the sign fill; C = the shifted-
                       out bit for n<bits, the operand sign for n>=bits. */
                    case instr::sar_opcode: {
                        if (src == 0) { pending_flags.dirty = false; return; }
                        switch (op_size) {
                            case 1: { u_byte dbf = static_cast<u_byte>(dst_before); u_word nn = src; const u_word bits = 8;
                                bool sign = (dbf >> (bits - 1)) & 1; u_byte r = static_cast<u_byte>(result);
                                z = r == 0; n = r & 0x80; c = (nn < bits) ? ((dbf >> (nn - 1)) & 1) : sign; v = false; break; }
                            case 2: { u_qword dbf = static_cast<u_qword>(dst_before); u_word nn = src; const u_word bits = 16;
                                bool sign = (dbf >> (bits - 1)) & 1; u_qword r = static_cast<u_qword>(result);
                                z = r == 0; n = r & 0x8000; c = (nn < bits) ? ((dbf >> (nn - 1)) & 1) : sign; v = false; break; }
                            case 4: { u_hword dbf = static_cast<u_hword>(dst_before); u_word nn = src; const u_word bits = 32;
                                bool sign = (dbf >> (bits - 1)) & 1; u_hword r = static_cast<u_hword>(result);
                                z = r == 0; n = r & 0x80000000; c = (nn < bits) ? ((dbf >> (nn - 1)) & 1) : sign; v = false; break; }
                            case 8: { u_word dbf = dst_before; u_word nn = src; const u_word bits = 64;
                                bool sign = (dbf >> (bits - 1)) & 1; u_word r = result;
                                z = r == 0; n = r & 0x8000000000000000; c = (nn < bits) ? ((dbf >> (nn - 1)) & 1) : sign; v = false; break; }
                        }
                        break;
                    }
                }

                u_word f = regs::rf.w0;
                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                    | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                regs::rf.w0 = f;
                pending_flags.dirty = false;
            }

            void copy_regval_regaddr(reg_value const &src, subreg_enum src_subreg, reg_value const &dst, subreg_enum dst_subreg) {
                auto src_offset = subreg_offset_map[static_cast<size_t>(src_subreg)];
                auto src_mask = subreg_mask_map[static_cast<size_t>(src_subreg)];
                auto size = subreg_size_map[static_cast<size_t>(src_subreg)];

                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];

                reg_value src_data;
                src_data.w0 = static_cast<s_word>((src.w0 & static_cast<u_word>(src_mask)) >> src_offset);
                u_word dst_address = translate((dst.w0 & static_cast<u_word>(dst_mask)) >> dst_offset, access_kind::store);

                switch (size) {
                    case 1: {
                        mm.write_byte(dst_address, src_data.b0());
                        break;
                    }

                    case 2: {
                        mm.write_qword(dst_address, src_data.q0());
                        break;
                    }

                    case 4: {
                        mm.write_hword(dst_address, src_data.h0());
                        break;
                    }

                    case 8: {
                        mm.write_word(dst_address, src_data.w0);
                        break;
                    }

                    default:
                        break;
                }
            }

            void copy_memval_regaddr(u_word address, size_t size, reg_value const &dst, subreg_enum dst_subreg) {
                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];

                reg_value src_data;
                mm.read(translate(address, access_kind::fetch), src_data, size, 0);
                u_word dst_address = translate((dst.w0 & static_cast<u_word>(dst_mask)) >> dst_offset, access_kind::store);

                switch (size) {
                    case 1: {
                        mm.write_byte(dst_address, src_data.b0());
                        break;
                    }

                    case 2: {
                        mm.write_qword(dst_address, src_data.q0());
                        break;
                    }

                    case 4: {
                        mm.write_hword(dst_address, src_data.h0());
                        break;
                    }

                    case 8: {
                        mm.write_word(dst_address, src_data.w0);
                        break;
                    }

                    default:
                        break;
                }
            }

            std::mutex int_mutex;
            std::condition_variable int_event;

            std::mutex io_set_mutex;
            std::condition_variable io_set_event;

            reg operand1;
            reg operand2;

            /* Control-register file, software TLB, and page-fault-delivery state (cards
               maize-180 / maize-194) are declared ABOVE the choke-point memory helpers so
               translate()'s inline Bare-mode fast path is visible before its first call
               site (see that block, maize-194 review #2605, nit/perf 5). */

            bool is_power_on = false;

            /* --show-perf instrumentation. perf_count_enabled gates the per-instruction
               counter so a normal run pays nothing but a predicted-not-taken branch; it is
               set once by enable_perf_counter(). perf_insn_count is a plain 64-bit counter
               written only by the CPU thread and read (relaxed, aligned-atomic on the host)
               by the display thread for the MIPS readout. */
            bool perf_count_enabled = false;
            u_word perf_insn_count = 0;

            /* Flat interrupt controller state (card maize-21). irq_pending is the single
               DURABLE pending-vector latch and the authoritative delivery signal: it is
               std::atomic so the run loop's lock-free fast path can read it race-free, and
               it survives a handler return (unlike the RF bit_interrupt_set mirror, which
               IRET pops back to its saved value). Writes happen under int_mutex. Multiple
               raises coalesce to this one latch (the flat model promises no queue). The RF
               interrupt_set_flag is kept as the ISA-visible mirror but is advisory only;
               delivery never gates on it. active_timer_ptr is the instruction-tick timer
               the run loop advances once per executed instruction; it is null until a
               timer is installed, so the machine and every existing fixture run with no
               interrupt source. */
            std::atomic<bool> irq_pending {false};
            u_byte irq_pending_vector = 0;
            timer_device* active_timer_ptr = nullptr;

            /* The single active instruction-tick input source (device-plugin API). null
               unless a headless stdin-injection device is selected as the sole stdin consumer;
               the run loop then calls its on_input_tick() per executed instruction to pull
               bytes and raise its IRQ on the CPU thread. A windowed keyboard leaves this null
               and is driven off-thread (push_event latches + raises; port_read drains). */
            input_device* active_input_ptr = nullptr;
        }

        bus address_bus;
        bus data_bus_0;
        bus data_bus_1;
        bus io_bus;

        memory_module mm;
        arithmetic_logic_unit alu;

        void add_device(u_qword id, device& new_device) {
            devices[id] = &new_device;
        }

        /* Device base hooks (device-plugin API). The defaults reproduce the shipped
           passive-register passthrough exactly: on_port_write does the sign-extending copy
           of the CPU-side value into the backing reg's w0, and on_port_read returns that
           w0. A plain `device` (loopback, the timer's three registers, the reserved block
           ports) therefore behaves identically to before this seam existed; a host-backed
           device overrides these to act at the port access. */
        void device::on_port_write(reg_value const& value, subreg_enum value_subreg) {
            copy_regval_reg(value, value_subreg, *this, subreg_enum::w0);
        }

        reg_value device::on_port_read(subreg_enum /*dst_subreg*/) {
            reg_value out;
            out.w0 = this->w0;
            return out;
        }

        /* Zero-extended value of a named subregister from a port operand. Exposed so a
           host-backed device hook can read the guest's written value as a plain u_word
           without reaching into cpu.cpp's private subreg helpers. */
        u_word port_value_bits(reg_value const& value, subreg_enum value_subreg) {
            return read_subreg_bits(value, value_subreg);
        }

        /* Single shared port-table lookup (card maize-21). Every IN / OUT / OUTR
           dispatch site routes through this one helper so no form retains the old
           devices[id] value-initialize-null-then-dereference path. The port operand is
           masked to the 16-bit port space in one place; a map miss returns nullptr, and
           the caller applies the frozen read-0 (IN) / write-discard (OUT/OUTR) outcome
           for an unpopulated port. */
        device* find_device(u_word port) {
            auto it = devices.find(static_cast<u_qword>(port & 0xFFFF));
            return it == devices.end() ? nullptr : it->second;
        }

        /* OUT / OUTR data transfer (card maize-21). Writes the CPU-side operand value
           into the selected device's data register (the full w0 width per the shipped
           dispatch). An unpopulated port is the frozen write-discard no-op; the caller
           still advances PC past its operands exactly as the populated path does. */
        void port_write(u_word port, reg_value const& value, subreg_enum value_subreg) {
            device* pdev = find_device(port);
            if (pdev == nullptr) {
                return;
            }
            pdev->on_port_write(value, value_subreg);
        }

        /* IN data transfer (card maize-21). Reads the selected device's data register
           into the destination sub-register. An unpopulated port yields the frozen
           read-0 outcome (the destination receives 0; no device is touched). */
        void port_read(u_word port, reg_value& dst, subreg_enum dst_subreg) {
            device* pdev = find_device(port);
            if (pdev == nullptr) {
                clr_reg(dst, dst_subreg);
                return;
            }
            reg_value v = pdev->on_port_read(dst_subreg);
            copy_regval_reg(v, subreg_enum::w0, dst, dst_subreg);
        }

        /* MOVTCR data transfer (card maize-180). Store the CPU-side operand value into
           control register CR[crn]. CR0 (SATP): the MODE field [3:0] is accepted and
           stored but INERT under this card (no translation runs regardless of MODE); the
           PPN field [63:12] is stored and readable but unused; the reserved bits [11:4]
           are forced to 0 (spec §1). A CR0 write unconditionally flushes the software TLB
           when one exists; under this card that is a no-op (no TLB exists yet, maize-194
           builds it). An undefined CR index (> 2) is the frozen unpopulated-port
           write-discard no-op; the caller still advances PC past its operands. */
        void cr_write(u_word crn, reg_value const& value, subreg_enum value_subreg) {
            if (crn >= control_reg_count) {
                return;
            }
            copy_regval_reg(value, value_subreg, control_regs[crn], subreg_enum::w0);
            if (crn == 0) {
                /* SATP reserved bits [11:4] read as zero: mask them off on write. */
                control_regs[0].w0 &= ~static_cast<u_word>(0x0FF0);
                /* A CR0 (SATP) write changes the address space, so every cached
                   translation is now potentially stale: flush the whole software TLB
                   (card maize-194). */
                tlb_flush_all();
            }
        }

        /* MOVFCR data transfer (card maize-180). Copy control register CR[crn] into the
           destination sub-register. An undefined CR index (> 2) yields the frozen
           unpopulated-port read-0 outcome (the destination receives 0; no CR is read). */
        void cr_read(u_word crn, reg_value& dst, subreg_enum dst_subreg) {
            if (crn >= control_reg_count) {
                clr_reg(dst, dst_subreg);
                return;
            }
            copy_regval_reg(control_regs[crn], subreg_enum::w0, dst, dst_subreg);
        }

        /* Deterministic no-handler halt for a vectored interrupt (card maize-21). An
           enabled IRQ whose table entry is zero (uninstalled) halts the VM with the
           cause surfaced, mirroring the no-handler synchronous-trap rule, never a silent
           ignore or an out-of-bounds read. */
        [[noreturn]] void halt_no_interrupt_handler(u_byte vector) {
            throw std::logic_error(std::string("unhandled interrupt: vector ")
                + std::to_string(static_cast<int>(vector)) + ", no handler installed");
        }

        /* Read entry[vector] from the shared vector table. The index is a u_byte, so it
           is always within the 256-entry table (the index-bounds guarantee); each entry
           is a full 64-bit handler address. A zero entry means uninstalled. */
        u_word read_vector_entry(u_byte vector) {
            u_word addr = trap_vector_table_base + static_cast<u_word>(vector) * trap_vector_entry_size;
            reg entry;
            entry.w0 = 0;
            mm.read(addr, entry, static_cast<size_t>(trap_vector_entry_size), 0);
            return entry.w0;
        }

        /* Push a full 64-bit word onto the full-descending stack (pre-decrement RS by the
           stack word size, then store), matching the PUSH / CALL / RET convention. The
           decrement uses the w0 sub-register size, a stack property, rather than the
           vector-table entry width (which merely happens to be the same 8 bytes). */
        void push_word(u_word value) {
            regs::rs.w0 -= subreg_size_map[static_cast<size_t>(subreg_enum::w0)];
            reg tmp;
            tmp.w0 = value;
            copy_regval_regaddr(tmp, subreg_enum::w0, regs::rs, subreg_enum::w0);
        }

        /* Build the shared four-word trap/interrupt frame and enter the handler (card
           maize-21). Push order is PC, then RF, then cause, then aux, so the frame reads
           from SP upward: aux, cause, RF, PC. cause packs the vector index in the low
           byte and the subcode in the next byte (bits 63:16 reserved-zero); aux is the
           source subcode word (0 for the timer). The saved RF is the pre-interrupt RF,
           captured before interrupts are masked, so a normal IRET return re-enables
           interrupts automatically. */
        void deliver_vectored(u_byte vector, u_byte subcode, u_word aux, u_word saved_pc) {
            u_word handler = read_vector_entry(vector);
            if (handler == 0) {
                halt_no_interrupt_handler(vector);
            }
            /* card maize-197: resolve deferred flags before snapshotting RF into the
               trap frame, or IRET would later restore stale Z/N/C/V into the
               interrupted context. */
            materialize_flags();
            u_word saved_rf = regs::rf.w0;

            /* User -> supervisor transition on trap entry (card maize-180, §6). Force the
               privilege bit true for the handler AFTER capturing saved_rf, so the frame
               still carries the interrupted code's own privilege; the handler's IRET
               restores it (dropping back to user when the saved privilege bit is clear).
               Every trap/interrupt handler therefore runs supervisor and can execute the
               now-privileged IRET; this is what makes gating IRET (§9) safe. */
            privilege_flag = true;

            u_word cause = static_cast<u_word>(vector) | (static_cast<u_word>(subcode) << 8);

            push_word(saved_pc);   // SP + 24
            push_word(saved_rf);   // SP + 16
            push_word(cause);      // SP + 8
            push_word(aux);        // SP + 0  (RS points here on handler entry)

            /* Mask: the handler runs with interrupts disabled until IRET restores the
               saved RF (or an explicit SETINT). Cleared after the frame is built so the
               saved RF above still carries the pre-interrupt enable bit. */
            interrupt_enabled_flag = false;

            regs::rp.w0 = handler;
        }

        /* ---- Sv48 address translation, software TLB, page fault (card maize-194) ----
           Defined here, below deliver_vectored (raise_page_fault vectors a cause-8 fault
           through it) and below control_regs / software_tlb / privilege_flag (all in the
           anonymous namespace above, visible here in the enclosing namespace). translate()
           and the two TLB-flush helpers were forward-declared before the choke-point
           helpers so every translated access site can call them by name. */

        /* Thrown by raise_page_fault once deliver_vectored has installed the cause-8 trap
           frame and redirected RP to the guest handler: it unwinds the aborted faulting
           instruction back to run(), which re-enters tick() at the handler. Distinct from
           the std::logic_error faults, which propagate to main() and exit the VM; a page
           fault instead runs the guest handler. */
        struct page_fault_redirect {};

        /* Raw physical PTE read. Mirrors read_vector_entry: always physical, NEVER through
           translate(), so the walker never recursively translates the page tables it is
           walking (spec exclusion). */
        u_word read_pte(u_word pa) {
            reg entry;
            entry.w0 = 0;
            mm.read(pa, entry, 8, 0);
            return entry.w0;
        }

        /* Latch CR1 FAULT_VA / CR2 FAULT_ERR and vector a cause-8 page fault through the
           trap table so a guest handler can run (spec item 5). Page fault is FAULT-class:
           the saved PC is the faulting instruction's own entry PC (current_instr_pc),
           captured at the top of MAIZE_NEXT before the opcode fetch, not the advanced PC,
           so a handler's IRET re-executes the faulting instruction. A fault raised while a
           trap frame is already being pushed (delivering_trap set) is a double fault: halt
           deterministically rather than recurse (spec item 5, narrow guard scoped to the
           frame-push). CR2 layout: bit0 PRESENT (0=no mapping, 1=perm violation),
           bits[2:1] ACCESS_KIND (fetch/load/store), bit3 USER, bits[63:4] reserved-zero. */
        [[noreturn]] void raise_page_fault(u_word va, access_kind kind, bool present) {
            if (delivering_trap) {
                std::stringstream err {};
                err << "double fault: page fault at VA 0x" << std::hex << va
                    << " during trap-frame delivery (cause "
                    << std::dec << static_cast<int>(trap::cause_page_fault) << ")";
                throw std::logic_error(err.str());
            }
            control_regs[1].w0 = va;                                        // CR1 FAULT_VA
            control_regs[2].w0 = (present ? u_word {1} : u_word {0})        // bit0 PRESENT
                | (static_cast<u_word>(kind) << 1)                          // bits[2:1] ACCESS_KIND
                | (privilege_flag ? u_word {0} : (u_word {1} << 3));        // bit3 USER
            delivering_trap = true;
            deliver_vectored(trap::cause_page_fault, 0, 0, current_instr_pc);
            delivering_trap = false;
            throw page_fault_redirect {};
        }

        /* Permission check for a leaf mapping: required bit is X for a fetch, R for a load,
           W for a store; a user-mode access (privilege_flag clear) also needs U. Supervisor
           bypasses the U check (no SUM-equivalent). A and D are software-managed and never
           consulted. */
        bool leaf_permits(access_kind kind, bool r, bool w, bool x, bool u) {
            bool kind_ok = (kind == access_kind::fetch) ? x
                         : (kind == access_kind::load)  ? r
                         :                                w;   // store
            if (!kind_ok) { return false; }
            if (!privilege_flag && !u) { return false; }
            return true;
        }

        /* Sv48 4-level walk (card maize-194). 9 VPN index bits per level, 4 KiB pages,
           8-byte PTEs (Maize-native packing: PPN in bits [63:12], flags V/R/W/X/U/A/D
           mirroring RISC-V Sv48). Leaf iff R|W|X != 0; non-leaf iff V=1 & R=W=X=0; V=0 is
           no mapping. Returns the physical address on success; raise_page_fault (which
           never returns) on any failure. A successful level-0 4 KiB leaf is installed into
           the software TLB; superpage leaves (level 1-3) are used but never cached. */
        u_word sv48_walk(u_word va, access_kind kind) {
            /* VA[63:48] is ignored: no canonical-form check, no fault (decision). */
            u_word table_base = control_regs[0].w0 & ~static_cast<u_word>(0xFFF);
            for (int level = 3; level >= 0; --level) {
                u_word vpn = (va >> (12 + level * 9)) & static_cast<u_word>(0x1FF);
                u_word pte = read_pte(table_base + vpn * 8);
                if ((pte & 0x1) == 0) {
                    raise_page_fault(va, kind, false);   // V=0: no mapping
                }
                bool r = pte & 0x2, w = pte & 0x4, x = pte & 0x8, u = pte & 0x10;
                if (r || w || x) {
                    /* Leaf. W-without-R is a reserved PTE encoding in RISC-V; reject it as
                       an invalid mapping (no valid mapping found) before any permission or
                       address use, rather than honoring it as a write-only page (maize-194
                       review #2605, nit 3). */
                    if (w && !r) {
                        raise_page_fault(va, kind, false);   // reserved W=1/R=0: invalid PTE
                    }
                    u_word offset_mask = (u_word {1} << (12 + level * 9)) - 1;
                    /* Misaligned superpage: a leaf above level 0 must have zero PPN bits
                       below its level's page boundary. A non-zero low PPN is a fault, not an
                       aliased mapping (maize-194 review #2605, minor 2). offset_mask & ~0xFFF
                       selects the PPN bits [12 .. 12 + level*9) that must be clear; at level 0
                       that mask is 0, so 4 KiB leaves are never rejected here. */
                    if ((pte & (offset_mask & ~static_cast<u_word>(0xFFF))) != 0) {
                        raise_page_fault(va, kind, false);   // misaligned superpage: invalid PTE
                    }
                    if (!leaf_permits(kind, r, w, x, u)) {
                        raise_page_fault(va, kind, true);   // mapping present, perm violation
                    }
                    u_word pa = (pte & ~static_cast<u_word>(0xFFF)) | (va & offset_mask);
                    if (level == 0) {
                        /* Only a 4 KiB leaf is cached; superpages never (decision). */
                        size_t idx = (va >> 12) & (tlb_size - 1);
                        software_tlb[idx].valid = true;
                        software_tlb[idx].tag = (va >> 12) & vpn_tag_mask;
                        software_tlb[idx].ppn = pte & ~static_cast<u_word>(0xFFF);
                        software_tlb[idx].r = r;
                        software_tlb[idx].w = w;
                        software_tlb[idx].x = x;
                        software_tlb[idx].u = u;
                    }
                    return pa;
                }
                /* Non-leaf (V=1, R=W=X=0): descend. At level 0 there is nothing left to
                   descend to, so treat it as not-present rather than walking off the end. */
                if (level == 0) {
                    raise_page_fault(va, kind, false);
                }
                table_base = pte & ~static_cast<u_word>(0xFFF);
            }
            raise_page_fault(va, kind, false);   // unreachable: level 0 always returns/faults
        }

        /* Out-of-line slow path for translate() (card maize-194): reached only under Sv48
           (MODE == 1; the inline translate() wrapper handles the Bare-mode identity fast
           path). Checks the software TLB, then walks on a miss. */
        u_word translate_slow(u_word va, access_kind kind) {
            size_t idx = (va >> 12) & (tlb_size - 1);
            tlb_entry const& e = software_tlb[idx];
            if (e.valid && e.tag == ((va >> 12) & vpn_tag_mask)) {
                /* Hit: re-check permission against the cached bits, since privilege/kind
                   can differ access-to-access on the same cached page. */
                if (!leaf_permits(kind, e.r, e.w, e.x, e.u)) {
                    raise_page_fault(va, kind, true);
                }
                return e.ppn | (va & static_cast<u_word>(0xFFF));
            }
            return sv48_walk(va, kind);
        }

        /* Invalidate the whole software TLB: called from cr_write on any CR0 (SATP) write
           and from TLBINV (card maize-194). */
        void tlb_flush_all() {
            for (size_t i = 0; i < tlb_size; ++i) {
                software_tlb[i].valid = false;
            }
        }

        /* Invalidate the single TLB entry that would cache `va`, if present: called from
           TLBINVA Rn (card maize-194). */
        void tlb_flush_va(u_word va) {
            size_t idx = (va >> 12) & (tlb_size - 1);
            if (software_tlb[idx].valid && software_tlb[idx].tag == ((va >> 12) & vpn_tag_mask)) {
                software_tlb[idx].valid = false;
            }
        }

        /* Single delivery seam (card maize-21). Checked at the instruction boundary in
           the run loop. The gate is the DURABLE controller latch irq_pending, not the RF
           interrupt_set_flag mirror: IRET pops the whole RF word, so the mirror bit is
           restored to whatever the frame saved and cannot be relied on across a handler
           return. An IRQ raised while a handler ran masked sets irq_pending; once the
           handler's IRET restores the enable bit, this gate delivers it. The fast path is
           a lock-free atomic read plus an RF-bit read; only when an IRQ is actually
           pending AND interrupts are enabled does it take int_mutex. Acknowledge-on-
           delivery clears the latch BEFORE handler entry, so the same IRQ is not
           re-delivered on IRET. Returns true when an interrupt was delivered (the run
           loop then continues to the handler's first instruction). */
        bool try_deliver_interrupt() {
            if (!interrupt_enabled_flag || !irq_pending.load(std::memory_order_relaxed)) {
                return false;
            }
            u_byte vector;
            {
                std::lock_guard<std::mutex> lk(int_mutex);
                if (!irq_pending || !interrupt_enabled_flag) {
                    return false;
                }
                vector = irq_pending_vector;
                irq_pending = false;
                interrupt_set_flag = false;
            }
            /* External interrupts resume at the boundary instruction, so the saved PC is
               the next instruction that would have run (no faulting-instruction stash). */
            deliver_vectored(vector, 0, 0, regs::rp.w0);
            return true;
        }

        void set_active_timer(timer_device* timer) {
            active_timer_ptr = timer;
        }

        void set_active_input(input_device* src) {
            active_input_ptr = src;
        }

        /* maize-140: idle-park hooks used by a blocking console read (see the
           declaration in maize_cpu.h). set_running toggles the run bit under int_mutex
           (the same latch HALT parks with); the console clears it while waiting on a
           keypress so the MIPS readout idles and there is no busy-spin, then sets it
           back before the SYS read returns. No notify is needed: the console's own
           condition variable (or a blocking host stdin read) governs the wake, and the
           run loop only re-reads the run bit after tick() returns, which cannot happen
           while the CPU thread is parked inside the syscall.

           maize-175: the un-park (set_running(true)) is gated on is_power_on so it can
           never re-animate a core that has already been powered off. The window-close
           shutdown path calls con.stop() (which wakes a queue-parked read) and then
           power_off() (running_flag=false, is_power_on=false) back to back. The woken
           read re-acquires q_mutex_ and calls set_running(true), which must take
           int_mutex, the very lock power_off() is holding, so set_running(true) reliably
           runs AFTER power_off(). Without this gate it would set running_flag=true again
           while is_power_on is false; tick() only tests running_flag (never is_power_on),
           so the guest would resume executing (read() returning 0 in a tight loop) and
           the process would orphan with its window gone. Gating on is_power_on makes the
           lagging un-park a no-op, so the guest falls out of tick() and run() returns. */
        void set_running(bool on) {
            std::lock_guard<std::mutex> lk(int_mutex);
            running_flag = on && is_power_on;
        }

        /* A device raises an IRQ by making a vector pending (card maize-21). The flat
           controller coalesces multiple raises to the single pending-vector latch
           (last-raise-wins). Taking int_mutex and notifying int_event makes delivery
           race-free for both a running core (checked at the instruction boundary) and a
           core waiting on int_event.

           Precondition: vector is an external-interrupt vector in [32, 255]; the
           synchronous-trap range 0..31 is not an IRQ source and must not be raised here
           (a sub-32 vector would deliver through a trap slot with an IRQ cause packing).

           Threading constraint: today the only caller is the instruction-tick timer,
           which runs on the CPU thread, so the interrupt_set_flag RF-mirror write below
           is safe. A host-thread device backend that calls this seam would race the CPU
           thread's unsynchronized RF accesses on that mirror bit; the durable latch
           (irq_pending, atomic) that delivery actually gates on is race-free, and the
           full cross-thread RF synchronization lands with the device-plugin work. */
        void raise_irq(u_byte vector) {
            assert(vector >= 32 && "raise_irq: vector must be an external-interrupt vector (32..255)");
            std::lock_guard<std::mutex> lk(int_mutex);
            irq_pending_vector = vector;
            irq_pending = true;
            interrupt_set_flag = true;
            int_event.notify_all();
        }

        /* Advance the instruction-tick timer one tick (card maize-21). Called once per
           executed instruction from the run loop. While a tick is pending (awaiting the
           handler's acknowledge), the countdown is paused so handler instructions do not
           over-count. On reaching zero it sets the tick-pending status bit and raises its
           IRQ; a periodic timer re-arms after the acknowledge clears the pending bit,
           while a one-shot timer clears its own enable bit.

           maize-200: hoisted from an out-of-line timer_device member into this
           static-inline namespace helper so the common case (no active timer, or an armed
           timer that has not yet expired) folds directly into the MAIZE_NEXT preamble
           instead of paying a per-instruction out-of-line call. The state machine is
           preserved bit-for-bit; only the call overhead is removed. raise_irq (which
           takes int_mutex) still fires only on the rare tick-complete path, as before. */
        static inline void tick_active_timer(timer_device &t) {
            bool enable = (t.control_reg.w0 & 0x1) != 0;
            if (!enable) {
                return;
            }
            if ((t.status_reg.w0 & 0x1) != 0) {
                /* Tick pending, waiting for the handler's ack: paused. */
                return;
            }
            if (t.counter == 0) {
                /* Freshly programmed or re-armed after an ack: (re)load the period. A
                   zero period is inert (an unprogrammed / disabled countdown). */
                t.counter = t.period_reg.w0;
                if (t.counter == 0) {
                    return;
                }
            }
            --t.counter;
            if (t.counter == 0) {
                t.status_reg.w0 |= 0x1;   // set tick-pending
                raise_irq(t.irq_vector);
                bool periodic = (t.control_reg.w0 & 0x2) != 0;
                if (!periodic) {
                    t.control_reg.w0 &= ~static_cast<u_word>(0x1);   // one-shot: disable
                }
                /* Periodic: counter stays 0 and reloads on the tick after the ack clears
                   the pending bit. */
            }
        }

        /* Divide-by-zero and signed INT_MIN/-1 overflow are divide-error traps (card maize-5).
           Until the interrupt mechanism exists, halt cleanly by throwing rather than invoking
           C++ undefined behavior; this matches the unknown-opcode handler's shape. */
        [[noreturn]] void raise_divide_error(const char* detail) {
            throw std::logic_error(std::string("divide error: ") + detail);
        }

        /* Breakpoint trap (card maize-78, Open Question O7, superseding maize-10
           Decision D6460). BRK ($FF) is a defined breakpoint trap of cause
           trap::cause_breakpoint (3), NOT a no-op. It is trap-class: it captures the
           following-instruction PC, which regs::rp.w0 already holds because tick()
           advanced past the single-byte opcode before dispatch. Until the maize-21
           vector table exists there is no handler to enter, so an unhandled breakpoint
           halts the VM deterministically with the cause surfaced, exactly as
           raise_divide_error halts an unhandled divide-error. This is the specified
           successor to today's throw-and-exit; the clean in-guest halt lands with the
           maize-21 delivery mechanism. */
        [[noreturn]] void raise_breakpoint() {
            throw std::logic_error(std::string("breakpoint trap: BRK ($FF), cause ")
                + std::to_string(static_cast<int>(trap::cause_breakpoint)));
        }

        /* Illegal FP encoding trap (card maize-122 / maize-78 taxonomy): a B* or Q*
           subregister on an FP operand, or a reserved/unallocated FP opcode form,
           is a deterministic illegal-instruction/illegal-operand trap (never
           undefined behavior or a silent no-op). Same shape as the divide-error
           and unknown-opcode handlers until the interrupt mechanism exists. */
        [[noreturn]] void raise_illegal_fp(const char* detail) {
            throw std::logic_error(std::string("illegal floating-point instruction: ") + detail);
        }

        /* Privileged-operation fault (card maize-21, cause trap::cause_privileged_op (4)).
           IN / OUT / OUTR executed with the RF privilege bit clear (user mode) raise this
           fault. Per the external-interrupts-only scope, the synchronous cause-4 fault
           keeps the frozen throw-and-exit / deterministic-halt behavior for the
           no-handler case (as BRK does today); it does not vector through the table here.
           The diagnostic carries "privileged" so the no-handler test can assert it. */
        [[noreturn]] void raise_privileged_op() {
            throw std::logic_error(std::string("privileged operation in user mode: cause ")
                + std::to_string(static_cast<int>(trap::cause_privileged_op)));
        }

        namespace {
            /* FCSR field access (card maize-122). FRM lives in bits 7-5, FFLAGS
               (sticky) in bits 4-0, RISC-V fcsr layout. FFLAGS are set by hardware
               and cleared only by software (FSETCSR). */
            u_byte fcsr_frm() {
                return (static_cast<u_byte>(regs::fcsr.b0()) >> 5) & 0x07;
            }

            /* The rounding mode consulted by every rounding FP op. FRM encodings
               101 / 110 are reserved and 111 (DYN) is unsupported on Maize (there
               is no per-instruction rounding field); rather than silently rounding
               to nearest, a rounding op with a reserved/unsupported FRM in FCSR is a
               deterministic illegal-operand trap (card maize-122; matches RISC-V,
               which treats a reserved static rounding mode as illegal). Non-rounding
               ops (FNEG/FABS/FMIN/FMAX/FCMP) never call this and are unaffected. */
            u_byte fp_checked_frm() {
                u_byte frm = fcsr_frm();
                if (frm > 4) {
                    raise_illegal_fp("reserved / unsupported FRM rounding mode in FCSR");
                }
                return frm;
            }

            void fcsr_raise(u_byte fflag_bits) {
                regs::fcsr.set_b0(static_cast<u_byte>(regs::fcsr.b0()) | (fflag_bits & 0x1F));
            }

            /* Map an FP operand's subregister to its float width in bytes: H0/H1 =>
               4 (binary32), W0 => 8 (binary64). A B* or Q* subregister is illegal for
               an FP operand and returns 0, which every caller treats as a trap. */
            u_byte fp_width_from_subreg(subreg_enum sr) {
                switch (sr) {
                    case subreg_enum::h0:
                    case subreg_enum::h1:
                        return 4;
                    case subreg_enum::w0:
                        return 8;
                    default:
                        return 0; // B* or Q* (or undefined): illegal for FP
                }
            }

            /* FCMP flag production (card maize-122, spec 3e). `a_bits` is the dst
               operand (op2), `b_bits` the src (op1); the outcome is a-versus-b,
               mapped onto Maize's x86-shaped flags per the UCOMISD convention:
                 a > src -> C=0 Z=0 P=0    a < src -> C=1 Z=0 P=0
                 a == src -> C=0 Z=1 P=0    unordered -> C=1 Z=1 P=1
               N and V are cleared. The parity bit P is the unordered indicator
               (JP/SETP predicate). FCMP is the quiet compare: a quiet NaN yields
               unordered without signaling; only a signaling NaN raises FFLAGS.NV.
               The integer RF flags are the only ones written here; FFLAGS.NV is the
               only FCSR bit FCMP can touch. */
            void do_fcmp(u_word a_bits, u_word b_bits, u_byte width) {
                fpu::fcmp_res c = fpu::fp_cmp(a_bits, b_bits, width);
                bool C = false, Z = false, P = false;
                switch (c.out) {
                    case fpu::fcmp_out::greater:   C = false; Z = false; P = false; break;
                    case fpu::fcmp_out::less:      C = true;  Z = false; P = false; break;
                    case fpu::fcmp_out::equal:     C = false; Z = true;  P = false; break;
                    case fpu::fcmp_out::unordered: C = true;  Z = true;  P = true;  break;
                }
                u_word f = regs::rf.w0;
                f &= ~(bit_carryout | bit_zero | bit_parity | bit_negative | bit_overflow);
                if (C) f |= bit_carryout;
                if (Z) f |= bit_zero;
                if (P) f |= bit_parity;
                regs::rf.w0 = f;
                /* card maize-197: FCMP fully determines C/Z/P and clears N/V with no
                   dependency on the prior bits, bypassing run_alu. Discard any pending
                   integer-ALU descriptor so a later materialize_flags cannot clobber
                   what FCMP just wrote. */
                pending_flags.dirty = false;
                if (c.nv) {
                    fcsr_raise(fpu::fflag_nv);
                }
            }
        }

        void run_alu() {
            u_byte op_size = alu.b2(); // Destination size
            u_byte alu_op = alu.b0() & arithmetic_logic_unit::opflag_code;
            u_word alu_op2_entry = alu.op2_reg.w0; // preserved to restore for compare/test ops

            switch (alu_op) {
                case instr::add_opcode: {
                    /* Carry (C) is the unsigned carry-out; overflow (V) is the signed-overflow
                       test (same-sign operands, result sign differs). See card maize-1.
                       Flags deferred: staged here, resolved by materialize_flags (maize-197). */
                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0();
                            u_byte src = alu.op1_reg.b0();
                            u_byte result = dst_before + src;
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0();
                            u_qword src = alu.op1_reg.q0();
                            u_qword result = dst_before + src;
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0();
                            u_hword src = alu.op1_reg.h0();
                            u_hword result = dst_before + src;
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word src = alu.op1_reg.w0;
                            u_word result = dst_before + src;
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }
                    }

                    break;
                }

                case instr::cmp_opcode:
                case instr::cmpind_opcode:
                case instr::sub_opcode: {
                    /* Carry (C) is the unsigned borrow (x86 convention: C=1 means dst_before <u src);
                       overflow (V) is the signed-overflow test (operands differ in sign, result differs
                       from the minuend's sign). See card maize-1. Flags deferred (maize-197). */
                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0();
                            u_byte src = alu.op1_reg.b0();
                            u_byte result = dst_before - src;
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0();
                            u_qword src = alu.op1_reg.q0();
                            u_qword result = dst_before - src;
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0();
                            u_hword src = alu.op1_reg.h0();
                            u_hword result = dst_before - src;
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word src = alu.op1_reg.w0;
                            u_word result = dst_before - src;
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }
                    }

                    break;
                }

                case instr::adc_opcode: {
                    /* Add with carry: dst + src + C (card maize-6). C = unsigned carry-out, V =
                       signed overflow, N = sign, Z = this word's result (per-word, x86-style, so
                       multi-word chains AND the per-word Z). Carry-out uses a two-step test so the
                       64-bit width needs no 128-bit accumulator. */
                    /* card maize-197: the carry-in read is a flag CONSUMER; resolve any
                       deferred flags before reading carryout_flag, then stage ADC's own
                       descriptor (result already bakes in the carry, so no stored carry). */
                    materialize_flags();
                    unsigned carry_in = (bool)carryout_flag ? 1u : 0u;

                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0();
                            u_byte src = alu.op1_reg.b0();
                            u_byte sum1 = dst_before + src;
                            u_byte result = sum1 + static_cast<u_byte>(carry_in);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0();
                            u_qword src = alu.op1_reg.q0();
                            u_qword sum1 = dst_before + src;
                            u_qword result = sum1 + static_cast<u_qword>(carry_in);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0();
                            u_hword src = alu.op1_reg.h0();
                            u_hword sum1 = dst_before + src;
                            u_hword result = sum1 + static_cast<u_hword>(carry_in);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word src = alu.op1_reg.w0;
                            u_word sum1 = dst_before + src;
                            u_word result = sum1 + static_cast<u_word>(carry_in);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }
                    }

                    break;
                }

                case instr::sbb_opcode: {
                    /* Subtract with borrow: dst - src - C (card maize-6). C = unsigned borrow
                       (x86 convention), V = signed overflow, N = sign, Z = this word's result. */
                    /* card maize-197: the borrow-in read is a flag CONSUMER; resolve any
                       deferred flags before reading carryout_flag, then stage SBB's own. */
                    materialize_flags();
                    unsigned borrow_in = (bool)carryout_flag ? 1u : 0u;

                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0();
                            u_byte src = alu.op1_reg.b0();
                            u_byte diff1 = dst_before - src;
                            u_byte result = diff1 - static_cast<u_byte>(borrow_in);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0();
                            u_qword src = alu.op1_reg.q0();
                            u_qword diff1 = dst_before - src;
                            u_qword result = diff1 - static_cast<u_qword>(borrow_in);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0();
                            u_hword src = alu.op1_reg.h0();
                            u_hword diff1 = dst_before - src;
                            u_hword result = diff1 - static_cast<u_hword>(borrow_in);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word src = alu.op1_reg.w0;
                            u_word diff1 = dst_before - src;
                            u_word result = diff1 - static_cast<u_word>(borrow_in);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }
                    }

                    break;
                }

                case instr::mul_opcode: {
                    /* Overflow (V) is the signed-overflow test on the pre-op operands. Carry (C) mirrors
                       V until the wide-multiply card (97821c447640) lands a high-half product; see card
                       maize-1 decision. The width-8 case reads the .w0 (64-bit) subregisters that its own
                       multiply uses, not the .h0() (32-bit) subregisters. Flags deferred
                       (maize-197): materialize_flags recomputes ovf from the stored
                       operands, matching the is_mul_overflow(op1, op2) argument order. */
                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0();
                            u_byte src = alu.op1_reg.b0();
                            u_byte result = dst_before * src;
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0();
                            u_qword src = alu.op1_reg.q0();
                            u_qword result = dst_before * src;
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0();
                            u_hword src = alu.op1_reg.h0();
                            u_hword result = dst_before * src;
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word src = alu.op1_reg.w0;
                            u_word result = dst_before * src;
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, src, result);
                            break;
                        }
                    }

                    break;
                }

                case instr::mulw_opcode: {
                    /* Signed wide multiply (card maize-7). Full 2w-bit product; low w bytes go to
                       dst (alu.op2_reg, byte-for-byte identical to MUL) and the high w bytes are
                       stashed in alu.op1_reg (dead src slot) for the dispatch to copy into hi.
                       Operands are cast to the signed 2w-bit type before the multiply so the high
                       half is not lost to C++ integer promotion (which does NOT widen 32x32 to 64,
                       open_question 6459). Flags: C = high half nonzero, N = full-product MSB,
                       Z = full product zero, V = product does not fit the low w-byte signed range. */
                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0();
                            u_byte src = alu.op1_reg.b0();
                            s_qword p = static_cast<s_qword>(static_cast<s_byte>(dst_before)) * static_cast<s_qword>(static_cast<s_byte>(src));
                            u_qword up = static_cast<u_qword>(p);
                            u_word lo = up & 0xFF;
                            u_word hi = (up >> 8) & 0xFF;
                            alu.op2_reg.w0 = lo;
                            alu.op1_reg.w0 = hi;
                            stage_pending_flags(alu_op, op_size, dst_before, src, lo);
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0();
                            u_qword src = alu.op1_reg.q0();
                            s_hword p = static_cast<s_hword>(static_cast<s_qword>(dst_before)) * static_cast<s_hword>(static_cast<s_qword>(src));
                            u_hword up = static_cast<u_hword>(p);
                            u_word lo = up & 0xFFFF;
                            u_word hi = (up >> 16) & 0xFFFF;
                            alu.op2_reg.w0 = lo;
                            alu.op1_reg.w0 = hi;
                            stage_pending_flags(alu_op, op_size, dst_before, src, lo);
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0();
                            u_hword src = alu.op1_reg.h0();
                            s_word p = static_cast<s_word>(static_cast<s_hword>(dst_before)) * static_cast<s_word>(static_cast<s_hword>(src));
                            u_word up = static_cast<u_word>(p);
                            u_word lo = up & 0xFFFFFFFF;
                            u_word hi = (up >> 32) & 0xFFFFFFFF;
                            alu.op2_reg.w0 = lo;
                            alu.op1_reg.w0 = hi;
                            stage_pending_flags(alu_op, op_size, dst_before, src, lo);
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word src = alu.op1_reg.w0;
                            __int128 p = static_cast<__int128>(static_cast<s_word>(dst_before)) * static_cast<__int128>(static_cast<s_word>(src));
                            unsigned __int128 up = static_cast<unsigned __int128>(p);
                            u_word lo = static_cast<u_word>(up);
                            u_word hi = static_cast<u_word>(up >> 64);
                            alu.op2_reg.w0 = lo;
                            alu.op1_reg.w0 = hi;
                            stage_pending_flags(alu_op, op_size, dst_before, src, lo);
                            break;
                        }
                    }

                    break;
                }

                case instr::umulw_opcode: {
                    /* Unsigned wide multiply (card maize-7). Same shape as MULW but zero-extended
                       operands and V == C == (high half nonzero). Operands are cast to the unsigned
                       2w-bit type before multiplying so w==4 keeps its high half (open_question 6459). */
                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0();
                            u_byte src = alu.op1_reg.b0();
                            u_qword up = static_cast<u_qword>(dst_before) * static_cast<u_qword>(src);
                            u_word lo = up & 0xFF;
                            u_word hi = (up >> 8) & 0xFF;
                            alu.op2_reg.w0 = lo;
                            alu.op1_reg.w0 = hi;
                            stage_pending_flags(alu_op, op_size, dst_before, src, lo);
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0();
                            u_qword src = alu.op1_reg.q0();
                            u_hword up = static_cast<u_hword>(dst_before) * static_cast<u_hword>(src);
                            u_word lo = up & 0xFFFF;
                            u_word hi = (up >> 16) & 0xFFFF;
                            alu.op2_reg.w0 = lo;
                            alu.op1_reg.w0 = hi;
                            stage_pending_flags(alu_op, op_size, dst_before, src, lo);
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0();
                            u_hword src = alu.op1_reg.h0();
                            u_word up = static_cast<u_word>(dst_before) * static_cast<u_word>(src);
                            u_word lo = up & 0xFFFFFFFF;
                            u_word hi = (up >> 32) & 0xFFFFFFFF;
                            alu.op2_reg.w0 = lo;
                            alu.op1_reg.w0 = hi;
                            stage_pending_flags(alu_op, op_size, dst_before, src, lo);
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word src = alu.op1_reg.w0;
                            unsigned __int128 up = static_cast<unsigned __int128>(dst_before) * static_cast<unsigned __int128>(src);
                            u_word lo = static_cast<u_word>(up);
                            u_word hi = static_cast<u_word>(up >> 64);
                            alu.op2_reg.w0 = lo;
                            alu.op1_reg.w0 = hi;
                            stage_pending_flags(alu_op, op_size, dst_before, src, lo);
                            break;
                        }
                    }

                    break;
                }

                case instr::div_opcode: {
                    /* Signed division (card maize-5). C and V are cleared; N and Z come from
                       the result. Divide-by-zero and the INT_MIN/-1 quotient overflow trap. */
                    switch (op_size) {
                        case 1: {
                            s_byte divisor = alu.op1_reg.b0();
                            s_byte dividend = alu.op2_reg.b0();
                            if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                            if (divisor == -1 && dividend == std::numeric_limits<s_byte>::min()) { raise_divide_error("signed division overflow"); }
                            s_byte result = dividend / divisor;
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_byte>(result));
                            alu.op2_reg.w0 = static_cast<u_byte>(result);
                            break;
                        }

                        case 2: {
                            s_qword divisor = alu.op1_reg.q0();
                            s_qword dividend = alu.op2_reg.q0();
                            if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                            if (divisor == -1 && dividend == std::numeric_limits<s_qword>::min()) { raise_divide_error("signed division overflow"); }
                            s_qword result = dividend / divisor;
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_qword>(result));
                            alu.op2_reg.w0 = static_cast<u_qword>(result);
                            break;
                        }

                        case 4: {
                            s_hword divisor = alu.op1_reg.h0();
                            s_hword dividend = alu.op2_reg.h0();
                            if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                            if (divisor == -1 && dividend == std::numeric_limits<s_hword>::min()) { raise_divide_error("signed division overflow"); }
                            s_hword result = dividend / divisor;
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_hword>(result));
                            alu.op2_reg.w0 = static_cast<u_hword>(result);
                            break;
                        }

                        case 8: {
                            s_word divisor = alu.op1_reg.w0;
                            s_word dividend = alu.op2_reg.w0;
                            if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                            if (divisor == -1 && dividend == std::numeric_limits<s_word>::min()) { raise_divide_error("signed division overflow"); }
                            s_word result = dividend / divisor;
                            stage_pending_flags(alu_op, op_size, 0, 0, result);
                            alu.op2_reg.w0 = static_cast<u_word>(result);
                            break;
                        }
                    }

                    break;
                }

                case instr::mod_opcode: {
                    /* Signed remainder (card maize-5). Same trap cases as signed DIV; the C++
                       remainder takes the sign of the dividend (truncated division). */
                    switch (op_size) {
                        case 1: {
                            s_byte divisor = alu.op1_reg.b0();
                            s_byte dividend = alu.op2_reg.b0();
                            if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                            if (divisor == -1 && dividend == std::numeric_limits<s_byte>::min()) { raise_divide_error("signed division overflow"); }
                            s_byte result = dividend % divisor;
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_byte>(result));
                            alu.op2_reg.w0 = static_cast<u_byte>(result);
                            break;
                        }

                        case 2: {
                            s_qword divisor = alu.op1_reg.q0();
                            s_qword dividend = alu.op2_reg.q0();
                            if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                            if (divisor == -1 && dividend == std::numeric_limits<s_qword>::min()) { raise_divide_error("signed division overflow"); }
                            s_qword result = dividend % divisor;
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_qword>(result));
                            alu.op2_reg.w0 = static_cast<u_qword>(result);
                            break;
                        }

                        case 4: {
                            s_hword divisor = alu.op1_reg.h0();
                            s_hword dividend = alu.op2_reg.h0();
                            if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                            if (divisor == -1 && dividend == std::numeric_limits<s_hword>::min()) { raise_divide_error("signed division overflow"); }
                            s_hword result = dividend % divisor;
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_hword>(result));
                            alu.op2_reg.w0 = static_cast<u_hword>(result);
                            break;
                        }

                        case 8: {
                            s_word divisor = alu.op1_reg.w0;
                            s_word dividend = alu.op2_reg.w0;
                            if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                            if (divisor == -1 && dividend == std::numeric_limits<s_word>::min()) { raise_divide_error("signed division overflow"); }
                            s_word result = dividend % divisor;
                            stage_pending_flags(alu_op, op_size, 0, 0, result);
                            alu.op2_reg.w0 = static_cast<u_word>(result);
                            break;
                        }
                    }

                    break;
                }

                case instr::udiv_opcode: {
                    /* Unsigned division (card maize-5). Divide-by-zero traps. */
                    switch (op_size) {
                        case 1: {
                            u_byte divisor = alu.op1_reg.b0();
                            if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                            u_byte result = alu.op2_reg.b0() / divisor;
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_byte>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword divisor = alu.op1_reg.q0();
                            if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                            u_qword result = alu.op2_reg.q0() / divisor;
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_qword>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword divisor = alu.op1_reg.h0();
                            if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                            u_hword result = alu.op2_reg.h0() / divisor;
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_hword>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word divisor = alu.op1_reg.w0;
                            if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                            u_word result = alu.op2_reg.w0 / divisor;
                            stage_pending_flags(alu_op, op_size, 0, 0, result);
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::umod_opcode: {
                    /* Unsigned remainder (card maize-5). Divide-by-zero traps. */
                    switch (op_size) {
                        case 1: {
                            u_byte divisor = alu.op1_reg.b0();
                            if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                            u_byte result = alu.op2_reg.b0() % divisor;
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_byte>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword divisor = alu.op1_reg.q0();
                            if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                            u_qword result = alu.op2_reg.q0() % divisor;
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_qword>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword divisor = alu.op1_reg.h0();
                            if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                            u_hword result = alu.op2_reg.h0() % divisor;
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_hword>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word divisor = alu.op1_reg.w0;
                            if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                            u_word result = alu.op2_reg.w0 % divisor;
                            stage_pending_flags(alu_op, op_size, 0, 0, result);
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::test_opcode:
                case instr::testind_opcode:
                case instr::and_opcode: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0() & alu.op1_reg.b0();
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_byte>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0() & alu.op1_reg.q0();
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_qword>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0() & alu.op1_reg.h0();
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_hword>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 & alu.op1_reg.w0;
                            stage_pending_flags(alu_op, op_size, 0, 0, result);
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::or_opcode: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0() | alu.op1_reg.b0();
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_byte>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0() | alu.op1_reg.q0();
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_qword>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0() | alu.op1_reg.h0();
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_hword>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 | alu.op1_reg.w0;
                            stage_pending_flags(alu_op, op_size, 0, 0, result);
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::nor_opcode: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = ~(alu.op2_reg.b0() | alu.op1_reg.b0());
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_byte>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = ~(alu.op2_reg.q0() | alu.op1_reg.q0());
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_qword>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = ~(alu.op2_reg.h0() | alu.op1_reg.h0());
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_hword>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = ~(alu.op2_reg.w0 | alu.op1_reg.w0);
                            stage_pending_flags(alu_op, op_size, 0, 0, result);
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::nand_opcode: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = ~(alu.op2_reg.b0() & alu.op1_reg.b0());
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_byte>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = ~(alu.op2_reg.q0() & alu.op1_reg.q0());
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_qword>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = ~(alu.op2_reg.h0() & alu.op1_reg.h0());
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_hword>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = ~(alu.op2_reg.w0 & alu.op1_reg.w0);
                            stage_pending_flags(alu_op, op_size, 0, 0, result);
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::xor_opcode: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0() ^ alu.op1_reg.b0();
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_byte>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0() ^ alu.op1_reg.q0();
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_qword>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0() ^ alu.op1_reg.h0();
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_hword>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 ^ alu.op1_reg.w0;
                            stage_pending_flags(alu_op, op_size, 0, 0, result);
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::shl_opcode: {
                    /* Shift-count edge cases (card maize-1): n==0 leaves all flags unaffected;
                       1<=n<=bits shifts normally with C = last bit shifted out the top and V defined
                       only for n==1 (sign bit changed); n>bits yields result=0 with all flags cleared.
                       Never pass an out-of-range count to C++'s << (that is undefined behavior).
                       Flags deferred (maize-197): n==0 stays flag-neutral (not staged); the other
                       branches stage dst_before + the count for materialize_flags to resolve. */
                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0();
                            u_word n = alu.op1_reg.b0();
                            const u_word bits = 8;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n <= bits) {
                                u_byte result = (n == bits) ? u_byte(0) : u_byte(dst_before << n);
                                alu.op2_reg.w0 = result;
                                stage_pending_flags(alu_op, op_size, dst_before, n, result);
                            }
                            else {
                                alu.op2_reg.w0 = 0;
                                stage_pending_flags(alu_op, op_size, dst_before, n, 0);
                            }
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0();
                            u_word n = alu.op1_reg.q0();
                            const u_word bits = 16;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n <= bits) {
                                u_qword result = (n == bits) ? u_qword(0) : u_qword(dst_before << n);
                                alu.op2_reg.w0 = result;
                                stage_pending_flags(alu_op, op_size, dst_before, n, result);
                            }
                            else {
                                alu.op2_reg.w0 = 0;
                                stage_pending_flags(alu_op, op_size, dst_before, n, 0);
                            }
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0();
                            u_word n = alu.op1_reg.h0();
                            const u_word bits = 32;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n <= bits) {
                                u_hword result = (n == bits) ? u_hword(0) : u_hword(dst_before << n);
                                alu.op2_reg.w0 = result;
                                stage_pending_flags(alu_op, op_size, dst_before, n, result);
                            }
                            else {
                                alu.op2_reg.w0 = 0;
                                stage_pending_flags(alu_op, op_size, dst_before, n, 0);
                            }
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word n = alu.op1_reg.w0;
                            const u_word bits = 64;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n <= bits) {
                                u_word result = (n == bits) ? u_word(0) : u_word(dst_before << n);
                                alu.op2_reg.w0 = result;
                                stage_pending_flags(alu_op, op_size, dst_before, n, result);
                            }
                            else {
                                alu.op2_reg.w0 = 0;
                                stage_pending_flags(alu_op, op_size, dst_before, n, 0);
                            }
                            break;
                        }
                    }

                    break;
                }

                case instr::shr_opcode: {
                    /* Shift-count edge cases (card maize-1): n==0 leaves all flags unaffected;
                       1<=n<=bits shifts normally with C = last bit shifted out the bottom and V defined
                       only for n==1 (logical shift always clears the sign bit, so V reflects whether it
                       was set beforehand); n>bits yields result=0 with all flags cleared. Never pass an
                       out-of-range count to C++'s >> (that is undefined behavior). */
                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0();
                            u_word n = alu.op1_reg.b0();
                            const u_word bits = 8;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n <= bits) {
                                u_byte result = (n == bits) ? u_byte(0) : u_byte(dst_before >> n);
                                alu.op2_reg.w0 = result;
                                stage_pending_flags(alu_op, op_size, dst_before, n, result);
                            }
                            else {
                                alu.op2_reg.w0 = 0;
                                stage_pending_flags(alu_op, op_size, dst_before, n, 0);
                            }
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0();
                            u_word n = alu.op1_reg.q0();
                            const u_word bits = 16;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n <= bits) {
                                u_qword result = (n == bits) ? u_qword(0) : u_qword(dst_before >> n);
                                alu.op2_reg.w0 = result;
                                stage_pending_flags(alu_op, op_size, dst_before, n, result);
                            }
                            else {
                                alu.op2_reg.w0 = 0;
                                stage_pending_flags(alu_op, op_size, dst_before, n, 0);
                            }
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0();
                            u_word n = alu.op1_reg.h0();
                            const u_word bits = 32;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n <= bits) {
                                u_hword result = (n == bits) ? u_hword(0) : u_hword(dst_before >> n);
                                alu.op2_reg.w0 = result;
                                stage_pending_flags(alu_op, op_size, dst_before, n, result);
                            }
                            else {
                                alu.op2_reg.w0 = 0;
                                stage_pending_flags(alu_op, op_size, dst_before, n, 0);
                            }
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word n = alu.op1_reg.w0;
                            const u_word bits = 64;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n <= bits) {
                                u_word result = (n == bits) ? u_word(0) : u_word(dst_before >> n);
                                alu.op2_reg.w0 = result;
                                stage_pending_flags(alu_op, op_size, dst_before, n, result);
                            }
                            else {
                                alu.op2_reg.w0 = 0;
                                stage_pending_flags(alu_op, op_size, dst_before, n, 0);
                            }
                            break;
                        }
                    }

                    break;
                }

                case instr::sar_opcode: {
                    /* Arithmetic (sign-preserving) right shift (card maize-54). Mirrors SHR but
                       fills vacated high bits with the operand's sign bit. Flags: C = last bit
                       shifted out (same formula as SHR); N/Z from the signed result; V = 0 always
                       (an arithmetic shift replicates the sign bit and can never flip it, so
                       signed overflow is impossible). n==0 leaves all flags unaffected (maize-31).
                       n>=bits saturates to the sign fill: -1 (all ones) for a negative operand,
                       0 for a non-negative one, with C = the operand's sign bit. Never pass an
                       out-of-range count to C++'s >> (that is undefined behavior); the signed
                       shift only runs for 1<=n<bits. */
                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0();
                            u_word n = alu.op1_reg.b0();
                            const u_word bits = 8;
                            bool sign = (dst_before >> (bits - 1)) & 1;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n < bits) {
                                u_byte result = u_byte(s_byte(dst_before) >> n);
                                alu.op2_reg.w0 = result;
                                stage_pending_flags(alu_op, op_size, dst_before, n, result);
                            }
                            else {
                                u_byte result = sign ? u_byte(0xFF) : u_byte(0);
                                alu.op2_reg.w0 = result;
                                stage_pending_flags(alu_op, op_size, dst_before, n, result);
                            }
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0();
                            u_word n = alu.op1_reg.q0();
                            const u_word bits = 16;
                            bool sign = (dst_before >> (bits - 1)) & 1;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n < bits) {
                                u_qword result = u_qword(s_qword(dst_before) >> n);
                                alu.op2_reg.w0 = result;
                                stage_pending_flags(alu_op, op_size, dst_before, n, result);
                            }
                            else {
                                u_qword result = sign ? u_qword(0xFFFF) : u_qword(0);
                                alu.op2_reg.w0 = result;
                                stage_pending_flags(alu_op, op_size, dst_before, n, result);
                            }
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0();
                            u_word n = alu.op1_reg.h0();
                            const u_word bits = 32;
                            bool sign = (dst_before >> (bits - 1)) & 1;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n < bits) {
                                u_hword result = u_hword(s_hword(dst_before) >> n);
                                alu.op2_reg.w0 = result;
                                stage_pending_flags(alu_op, op_size, dst_before, n, result);
                            }
                            else {
                                u_hword result = sign ? u_hword(0xFFFFFFFF) : u_hword(0);
                                alu.op2_reg.w0 = result;
                                stage_pending_flags(alu_op, op_size, dst_before, n, result);
                            }
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word n = alu.op1_reg.w0;
                            const u_word bits = 64;
                            bool sign = (dst_before >> (bits - 1)) & 1;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n < bits) {
                                u_word result = u_word(s_word(dst_before) >> n);
                                alu.op2_reg.w0 = result;
                                stage_pending_flags(alu_op, op_size, dst_before, n, result);
                            }
                            else {
                                u_word result = sign ? u_word(0xFFFFFFFFFFFFFFFF) : u_word(0);
                                alu.op2_reg.w0 = result;
                                stage_pending_flags(alu_op, op_size, dst_before, n, result);
                            }
                            break;
                        }
                    }

                    break;
                }

                case alu_uop_inc: {
                    /* INC is ADD with src = 1; C and V follow the ADD family (card maize-1).
                       Flags deferred (maize-197): staged with src = 1 so materialize_flags
                       shares the ADD formula. */
                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0();
                            u_byte result = dst_before + u_byte(1);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, 1, result);
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0();
                            u_qword result = dst_before + u_qword(1);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, 1, result);
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0();
                            u_hword result = dst_before + u_hword(1);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, 1, result);
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word result = dst_before + u_word(1);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, 1, result);
                            break;
                        }
                    }

                    break;
                }

                case alu_uop_dec: {
                    /* DEC is SUB with src = 1; C and V follow the SUB family (card maize-1).
                       Flags deferred (maize-197): staged with src = 1 so materialize_flags
                       shares the SUB formula. */
                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0();
                            u_byte result = dst_before - u_byte(1);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, 1, result);
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0();
                            u_qword result = dst_before - u_qword(1);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, 1, result);
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0();
                            u_hword result = dst_before - u_hword(1);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, 1, result);
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word result = dst_before - u_word(1);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, dst_before, 1, result);
                            break;
                        }
                    }

                    break;
                }

                case alu_uop_not: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = ~alu.op2_reg.b0();
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_byte>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = ~alu.op2_reg.q0();
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_qword>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = ~alu.op2_reg.h0();
                            stage_pending_flags(alu_op, op_size, 0, 0, static_cast<u_hword>(result));
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = ~alu.op2_reg.w0;
                            stage_pending_flags(alu_op, op_size, 0, 0, result);
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu_uop_neg: {
                    /* NEG is SUB with dst = 0, src = operand: result = 0 - x (card
                       maize-64). C/N/V/Z follow the SUB family with a zero minuend, so
                       C (borrow) is set whenever x != 0, and V is set only when x is the
                       width's INT_MIN (the one value -x cannot represent). */
                    switch (op_size) {
                        case 1: {
                            u_byte x = alu.op2_reg.b0();
                            u_byte result = static_cast<u_byte>(u_byte(0) - x);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, x, 0, result);
                            break;
                        }

                        case 2: {
                            u_qword x = alu.op2_reg.q0();
                            u_qword result = static_cast<u_qword>(u_qword(0) - x);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, x, 0, result);
                            break;
                        }

                        case 4: {
                            u_hword x = alu.op2_reg.h0();
                            u_hword result = static_cast<u_hword>(u_hword(0) - x);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, x, 0, result);
                            break;
                        }

                        case 8: {
                            u_word x = alu.op2_reg.w0;
                            u_word result = static_cast<u_word>(u_word(0) - x);
                            alu.op2_reg.w0 = result;
                            stage_pending_flags(alu_op, op_size, x, 0, result);
                            break;
                        }
                    }

                    break;
                }
            }

            /* CMP/CMPIND/TEST/TSTIND set flags only; they must not modify the destination
               operand (card maize-40). The width branches above wrote the compare/AND result
               into the op2 scratch, which the dispatch then writes back to the real register.
               Restore the scratch to its entry value so that writeback is a no-op. */
            if (alu_op == instr::cmp_opcode || alu_op == instr::cmpind_opcode
                || alu_op == instr::test_opcode || alu_op == instr::testind_opcode) {
                alu.op2_reg.w0 = alu_op2_entry;
            }
        }

        /* ---- card maize-198: in-place, per-op ALU computation functions ---------------
           Each function below is the width-specialized body lifted VERBATIM from the
           corresponding run_alu case, with two mechanical substitutions: the shared
           alu.op1_reg / alu.op2_reg staging-bank reads become the passed-in src_in /
           dst_in locals (truncated per width exactly as the run_alu case read
           alu.op1_reg.b0()/q0/h0/w0 and alu.op2_reg.b0()/q0/h0/w0), and the
           `alu.op2_reg.w0 = result` store becomes the returned value. Every function
           still calls stage_pending_flags(op_kind, op_size, dst_before, src, result)
           with the identical arguments run_alu staged, so materialize_flags()
           (maize-197) yields bit-identical flags; flags are NOT computed eagerly.

           op_kind is passed rather than hardcoded so CMP can reuse alu_sub and TEST can
           reuse alu_and while each stages under its own opcode (Decision D5), mirroring
           run_alu's shared case blocks (cmp/cmpind share sub's; test/testind share
           and's). src_in arrives already sign-extended from its own encoded subreg
           width to a full word (Decision D3). The caller writes the returned value back
           via write_subreg_bits EXCEPT for CMP/TEST, which discard it (Decision D2, the
           in-place replacement for the alu_op2_entry snapshot/restore). run_alu() and
           the alu.op1_reg/op2_reg staging bank stay in place for the out-of-scope call
           sites (unary INC/DEC/NOT/NEG, CMPIND/TESTIND, LEA, MULW/UMULW, FP). */

        u_word alu_add(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            /* Carry (C) is the unsigned carry-out; overflow (V) is the signed-overflow
               test (same-sign operands, result sign differs). See card maize-1.
               Flags deferred: staged here, resolved by materialize_flags (maize-197). */
            u_word ret = 0;
            switch (op_size) {
                case 1: { u_byte dst_before = static_cast<u_byte>(dst_in); u_byte src = static_cast<u_byte>(src_in);
                    u_byte result = dst_before + src; ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
                case 2: { u_qword dst_before = static_cast<u_qword>(dst_in); u_qword src = static_cast<u_qword>(src_in);
                    u_qword result = dst_before + src; ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
                case 4: { u_hword dst_before = static_cast<u_hword>(dst_in); u_hword src = static_cast<u_hword>(src_in);
                    u_hword result = dst_before + src; ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
                case 8: { u_word dst_before = dst_in; u_word src = src_in;
                    u_word result = dst_before + src; ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
            }
            return ret;
        }

        u_word alu_sub(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            /* Carry (C) is the unsigned borrow (x86 convention: C=1 means dst_before <u src);
               overflow (V) is the signed-overflow test. See card maize-1. Also serves CMP
               (op_kind = cmp_opcode), whose caller discards the result. */
            u_word ret = 0;
            switch (op_size) {
                case 1: { u_byte dst_before = static_cast<u_byte>(dst_in); u_byte src = static_cast<u_byte>(src_in);
                    u_byte result = dst_before - src; ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
                case 2: { u_qword dst_before = static_cast<u_qword>(dst_in); u_qword src = static_cast<u_qword>(src_in);
                    u_qword result = dst_before - src; ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
                case 4: { u_hword dst_before = static_cast<u_hword>(dst_in); u_hword src = static_cast<u_hword>(src_in);
                    u_hword result = dst_before - src; ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
                case 8: { u_word dst_before = dst_in; u_word src = src_in;
                    u_word result = dst_before - src; ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
            }
            return ret;
        }

        u_word alu_adc(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            /* Add with carry: dst + src + C (card maize-6). card maize-197: the carry-in
               read is a flag CONSUMER; resolve any deferred flags before reading
               carryout_flag, then stage ADC's own descriptor (result bakes in the carry). */
            materialize_flags();
            unsigned carry_in = (bool)carryout_flag ? 1u : 0u;
            u_word ret = 0;
            switch (op_size) {
                case 1: { u_byte dst_before = static_cast<u_byte>(dst_in); u_byte src = static_cast<u_byte>(src_in);
                    u_byte sum1 = dst_before + src; u_byte result = sum1 + static_cast<u_byte>(carry_in); ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
                case 2: { u_qword dst_before = static_cast<u_qword>(dst_in); u_qword src = static_cast<u_qword>(src_in);
                    u_qword sum1 = dst_before + src; u_qword result = sum1 + static_cast<u_qword>(carry_in); ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
                case 4: { u_hword dst_before = static_cast<u_hword>(dst_in); u_hword src = static_cast<u_hword>(src_in);
                    u_hword sum1 = dst_before + src; u_hword result = sum1 + static_cast<u_hword>(carry_in); ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
                case 8: { u_word dst_before = dst_in; u_word src = src_in;
                    u_word sum1 = dst_before + src; u_word result = sum1 + static_cast<u_word>(carry_in); ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
            }
            return ret;
        }

        u_word alu_sbb(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            /* Subtract with borrow: dst - src - C (card maize-6). Same carry-in-consumer
               ordering as ADC (materialize before reading carryout_flag). */
            materialize_flags();
            unsigned borrow_in = (bool)carryout_flag ? 1u : 0u;
            u_word ret = 0;
            switch (op_size) {
                case 1: { u_byte dst_before = static_cast<u_byte>(dst_in); u_byte src = static_cast<u_byte>(src_in);
                    u_byte diff1 = dst_before - src; u_byte result = diff1 - static_cast<u_byte>(borrow_in); ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
                case 2: { u_qword dst_before = static_cast<u_qword>(dst_in); u_qword src = static_cast<u_qword>(src_in);
                    u_qword diff1 = dst_before - src; u_qword result = diff1 - static_cast<u_qword>(borrow_in); ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
                case 4: { u_hword dst_before = static_cast<u_hword>(dst_in); u_hword src = static_cast<u_hword>(src_in);
                    u_hword diff1 = dst_before - src; u_hword result = diff1 - static_cast<u_hword>(borrow_in); ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
                case 8: { u_word dst_before = dst_in; u_word src = src_in;
                    u_word diff1 = dst_before - src; u_word result = diff1 - static_cast<u_word>(borrow_in); ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
            }
            return ret;
        }

        u_word alu_mul(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            /* Overflow (V) is the signed-overflow test on the pre-op operands; C mirrors V.
               materialize_flags recomputes ovf from the stored operands (maize-197). */
            u_word ret = 0;
            switch (op_size) {
                case 1: { u_byte dst_before = static_cast<u_byte>(dst_in); u_byte src = static_cast<u_byte>(src_in);
                    u_byte result = dst_before * src; ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
                case 2: { u_qword dst_before = static_cast<u_qword>(dst_in); u_qword src = static_cast<u_qword>(src_in);
                    u_qword result = dst_before * src; ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
                case 4: { u_hword dst_before = static_cast<u_hword>(dst_in); u_hword src = static_cast<u_hword>(src_in);
                    u_hword result = dst_before * src; ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
                case 8: { u_word dst_before = dst_in; u_word src = src_in;
                    u_word result = dst_before * src; ret = result;
                    stage_pending_flags(op_kind, op_size, dst_before, src, result); break; }
            }
            return ret;
        }

        u_word alu_div(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            /* Signed division (card maize-5). C/V cleared; N/Z from the result. Divide-by-
               zero and INT_MIN/-1 overflow trap BEFORE any destination write. */
            u_word ret = 0;
            switch (op_size) {
                case 1: { s_byte divisor = static_cast<u_byte>(src_in); s_byte dividend = static_cast<u_byte>(dst_in);
                    if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                    if (divisor == -1 && dividend == std::numeric_limits<s_byte>::min()) { raise_divide_error("signed division overflow"); }
                    s_byte result = dividend / divisor;
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_byte>(result)); ret = static_cast<u_byte>(result); break; }
                case 2: { s_qword divisor = static_cast<u_qword>(src_in); s_qword dividend = static_cast<u_qword>(dst_in);
                    if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                    if (divisor == -1 && dividend == std::numeric_limits<s_qword>::min()) { raise_divide_error("signed division overflow"); }
                    s_qword result = dividend / divisor;
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_qword>(result)); ret = static_cast<u_qword>(result); break; }
                case 4: { s_hword divisor = static_cast<u_hword>(src_in); s_hword dividend = static_cast<u_hword>(dst_in);
                    if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                    if (divisor == -1 && dividend == std::numeric_limits<s_hword>::min()) { raise_divide_error("signed division overflow"); }
                    s_hword result = dividend / divisor;
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_hword>(result)); ret = static_cast<u_hword>(result); break; }
                case 8: { s_word divisor = src_in; s_word dividend = dst_in;
                    if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                    if (divisor == -1 && dividend == std::numeric_limits<s_word>::min()) { raise_divide_error("signed division overflow"); }
                    s_word result = dividend / divisor;
                    stage_pending_flags(op_kind, op_size, 0, 0, result); ret = static_cast<u_word>(result); break; }
            }
            return ret;
        }

        u_word alu_mod(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            /* Signed remainder (card maize-5). Same trap cases as signed DIV. */
            u_word ret = 0;
            switch (op_size) {
                case 1: { s_byte divisor = static_cast<u_byte>(src_in); s_byte dividend = static_cast<u_byte>(dst_in);
                    if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                    if (divisor == -1 && dividend == std::numeric_limits<s_byte>::min()) { raise_divide_error("signed division overflow"); }
                    s_byte result = dividend % divisor;
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_byte>(result)); ret = static_cast<u_byte>(result); break; }
                case 2: { s_qword divisor = static_cast<u_qword>(src_in); s_qword dividend = static_cast<u_qword>(dst_in);
                    if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                    if (divisor == -1 && dividend == std::numeric_limits<s_qword>::min()) { raise_divide_error("signed division overflow"); }
                    s_qword result = dividend % divisor;
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_qword>(result)); ret = static_cast<u_qword>(result); break; }
                case 4: { s_hword divisor = static_cast<u_hword>(src_in); s_hword dividend = static_cast<u_hword>(dst_in);
                    if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                    if (divisor == -1 && dividend == std::numeric_limits<s_hword>::min()) { raise_divide_error("signed division overflow"); }
                    s_hword result = dividend % divisor;
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_hword>(result)); ret = static_cast<u_hword>(result); break; }
                case 8: { s_word divisor = src_in; s_word dividend = dst_in;
                    if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                    if (divisor == -1 && dividend == std::numeric_limits<s_word>::min()) { raise_divide_error("signed division overflow"); }
                    s_word result = dividend % divisor;
                    stage_pending_flags(op_kind, op_size, 0, 0, result); ret = static_cast<u_word>(result); break; }
            }
            return ret;
        }

        u_word alu_udiv(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            /* Unsigned division (card maize-5). Divide-by-zero traps. */
            u_word ret = 0;
            switch (op_size) {
                case 1: { u_byte divisor = static_cast<u_byte>(src_in);
                    if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                    u_byte result = static_cast<u_byte>(dst_in) / divisor;
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_byte>(result)); ret = result; break; }
                case 2: { u_qword divisor = static_cast<u_qword>(src_in);
                    if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                    u_qword result = static_cast<u_qword>(dst_in) / divisor;
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_qword>(result)); ret = result; break; }
                case 4: { u_hword divisor = static_cast<u_hword>(src_in);
                    if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                    u_hword result = static_cast<u_hword>(dst_in) / divisor;
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_hword>(result)); ret = result; break; }
                case 8: { u_word divisor = src_in;
                    if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                    u_word result = dst_in / divisor;
                    stage_pending_flags(op_kind, op_size, 0, 0, result); ret = result; break; }
            }
            return ret;
        }

        u_word alu_umod(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            /* Unsigned remainder (card maize-5). Divide-by-zero traps. */
            u_word ret = 0;
            switch (op_size) {
                case 1: { u_byte divisor = static_cast<u_byte>(src_in);
                    if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                    u_byte result = static_cast<u_byte>(dst_in) % divisor;
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_byte>(result)); ret = result; break; }
                case 2: { u_qword divisor = static_cast<u_qword>(src_in);
                    if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                    u_qword result = static_cast<u_qword>(dst_in) % divisor;
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_qword>(result)); ret = result; break; }
                case 4: { u_hword divisor = static_cast<u_hword>(src_in);
                    if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                    u_hword result = static_cast<u_hword>(dst_in) % divisor;
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_hword>(result)); ret = result; break; }
                case 8: { u_word divisor = src_in;
                    if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                    u_word result = dst_in % divisor;
                    stage_pending_flags(op_kind, op_size, 0, 0, result); ret = result; break; }
            }
            return ret;
        }

        u_word alu_and(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            /* Logic: Z/N from result, C/V cleared. Also serves TEST (op_kind = test_opcode),
               whose caller discards the result. */
            u_word ret = 0;
            switch (op_size) {
                case 1: { u_byte result = static_cast<u_byte>(dst_in) & static_cast<u_byte>(src_in);
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_byte>(result)); ret = result; break; }
                case 2: { u_qword result = static_cast<u_qword>(dst_in) & static_cast<u_qword>(src_in);
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_qword>(result)); ret = result; break; }
                case 4: { u_hword result = static_cast<u_hword>(dst_in) & static_cast<u_hword>(src_in);
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_hword>(result)); ret = result; break; }
                case 8: { u_word result = dst_in & src_in;
                    stage_pending_flags(op_kind, op_size, 0, 0, result); ret = result; break; }
            }
            return ret;
        }

        u_word alu_or(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            u_word ret = 0;
            switch (op_size) {
                case 1: { u_byte result = static_cast<u_byte>(dst_in) | static_cast<u_byte>(src_in);
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_byte>(result)); ret = result; break; }
                case 2: { u_qword result = static_cast<u_qword>(dst_in) | static_cast<u_qword>(src_in);
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_qword>(result)); ret = result; break; }
                case 4: { u_hword result = static_cast<u_hword>(dst_in) | static_cast<u_hword>(src_in);
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_hword>(result)); ret = result; break; }
                case 8: { u_word result = dst_in | src_in;
                    stage_pending_flags(op_kind, op_size, 0, 0, result); ret = result; break; }
            }
            return ret;
        }

        u_word alu_nor(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            u_word ret = 0;
            switch (op_size) {
                case 1: { u_byte result = ~(static_cast<u_byte>(dst_in) | static_cast<u_byte>(src_in));
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_byte>(result)); ret = result; break; }
                case 2: { u_qword result = ~(static_cast<u_qword>(dst_in) | static_cast<u_qword>(src_in));
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_qword>(result)); ret = result; break; }
                case 4: { u_hword result = ~(static_cast<u_hword>(dst_in) | static_cast<u_hword>(src_in));
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_hword>(result)); ret = result; break; }
                case 8: { u_word result = ~(dst_in | src_in);
                    stage_pending_flags(op_kind, op_size, 0, 0, result); ret = result; break; }
            }
            return ret;
        }

        u_word alu_nand(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            u_word ret = 0;
            switch (op_size) {
                case 1: { u_byte result = ~(static_cast<u_byte>(dst_in) & static_cast<u_byte>(src_in));
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_byte>(result)); ret = result; break; }
                case 2: { u_qword result = ~(static_cast<u_qword>(dst_in) & static_cast<u_qword>(src_in));
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_qword>(result)); ret = result; break; }
                case 4: { u_hword result = ~(static_cast<u_hword>(dst_in) & static_cast<u_hword>(src_in));
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_hword>(result)); ret = result; break; }
                case 8: { u_word result = ~(dst_in & src_in);
                    stage_pending_flags(op_kind, op_size, 0, 0, result); ret = result; break; }
            }
            return ret;
        }

        u_word alu_xor(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            u_word ret = 0;
            switch (op_size) {
                case 1: { u_byte result = static_cast<u_byte>(dst_in) ^ static_cast<u_byte>(src_in);
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_byte>(result)); ret = result; break; }
                case 2: { u_qword result = static_cast<u_qword>(dst_in) ^ static_cast<u_qword>(src_in);
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_qword>(result)); ret = result; break; }
                case 4: { u_hword result = static_cast<u_hword>(dst_in) ^ static_cast<u_hword>(src_in);
                    stage_pending_flags(op_kind, op_size, 0, 0, static_cast<u_hword>(result)); ret = result; break; }
                case 8: { u_word result = dst_in ^ src_in;
                    stage_pending_flags(op_kind, op_size, 0, 0, result); ret = result; break; }
            }
            return ret;
        }

        /* Shift count edge cases (cards maize-1 / maize-31): n==0 leaves the destination
           value unchanged (a same-value write, per open_question 8826) and stages NO flags,
           so a still-pending descriptor from a prior instruction is neither clobbered nor
           superseded; 1<=n<=bits shifts normally; n>bits (SHL/SHR) yields 0. Never pass an
           out-of-range count to C++'s shift operators. Returns dst_before for n==0. */
        u_word alu_shl(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            u_word ret = 0;
            switch (op_size) {
                case 1: { u_byte dst_before = static_cast<u_byte>(dst_in); u_word n = static_cast<u_byte>(src_in); const u_word bits = 8;
                    if (n == 0) { ret = dst_before; }
                    else if (n <= bits) { u_byte result = (n == bits) ? u_byte(0) : u_byte(dst_before << n); ret = result;
                        stage_pending_flags(op_kind, op_size, dst_before, n, result); }
                    else { ret = 0; stage_pending_flags(op_kind, op_size, dst_before, n, 0); } break; }
                case 2: { u_qword dst_before = static_cast<u_qword>(dst_in); u_word n = static_cast<u_qword>(src_in); const u_word bits = 16;
                    if (n == 0) { ret = dst_before; }
                    else if (n <= bits) { u_qword result = (n == bits) ? u_qword(0) : u_qword(dst_before << n); ret = result;
                        stage_pending_flags(op_kind, op_size, dst_before, n, result); }
                    else { ret = 0; stage_pending_flags(op_kind, op_size, dst_before, n, 0); } break; }
                case 4: { u_hword dst_before = static_cast<u_hword>(dst_in); u_word n = static_cast<u_hword>(src_in); const u_word bits = 32;
                    if (n == 0) { ret = dst_before; }
                    else if (n <= bits) { u_hword result = (n == bits) ? u_hword(0) : u_hword(dst_before << n); ret = result;
                        stage_pending_flags(op_kind, op_size, dst_before, n, result); }
                    else { ret = 0; stage_pending_flags(op_kind, op_size, dst_before, n, 0); } break; }
                case 8: { u_word dst_before = dst_in; u_word n = src_in; const u_word bits = 64;
                    if (n == 0) { ret = dst_before; }
                    else if (n <= bits) { u_word result = (n == bits) ? u_word(0) : u_word(dst_before << n); ret = result;
                        stage_pending_flags(op_kind, op_size, dst_before, n, result); }
                    else { ret = 0; stage_pending_flags(op_kind, op_size, dst_before, n, 0); } break; }
            }
            return ret;
        }

        u_word alu_shr(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            u_word ret = 0;
            switch (op_size) {
                case 1: { u_byte dst_before = static_cast<u_byte>(dst_in); u_word n = static_cast<u_byte>(src_in); const u_word bits = 8;
                    if (n == 0) { ret = dst_before; }
                    else if (n <= bits) { u_byte result = (n == bits) ? u_byte(0) : u_byte(dst_before >> n); ret = result;
                        stage_pending_flags(op_kind, op_size, dst_before, n, result); }
                    else { ret = 0; stage_pending_flags(op_kind, op_size, dst_before, n, 0); } break; }
                case 2: { u_qword dst_before = static_cast<u_qword>(dst_in); u_word n = static_cast<u_qword>(src_in); const u_word bits = 16;
                    if (n == 0) { ret = dst_before; }
                    else if (n <= bits) { u_qword result = (n == bits) ? u_qword(0) : u_qword(dst_before >> n); ret = result;
                        stage_pending_flags(op_kind, op_size, dst_before, n, result); }
                    else { ret = 0; stage_pending_flags(op_kind, op_size, dst_before, n, 0); } break; }
                case 4: { u_hword dst_before = static_cast<u_hword>(dst_in); u_word n = static_cast<u_hword>(src_in); const u_word bits = 32;
                    if (n == 0) { ret = dst_before; }
                    else if (n <= bits) { u_hword result = (n == bits) ? u_hword(0) : u_hword(dst_before >> n); ret = result;
                        stage_pending_flags(op_kind, op_size, dst_before, n, result); }
                    else { ret = 0; stage_pending_flags(op_kind, op_size, dst_before, n, 0); } break; }
                case 8: { u_word dst_before = dst_in; u_word n = src_in; const u_word bits = 64;
                    if (n == 0) { ret = dst_before; }
                    else if (n <= bits) { u_word result = (n == bits) ? u_word(0) : u_word(dst_before >> n); ret = result;
                        stage_pending_flags(op_kind, op_size, dst_before, n, result); }
                    else { ret = 0; stage_pending_flags(op_kind, op_size, dst_before, n, 0); } break; }
            }
            return ret;
        }

        u_word alu_sar(u_byte op_kind, u_byte op_size, u_word src_in, u_word dst_in) {
            /* Arithmetic (sign-preserving) right shift (card maize-54). n==0 leaves the
               value unchanged and stages no flags; n>=bits saturates to the sign fill. */
            u_word ret = 0;
            switch (op_size) {
                case 1: { u_byte dst_before = static_cast<u_byte>(dst_in); u_word n = static_cast<u_byte>(src_in); const u_word bits = 8;
                    bool sign = (dst_before >> (bits - 1)) & 1;
                    if (n == 0) { ret = dst_before; }
                    else if (n < bits) { u_byte result = u_byte(s_byte(dst_before) >> n); ret = result;
                        stage_pending_flags(op_kind, op_size, dst_before, n, result); }
                    else { u_byte result = sign ? u_byte(0xFF) : u_byte(0); ret = result;
                        stage_pending_flags(op_kind, op_size, dst_before, n, result); } break; }
                case 2: { u_qword dst_before = static_cast<u_qword>(dst_in); u_word n = static_cast<u_qword>(src_in); const u_word bits = 16;
                    bool sign = (dst_before >> (bits - 1)) & 1;
                    if (n == 0) { ret = dst_before; }
                    else if (n < bits) { u_qword result = u_qword(s_qword(dst_before) >> n); ret = result;
                        stage_pending_flags(op_kind, op_size, dst_before, n, result); }
                    else { u_qword result = sign ? u_qword(0xFFFF) : u_qword(0); ret = result;
                        stage_pending_flags(op_kind, op_size, dst_before, n, result); } break; }
                case 4: { u_hword dst_before = static_cast<u_hword>(dst_in); u_word n = static_cast<u_hword>(src_in); const u_word bits = 32;
                    bool sign = (dst_before >> (bits - 1)) & 1;
                    if (n == 0) { ret = dst_before; }
                    else if (n < bits) { u_hword result = u_hword(s_hword(dst_before) >> n); ret = result;
                        stage_pending_flags(op_kind, op_size, dst_before, n, result); }
                    else { u_hword result = sign ? u_hword(0xFFFFFFFF) : u_hword(0); ret = result;
                        stage_pending_flags(op_kind, op_size, dst_before, n, result); } break; }
                case 8: { u_word dst_before = dst_in; u_word n = src_in; const u_word bits = 64;
                    bool sign = (dst_before >> (bits - 1)) & 1;
                    if (n == 0) { ret = dst_before; }
                    else if (n < bits) { u_word result = u_word(s_word(dst_before) >> n); ret = result;
                        stage_pending_flags(op_kind, op_size, dst_before, n, result); }
                    else { u_word result = sign ? u_word(0xFFFFFFFFFFFFFFFF) : u_word(0); ret = result;
                        stage_pending_flags(op_kind, op_size, dst_before, n, result); } break; }
            }
            return ret;
        }

        /* Floating-point arithmetic runner (card maize-122), the FP analogue of
           run_alu for the four two-operand arithmetic ops. The caller loads the
           src operand into alu.op1_reg and the dst operand into alu.op2_reg (both
           as raw bit patterns), sets alu.b0() to the opcode byte and alu.b2() to the
           operation width (4 = binary32, 8 = binary64), and reads the result back
           from alu.op2_reg. Result = dst OP src (op2 OP op1), matching the integer
           ALU operand convention. FFLAGS are OR-ed into FCSR (sticky); the integer
           RF flags C/N/V/Z are left untouched (a dedicated FCSR, decision recorded). */
        void run_fpu_arith() {
            u_byte base = alu.b0() & arithmetic_logic_unit::opflag_code;
            u_byte width = alu.b2();
            u_byte frm = fp_checked_frm();
            u_word dst = alu.op2_reg.w0;
            u_word src = alu.op1_reg.w0;
            fpu::fresult res;

            switch (base) {
                case instr::fadd_opcode: res = fpu::fp_add(dst, src, width, frm); break;
                case instr::fsub_opcode: res = fpu::fp_sub(dst, src, width, frm); break;
                case instr::fmul_opcode: res = fpu::fp_mul(dst, src, width, frm); break;
                case instr::fdiv_opcode: res = fpu::fp_div(dst, src, width, frm); break;
                default: raise_illegal_fp("fp arithmetic dispatch");
            }

            if (res.flags) {
                fcsr_raise(res.flags);
            }
            alu.op2_reg.w0 = res.bits;
        }

        /* Out-of-line unknown-opcode trap, kept out of tick() so no non-trivial local lives
           in the threaded-dispatch body (see LBL_default). */
        [[noreturn]] void raise_unknown_opcode(u_byte op) {
            std::stringstream err {};
            err << "unknown opcode: " << std::hex << static_cast<unsigned>(op);
            throw std::logic_error(err.str());
        }

        /* This is the state machine that implements the machine-code instructions. */
        void tick() {
            running_flag = true;

            /* active_input_ptr is set only for the headless stdin-injection path (the windowed
               keyboard is driven off-thread by push_event/port_read and leaves it null), so this
               drain runs per instruction only when a deterministic stdin source is attached. */
            static const void* dtbl[256];
            static bool dtbl_ready = false;
            if (!dtbl_ready) {
                for (int di = 0; di < 256; ++di) { dtbl[di] = &&LBL_default; }
                dtbl[instr::halt_opcode] = &&LBL_halt_opcode;
                dtbl[instr::clr_opcode] = &&LBL_clr_opcode;
                dtbl[instr::setz_opcode] = &&LBL_setz_opcode;
                dtbl[instr::setnz_opcode] = &&LBL_setnz_opcode;
                dtbl[instr::setlt_opcode] = &&LBL_setlt_opcode;
                dtbl[instr::setb_opcode] = &&LBL_setb_opcode;
                dtbl[instr::setgt_opcode] = &&LBL_setgt_opcode;
                dtbl[instr::seta_opcode] = &&LBL_seta_opcode;
                dtbl[instr::setge_opcode] = &&LBL_setge_opcode;
                dtbl[instr::setle_opcode] = &&LBL_setle_opcode;
                dtbl[instr::setbe_opcode] = &&LBL_setbe_opcode;
                dtbl[instr::setae_opcode] = &&LBL_setae_opcode;
                dtbl[instr::setp_opcode] = &&LBL_setp_opcode;
                dtbl[instr::cp_regVal_reg] = &&LBL_cp_regVal_reg;
                dtbl[instr::cp_immVal_reg] = &&LBL_cp_immVal_reg;
                dtbl[instr::ld_regAddr_reg] = &&LBL_ld_regAddr_reg;
                dtbl[instr::ld_immAddr_reg] = &&LBL_ld_immAddr_reg;
                dtbl[instr::cpz_regVal_reg] = &&LBL_cpz_regVal_reg;
                dtbl[instr::cpz_immVal_reg] = &&LBL_cpz_immVal_reg;
                dtbl[instr::ldz_regAddr_reg] = &&LBL_ldz_regAddr_reg;
                dtbl[instr::ldz_immAddr_reg] = &&LBL_ldz_immAddr_reg;
                dtbl[instr::st_regVal_regAddr] = &&LBL_st_regVal_regAddr;
                dtbl[instr::st_immVal_regAddr] = &&LBL_st_immVal_regAddr;
                dtbl[instr::add_regVal_reg] = &&LBL_add_regVal_reg;
                dtbl[instr::sub_regVal_reg] = &&LBL_sub_regVal_reg;
                dtbl[instr::mul_regVal_reg] = &&LBL_mul_regVal_reg;
                dtbl[instr::div_regVal_reg] = &&LBL_div_regVal_reg;
                dtbl[instr::mod_regVal_reg] = &&LBL_mod_regVal_reg;
                dtbl[instr::udiv_regVal_reg] = &&LBL_udiv_regVal_reg;
                dtbl[instr::adc_regVal_reg] = &&LBL_adc_regVal_reg;
                dtbl[instr::sbb_regVal_reg] = &&LBL_sbb_regVal_reg;
                dtbl[instr::umod_regVal_reg] = &&LBL_umod_regVal_reg;
                dtbl[instr::and_regVal_reg] = &&LBL_and_regVal_reg;
                dtbl[instr::or_regVal_reg] = &&LBL_or_regVal_reg;
                dtbl[instr::nor_regVal_reg] = &&LBL_nor_regVal_reg;
                dtbl[instr::nand_regVal_reg] = &&LBL_nand_regVal_reg;
                dtbl[instr::xor_regVal_reg] = &&LBL_xor_regVal_reg;
                dtbl[instr::shl_regVal_reg] = &&LBL_shl_regVal_reg;
                dtbl[instr::shr_regVal_reg] = &&LBL_shr_regVal_reg;
                dtbl[instr::sar_regVal_reg] = &&LBL_sar_regVal_reg;
                dtbl[instr::cmp_regVal_reg] = &&LBL_cmp_regVal_reg;
                dtbl[instr::test_regVal_reg] = &&LBL_test_regVal_reg;
                dtbl[instr::add_immVal_reg] = &&LBL_add_immVal_reg;
                dtbl[instr::sub_immVal_reg] = &&LBL_sub_immVal_reg;
                dtbl[instr::mul_immVal_reg] = &&LBL_mul_immVal_reg;
                dtbl[instr::div_immVal_reg] = &&LBL_div_immVal_reg;
                dtbl[instr::mod_immVal_reg] = &&LBL_mod_immVal_reg;
                dtbl[instr::udiv_immVal_reg] = &&LBL_udiv_immVal_reg;
                dtbl[instr::adc_immVal_reg] = &&LBL_adc_immVal_reg;
                dtbl[instr::sbb_immVal_reg] = &&LBL_sbb_immVal_reg;
                dtbl[instr::umod_immVal_reg] = &&LBL_umod_immVal_reg;
                dtbl[instr::and_immVal_reg] = &&LBL_and_immVal_reg;
                dtbl[instr::or_immVal_reg] = &&LBL_or_immVal_reg;
                dtbl[instr::nor_immVal_reg] = &&LBL_nor_immVal_reg;
                dtbl[instr::nand_immVal_reg] = &&LBL_nand_immVal_reg;
                dtbl[instr::xor_immVal_reg] = &&LBL_xor_immVal_reg;
                dtbl[instr::shl_immVal_reg] = &&LBL_shl_immVal_reg;
                dtbl[instr::shr_immVal_reg] = &&LBL_shr_immVal_reg;
                dtbl[instr::sar_immVal_reg] = &&LBL_sar_immVal_reg;
                dtbl[instr::cmp_immVal_reg] = &&LBL_cmp_immVal_reg;
                dtbl[instr::test_immVal_reg] = &&LBL_test_immVal_reg;
                dtbl[instr::add_regAddr_reg] = &&LBL_add_regAddr_reg;
                dtbl[instr::sub_regAddr_reg] = &&LBL_sub_regAddr_reg;
                dtbl[instr::mul_regAddr_reg] = &&LBL_mul_regAddr_reg;
                dtbl[instr::div_regAddr_reg] = &&LBL_div_regAddr_reg;
                dtbl[instr::mod_regAddr_reg] = &&LBL_mod_regAddr_reg;
                dtbl[instr::udiv_regAddr_reg] = &&LBL_udiv_regAddr_reg;
                dtbl[instr::adc_regAddr_reg] = &&LBL_adc_regAddr_reg;
                dtbl[instr::sbb_regAddr_reg] = &&LBL_sbb_regAddr_reg;
                dtbl[instr::umod_regAddr_reg] = &&LBL_umod_regAddr_reg;
                dtbl[instr::and_regAddr_reg] = &&LBL_and_regAddr_reg;
                dtbl[instr::or_regAddr_reg] = &&LBL_or_regAddr_reg;
                dtbl[instr::nor_regAddr_reg] = &&LBL_nor_regAddr_reg;
                dtbl[instr::nand_regAddr_reg] = &&LBL_nand_regAddr_reg;
                dtbl[instr::xor_regAddr_reg] = &&LBL_xor_regAddr_reg;
                dtbl[instr::shl_regAddr_reg] = &&LBL_shl_regAddr_reg;
                dtbl[instr::shr_regAddr_reg] = &&LBL_shr_regAddr_reg;
                dtbl[instr::sar_regAddr_reg] = &&LBL_sar_regAddr_reg;
                dtbl[instr::cmp_regAddr_reg] = &&LBL_cmp_regAddr_reg;
                dtbl[instr::test_regAddr_reg] = &&LBL_test_regAddr_reg;
                dtbl[instr::add_immAddr_reg] = &&LBL_add_immAddr_reg;
                dtbl[instr::sub_immAddr_reg] = &&LBL_sub_immAddr_reg;
                dtbl[instr::mul_immAddr_reg] = &&LBL_mul_immAddr_reg;
                dtbl[instr::div_immAddr_reg] = &&LBL_div_immAddr_reg;
                dtbl[instr::mod_immAddr_reg] = &&LBL_mod_immAddr_reg;
                dtbl[instr::udiv_immAddr_reg] = &&LBL_udiv_immAddr_reg;
                dtbl[instr::adc_immAddr_reg] = &&LBL_adc_immAddr_reg;
                dtbl[instr::sbb_immAddr_reg] = &&LBL_sbb_immAddr_reg;
                dtbl[instr::umod_immAddr_reg] = &&LBL_umod_immAddr_reg;
                dtbl[instr::and_immAddr_reg] = &&LBL_and_immAddr_reg;
                dtbl[instr::or_immAddr_reg] = &&LBL_or_immAddr_reg;
                dtbl[instr::nor_immAddr_reg] = &&LBL_nor_immAddr_reg;
                dtbl[instr::nand_immAddr_reg] = &&LBL_nand_immAddr_reg;
                dtbl[instr::xor_immAddr_reg] = &&LBL_xor_immAddr_reg;
                dtbl[instr::shl_immAddr_reg] = &&LBL_shl_immAddr_reg;
                dtbl[instr::shr_immAddr_reg] = &&LBL_shr_immAddr_reg;
                dtbl[instr::sar_immAddr_reg] = &&LBL_sar_immAddr_reg;
                dtbl[instr::cmp_immAddr_reg] = &&LBL_cmp_immAddr_reg;
                dtbl[instr::test_immAddr_reg] = &&LBL_test_immAddr_reg;
                dtbl[instr::inc_opcode] = &&LBL_inc_opcode;
                dtbl[instr::dec_opcode] = &&LBL_dec_opcode;
                dtbl[instr::not_opcode] = &&LBL_not_opcode;
                dtbl[instr::neg_opcode] = &&LBL_neg_opcode;
                dtbl[instr::cmpind_immVal_regAddr] = &&LBL_cmpind_immVal_regAddr;
                dtbl[instr::testind_immVal_regAddr] = &&LBL_testind_immVal_regAddr;
                dtbl[instr::cmpind_regVal_regAddr] = &&LBL_cmpind_regVal_regAddr;
                dtbl[instr::testind_regVal_regAddr] = &&LBL_testind_regVal_regAddr;
                dtbl[instr::cmpxchg_regVal_regreg] = &&LBL_cmpxchg_regVal_regreg;
                dtbl[instr::cmpxchg_regAddr_regreg] = &&LBL_cmpxchg_regAddr_regreg;
                dtbl[instr::cmpxchg_immVal_regreg] = &&LBL_cmpxchg_immVal_regreg;
                dtbl[instr::cmpxchg_immAddr_regreg] = &&LBL_cmpxchg_immAddr_regreg;
                dtbl[instr::lea_regVal_regreg] = &&LBL_lea_regVal_regreg;
                dtbl[instr::lea_regAddr_regreg] = &&LBL_lea_regAddr_regreg;
                dtbl[instr::lea_immVal_regreg] = &&LBL_lea_immVal_regreg;
                dtbl[instr::lea_immAddr_regreg] = &&LBL_lea_immAddr_regreg;
                dtbl[instr::mulw_regVal_regreg] = &&LBL_mulw_regVal_regreg;
                dtbl[instr::umulw_regVal_regreg] = &&LBL_umulw_regVal_regreg;
                dtbl[instr::mulw_immVal_regreg] = &&LBL_mulw_immVal_regreg;
                dtbl[instr::umulw_immVal_regreg] = &&LBL_umulw_immVal_regreg;
                dtbl[instr::mulw_regAddr_regreg] = &&LBL_mulw_regAddr_regreg;
                dtbl[instr::umulw_regAddr_regreg] = &&LBL_umulw_regAddr_regreg;
                dtbl[instr::mulw_immAddr_regreg] = &&LBL_mulw_immAddr_regreg;
                dtbl[instr::umulw_immAddr_regreg] = &&LBL_umulw_immAddr_regreg;
                dtbl[instr::xchg_opcode] = &&LBL_xchg_opcode;
                dtbl[instr::out_regVal_imm] = &&LBL_out_regVal_imm;
                dtbl[instr::out_immVal_imm] = &&LBL_out_immVal_imm;
                dtbl[instr::out_regAddr_imm] = &&LBL_out_regAddr_imm;
                dtbl[instr::out_immAddr_imm] = &&LBL_out_immAddr_imm;
                dtbl[instr::outr_regVal_reg] = &&LBL_outr_regVal_reg;
                dtbl[instr::outr_immVal_imm] = &&LBL_outr_immVal_imm;
                dtbl[instr::outr_regAddr_imm] = &&LBL_outr_regAddr_imm;
                dtbl[instr::outr_immAddr_imm] = &&LBL_outr_immAddr_imm;
                dtbl[instr::in_regVal_imm] = &&LBL_in_regVal_imm;
                dtbl[instr::in_immVal_imm] = &&LBL_in_immVal_imm;
                dtbl[instr::in_regAddr_imm] = &&LBL_in_regAddr_imm;
                dtbl[instr::in_immAddr_imm] = &&LBL_in_immAddr_imm;
                dtbl[instr::sys_immVal] = &&LBL_sys_immVal;
                dtbl[instr::sys_regVal] = &&LBL_sys_regVal;
                dtbl[instr::pop_opcode] = &&LBL_pop_opcode;
                dtbl[instr::push_regVal] = &&LBL_push_regVal;
                dtbl[instr::push_immVal] = &&LBL_push_immVal;
                dtbl[instr::call_regVal] = &&LBL_call_regVal;
                dtbl[instr::call_immVal] = &&LBL_call_immVal;
                dtbl[instr::call_regAddr] = &&LBL_call_regAddr;
                dtbl[instr::call_immAddr] = &&LBL_call_immAddr;
                dtbl[instr::ret_opcode] = &&LBL_ret_opcode;
                dtbl[instr::iret_opcode] = &&LBL_iret_opcode;
                dtbl[instr::jmp_regVal] = &&LBL_jmp_regVal;
                dtbl[instr::jmp_immVal] = &&LBL_jmp_immVal;
                dtbl[instr::jmp_regAddr] = &&LBL_jmp_regAddr;
                dtbl[instr::jmp_immAddr] = &&LBL_jmp_immAddr;
                dtbl[instr::jz_opcode] = &&LBL_jz_opcode;
                dtbl[instr::jnz_opcode] = &&LBL_jnz_opcode;
                dtbl[instr::jlt_opcode] = &&LBL_jlt_opcode;
                dtbl[instr::jb_opcode] = &&LBL_jb_opcode;
                dtbl[instr::jgt_opcode] = &&LBL_jgt_opcode;
                dtbl[instr::ja_opcode] = &&LBL_ja_opcode;
                dtbl[instr::jge_opcode] = &&LBL_jge_opcode;
                dtbl[instr::jle_opcode] = &&LBL_jle_opcode;
                dtbl[instr::jbe_opcode] = &&LBL_jbe_opcode;
                dtbl[instr::jae_opcode] = &&LBL_jae_opcode;
                dtbl[instr::jp_opcode] = &&LBL_jp_opcode;
                dtbl[instr::setcry_opcode] = &&LBL_setcry_opcode;
                dtbl[instr::clrcry_opcode] = &&LBL_clrcry_opcode;
                dtbl[instr::setint_opcode] = &&LBL_setint_opcode;
                dtbl[instr::clrint_opcode] = &&LBL_clrint_opcode;
                dtbl[instr::setsysg_opcode] = &&LBL_setsysg_opcode;
                dtbl[instr::clrsysg_opcode] = &&LBL_clrsysg_opcode;
                dtbl[instr::movtcr_regVal_imm] = &&LBL_movtcr_regVal_imm;
                dtbl[instr::movtcr_immVal_imm] = &&LBL_movtcr_immVal_imm;
                dtbl[instr::movfcr_immVal_reg] = &&LBL_movfcr_immVal_reg;
                dtbl[instr::tlbinv_opcode] = &&LBL_tlbinv_opcode;
                dtbl[instr::tlbinva_opcode] = &&LBL_tlbinva_opcode;
                dtbl[instr::nop_opcode] = &&LBL_nop_opcode;
                dtbl[instr::brk_opcode] = &&LBL_brk_opcode;
                dtbl[instr::fadd_regVal_reg] = &&LBL_fadd_regVal_reg;
                dtbl[instr::fsub_regVal_reg] = &&LBL_fsub_regVal_reg;
                dtbl[instr::fmul_regVal_reg] = &&LBL_fmul_regVal_reg;
                dtbl[instr::fdiv_regVal_reg] = &&LBL_fdiv_regVal_reg;
                dtbl[instr::fadd_immVal_reg] = &&LBL_fadd_immVal_reg;
                dtbl[instr::fsub_immVal_reg] = &&LBL_fsub_immVal_reg;
                dtbl[instr::fmul_immVal_reg] = &&LBL_fmul_immVal_reg;
                dtbl[instr::fdiv_immVal_reg] = &&LBL_fdiv_immVal_reg;
                dtbl[instr::fadd_regAddr_reg] = &&LBL_fadd_regAddr_reg;
                dtbl[instr::fsub_regAddr_reg] = &&LBL_fsub_regAddr_reg;
                dtbl[instr::fmul_regAddr_reg] = &&LBL_fmul_regAddr_reg;
                dtbl[instr::fdiv_regAddr_reg] = &&LBL_fdiv_regAddr_reg;
                dtbl[instr::fadd_immAddr_reg] = &&LBL_fadd_immAddr_reg;
                dtbl[instr::fsub_immAddr_reg] = &&LBL_fsub_immAddr_reg;
                dtbl[instr::fmul_immAddr_reg] = &&LBL_fmul_immAddr_reg;
                dtbl[instr::fdiv_immAddr_reg] = &&LBL_fdiv_immAddr_reg;
                dtbl[instr::fcmp_regVal_reg] = &&LBL_fcmp_regVal_reg;
                dtbl[instr::fcmp_immVal_reg] = &&LBL_fcmp_immVal_reg;
                dtbl[instr::fcmp_regAddr_reg] = &&LBL_fcmp_regAddr_reg;
                dtbl[instr::fcmp_immAddr_reg] = &&LBL_fcmp_immAddr_reg;
                dtbl[instr::fsqrt_opcode] = &&LBL_fsqrt_opcode;
                dtbl[instr::fneg_opcode] = &&LBL_fneg_opcode;
                dtbl[instr::fabs_opcode] = &&LBL_fabs_opcode;
                dtbl[instr::fmin_opcode] = &&LBL_fmin_opcode;
                dtbl[instr::fmax_opcode] = &&LBL_fmax_opcode;
                dtbl[instr::fcvtff_opcode] = &&LBL_fcvtff_opcode;
                dtbl[instr::fcvtfs_opcode] = &&LBL_fcvtfs_opcode;
                dtbl[instr::fcvtfu_opcode] = &&LBL_fcvtfu_opcode;
                dtbl[instr::fcvtsf_opcode] = &&LBL_fcvtsf_opcode;
                dtbl[instr::fcvtuf_opcode] = &&LBL_fcvtuf_opcode;
                dtbl[instr::fmadd_regVal_regreg] = &&LBL_fmadd_regVal_regreg;
                dtbl[instr::fmsub_regVal_regreg] = &&LBL_fmsub_regVal_regreg;
                dtbl[instr::fmadd_immVal_regreg] = &&LBL_fmadd_immVal_regreg;
                dtbl[instr::fmsub_immVal_regreg] = &&LBL_fmsub_immVal_regreg;
                dtbl[instr::fmadd_regAddr_regreg] = &&LBL_fmadd_regAddr_regreg;
                dtbl[instr::fmsub_regAddr_regreg] = &&LBL_fmsub_regAddr_regreg;
                dtbl[instr::fmadd_immAddr_regreg] = &&LBL_fmadd_immAddr_regreg;
                dtbl[instr::fmsub_immAddr_regreg] = &&LBL_fmsub_immAddr_regreg;
                dtbl[instr::fgetcsr_opcode] = &&LBL_fgetcsr_opcode;
                dtbl[instr::fsetcsr_opcode] = &&LBL_fsetcsr_opcode;
                dtbl_ready = true;
            }

            /* Token-threaded dispatch: each handler tail-calls MAIZE_NEXT, which does
               the per-instruction preamble (interrupt/timer/input, fetch) and jumps
               straight to the next handler via its own indirect branch, so the
               predictor learns opcode-pair correlations instead of funneling every
               opcode through one shared switch branch. */
#define MAIZE_NEXT() do { \
                if (!running_flag) { goto tick_exit; } \
                if (interrupt_enabled_flag && irq_pending.load(std::memory_order_relaxed)) { \
                    try_deliver_interrupt(); \
                } \
                if (active_timer_ptr != nullptr) { tick_active_timer(*active_timer_ptr); } \
                if (active_input_ptr != nullptr) { active_input_ptr->on_input_tick(); } \
                current_instr_pc = regs::rp.w0; \
                mm.read(translate(regs::rp.w0, access_kind::fetch), regs::ri, subreg_enum::w0); \
                ++regs::rp.w0; \
                if (perf_count_enabled) { ++perf_insn_count; } \
                goto *dtbl[regs::ri.b0()]; \
            } while (0)

            MAIZE_NEXT();   // enter the thread
                    LBL_halt_opcode: {
                        /* HALT is wait-for-interrupt (card maize-21 park). With interrupts
                           ENABLED a source can wake the core, so park: drop out of tick()
                           keeping power on (running_flag clear, is_power_on set), and run()'s
                           wait-for-interrupt loop blocks on int_event until a deliverable IRQ
                           arrives, then delivers it and re-enters tick(). With interrupts
                           DISABLED nothing can ever wake the core, so HALT is a permanent halt
                           and powers off (exit). This preserves every HALT-to-end program
                           (hello.mzb and the whole asm suite start interrupts-disabled and end
                           in HALT, so they exit exactly as before); only a guest that opts in
                           with SETINT + an installed handler parks. It carries no exit status;
                           the status-carrying path is SYS $3C (sys_exit), see src/sys.cpp. */
                        if (!privilege_flag) { raise_privileged_op(); }   // privileged (card maize-180, §9)
                        if (interrupt_enabled_flag) {
                            running_flag = false;   // park; is_power_on stays set
                        }
                        else {
                            power_off();
                        }
                        MAIZE_NEXT();
                    }

                    LBL_clr_opcode: {
                        regs::rp.w0 += 1;
                        clr_reg(op1_reg(), op1_subreg_flag());
                        MAIZE_NEXT();
                    }

                    /* SETcc (cards maize-55 / maize-64): materialize a flag condition
                       as a 0/1 value in the single register operand. The condition is
                       decoded from the opcode's row/column bits and evaluated by the
                       shared eval_condition predicate table, the SAME table Jcc uses,
                       so the two families can never disagree. Flag-neutral: RF is read
                       but never written. */
                    LBL_setz_opcode:
                    LBL_setnz_opcode:
                    LBL_setlt_opcode:
                    LBL_setb_opcode:
                    LBL_setgt_opcode:
                    LBL_seta_opcode:
                    LBL_setge_opcode:
                    LBL_setle_opcode:
                    LBL_setbe_opcode:
                    LBL_setae_opcode:
                    LBL_setp_opcode: {
                        regs::rp.w0 += 1;
                        set_reg(op1_reg(), op1_subreg_flag(),
                            eval_condition(decode_condition(instr::setcc_base)));
                        MAIZE_NEXT();
                    }

                    LBL_cp_regVal_reg: {
                        regs::rp.w0 += 2;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_cp_immVal_reg: {
                        regs::rp.w0 += 2;
                        u_hword imm_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, imm_size, op2_reg(), op2_subreg_flag(), access_kind::fetch);
                        regs::rp.w0 += imm_size;
                        MAIZE_NEXT();
                    }

                    LBL_ld_regAddr_reg: {
                        regs::rp.w0 += 2;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_ld_immAddr_reg: {
                        regs::rp.w0 += 2;
                        u_hword imm_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, imm_size, op2_reg(), op2_subreg_flag());
                        regs::rp.w0 += imm_size;
                        MAIZE_NEXT();
                    }

                    LBL_cpz_regVal_reg: {
                        regs::rp.w0 += 2;
                        copy_regval_reg_zext(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_cpz_immVal_reg: {
                        regs::rp.w0 += 2;
                        u_hword imm_size = op1_imm_size();
                        copy_memval_reg_zext(regs::rp.w0, imm_size, op2_reg(), op2_subreg_flag(), access_kind::fetch);
                        regs::rp.w0 += imm_size;
                        MAIZE_NEXT();
                    }

                    /* LDZ (the zero-extending load, opcodes $93 / $D3, reintroduced by card
                       maize-204): reads N bytes (N = destination subregister width) like LD, then
                       zero-extends the result into the FULL destination register. Equivalent to
                       LD dst.<width> followed by CPZ dst.<width> dst, in one instruction. These
                       two address-form encodings were reserved for exactly this since maize-29
                       removed the original LDZ. */

                    LBL_ldz_regAddr_reg: {
                        regs::rp.w0 += 2;
                        copy_regaddr_reg_zext(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_ldz_immAddr_reg: {
                        regs::rp.w0 += 2;
                        u_hword imm_size = op1_imm_size();
                        copy_memaddr_reg_zext(regs::rp.w0, imm_size, op2_reg(), op2_subreg_flag());
                        regs::rp.w0 += imm_size;
                        MAIZE_NEXT();
                    }

                    LBL_st_regVal_regAddr: {
                        regs::rp.w0 += 2;
                        copy_regval_regaddr(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_st_immVal_regAddr: {
                        regs::rp.w0 += 2;
                        u_hword imm_size = op1_imm_size();
                        copy_memval_regaddr(regs::rp.w0, imm_size, op2_reg(), op2_subreg_flag());
                        regs::rp.w0 += imm_size;
                        MAIZE_NEXT();
                    }

                    /* card maize-198: in-place, per-op ALU dispatch. Each of the four
                       addressing-mode families gets ONE label per op (19 ops), replacing
                       the old shared block that staged both operands into the alu.op1_reg/
                       op2_reg bank and re-decoded the opcode through run_alu's switch. Each
                       label now reads the source (sign-extended, Decision D3) and the
                       destination directly, calls its width-specialized computation function
                       ONCE (no second dispatch), and writes the result back with
                       write_subreg_bits, EXCEPT CMP/TEST which discard it (Decision D2). The
                       destination register accessor op2_reg() is invoked twice (dst read +
                       writeback), preserving the maize-197 RF-materialize hook semantics
                       exactly as the old two-copy-through-op2_reg() shape did. src/dst reads
                       precede the ADC/SBB carry-in materialize inside the compute function,
                       matching run_alu's relative ordering (cpu.cpp carryout read). */

                    /* regVal_reg: source is a register, read directly (no staging bank). */
#define MAIZE_ALU_RV(FN, OPK) { \
                        regs::rp.w0 += 2; \
                        u_word alu_src = read_subreg_signext(op1_reg(), op1_subreg_flag()); \
                        u_byte alu_size = op2_subreg_size(); \
                        u_word alu_dst = read_subreg_bits(op2_reg(), op2_subreg_flag()); \
                        u_word alu_res = FN(OPK, alu_size, alu_src, alu_dst); \
                        write_subreg_bits(op2_reg(), op2_subreg_flag(), alu_res); \
                        MAIZE_NEXT(); \
                    }
#define MAIZE_ALU_RV_NW(FN, OPK) { \
                        regs::rp.w0 += 2; \
                        u_word alu_src = read_subreg_signext(op1_reg(), op1_subreg_flag()); \
                        u_byte alu_size = op2_subreg_size(); \
                        u_word alu_dst = read_subreg_bits(op2_reg(), op2_subreg_flag()); \
                        (void) FN(OPK, alu_size, alu_src, alu_dst); \
                        MAIZE_NEXT(); \
                    }
                    /* immVal_reg: source is an instruction-stream immediate, fetched (and
                       sign-extended) by copy_memval_reg into the alu.op1_reg scratch exactly
                       as before, then read back as a full word. */
#define MAIZE_ALU_IV(FN, OPK) { \
                        regs::rp.w0 += 2; \
                        u_byte alu_src_size = op1_imm_size(); \
                        copy_memval_reg(regs::rp.w0, alu_src_size, alu.op1_reg, subreg_enum::w0, access_kind::fetch); \
                        u_word alu_src = alu.op1_reg.w0; \
                        u_byte alu_size = op2_subreg_size(); \
                        u_word alu_dst = read_subreg_bits(op2_reg(), op2_subreg_flag()); \
                        u_word alu_res = FN(OPK, alu_size, alu_src, alu_dst); \
                        regs::rp.w0 += alu_src_size; \
                        write_subreg_bits(op2_reg(), op2_subreg_flag(), alu_res); \
                        MAIZE_NEXT(); \
                    }
#define MAIZE_ALU_IV_NW(FN, OPK) { \
                        regs::rp.w0 += 2; \
                        u_byte alu_src_size = op1_imm_size(); \
                        copy_memval_reg(regs::rp.w0, alu_src_size, alu.op1_reg, subreg_enum::w0, access_kind::fetch); \
                        u_word alu_src = alu.op1_reg.w0; \
                        u_byte alu_size = op2_subreg_size(); \
                        u_word alu_dst = read_subreg_bits(op2_reg(), op2_subreg_flag()); \
                        (void) FN(OPK, alu_size, alu_src, alu_dst); \
                        regs::rp.w0 += alu_src_size; \
                        MAIZE_NEXT(); \
                    }
                    /* regAddr_reg: source is loaded indirectly through a register by
                       copy_regaddr_reg into the alu.op1_reg scratch. */
#define MAIZE_ALU_RA(FN, OPK) { \
                        regs::rp.w0 += 2; \
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0); \
                        u_word alu_src = alu.op1_reg.w0; \
                        u_byte alu_size = op2_subreg_size(); \
                        u_word alu_dst = read_subreg_bits(op2_reg(), op2_subreg_flag()); \
                        u_word alu_res = FN(OPK, alu_size, alu_src, alu_dst); \
                        write_subreg_bits(op2_reg(), op2_subreg_flag(), alu_res); \
                        MAIZE_NEXT(); \
                    }
#define MAIZE_ALU_RA_NW(FN, OPK) { \
                        regs::rp.w0 += 2; \
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0); \
                        u_word alu_src = alu.op1_reg.w0; \
                        u_byte alu_size = op2_subreg_size(); \
                        u_word alu_dst = read_subreg_bits(op2_reg(), op2_subreg_flag()); \
                        (void) FN(OPK, alu_size, alu_src, alu_dst); \
                        MAIZE_NEXT(); \
                    }
                    /* immAddr_reg: source is loaded indirectly through an immediate address
                       by copy_memaddr_reg into the alu.op1_reg scratch. */
#define MAIZE_ALU_IA(FN, OPK) { \
                        regs::rp.w0 += 2; \
                        u_byte alu_src_size = op1_imm_size(); \
                        copy_memaddr_reg(regs::rp.w0, alu_src_size, alu.op1_reg, subreg_enum::w0); \
                        u_word alu_src = alu.op1_reg.w0; \
                        u_byte alu_size = op2_subreg_size(); \
                        u_word alu_dst = read_subreg_bits(op2_reg(), op2_subreg_flag()); \
                        u_word alu_res = FN(OPK, alu_size, alu_src, alu_dst); \
                        regs::rp.w0 += alu_src_size; \
                        write_subreg_bits(op2_reg(), op2_subreg_flag(), alu_res); \
                        MAIZE_NEXT(); \
                    }
#define MAIZE_ALU_IA_NW(FN, OPK) { \
                        regs::rp.w0 += 2; \
                        u_byte alu_src_size = op1_imm_size(); \
                        copy_memaddr_reg(regs::rp.w0, alu_src_size, alu.op1_reg, subreg_enum::w0); \
                        u_word alu_src = alu.op1_reg.w0; \
                        u_byte alu_size = op2_subreg_size(); \
                        u_word alu_dst = read_subreg_bits(op2_reg(), op2_subreg_flag()); \
                        (void) FN(OPK, alu_size, alu_src, alu_dst); \
                        regs::rp.w0 += alu_src_size; \
                        MAIZE_NEXT(); \
                    }

                    LBL_add_regVal_reg:  MAIZE_ALU_RV(alu_add,  instr::add_opcode)
                    LBL_sub_regVal_reg:  MAIZE_ALU_RV(alu_sub,  instr::sub_opcode)
                    LBL_mul_regVal_reg:  MAIZE_ALU_RV(alu_mul,  instr::mul_opcode)
                    LBL_div_regVal_reg:  MAIZE_ALU_RV(alu_div,  instr::div_opcode)
                    LBL_mod_regVal_reg:  MAIZE_ALU_RV(alu_mod,  instr::mod_opcode)
                    LBL_udiv_regVal_reg: MAIZE_ALU_RV(alu_udiv, instr::udiv_opcode)
                    LBL_adc_regVal_reg:  MAIZE_ALU_RV(alu_adc,  instr::adc_opcode)
                    LBL_sbb_regVal_reg:  MAIZE_ALU_RV(alu_sbb,  instr::sbb_opcode)
                    LBL_umod_regVal_reg: MAIZE_ALU_RV(alu_umod, instr::umod_opcode)
                    LBL_and_regVal_reg:  MAIZE_ALU_RV(alu_and,  instr::and_opcode)
                    LBL_or_regVal_reg:   MAIZE_ALU_RV(alu_or,   instr::or_opcode)
                    LBL_nor_regVal_reg:  MAIZE_ALU_RV(alu_nor,  instr::nor_opcode)
                    LBL_nand_regVal_reg: MAIZE_ALU_RV(alu_nand, instr::nand_opcode)
                    LBL_xor_regVal_reg:  MAIZE_ALU_RV(alu_xor,  instr::xor_opcode)
                    LBL_shl_regVal_reg:  MAIZE_ALU_RV(alu_shl,  instr::shl_opcode)
                    LBL_shr_regVal_reg:  MAIZE_ALU_RV(alu_shr,  instr::shr_opcode)
                    LBL_sar_regVal_reg:  MAIZE_ALU_RV(alu_sar,  instr::sar_opcode)
                    LBL_cmp_regVal_reg:  MAIZE_ALU_RV_NW(alu_sub, instr::cmp_opcode)
                    LBL_test_regVal_reg: MAIZE_ALU_RV_NW(alu_and, instr::test_opcode)

                    LBL_add_immVal_reg:  MAIZE_ALU_IV(alu_add,  instr::add_opcode)
                    LBL_sub_immVal_reg:  MAIZE_ALU_IV(alu_sub,  instr::sub_opcode)
                    LBL_mul_immVal_reg:  MAIZE_ALU_IV(alu_mul,  instr::mul_opcode)
                    LBL_div_immVal_reg:  MAIZE_ALU_IV(alu_div,  instr::div_opcode)
                    LBL_mod_immVal_reg:  MAIZE_ALU_IV(alu_mod,  instr::mod_opcode)
                    LBL_udiv_immVal_reg: MAIZE_ALU_IV(alu_udiv, instr::udiv_opcode)
                    LBL_adc_immVal_reg:  MAIZE_ALU_IV(alu_adc,  instr::adc_opcode)
                    LBL_sbb_immVal_reg:  MAIZE_ALU_IV(alu_sbb,  instr::sbb_opcode)
                    LBL_umod_immVal_reg: MAIZE_ALU_IV(alu_umod, instr::umod_opcode)
                    LBL_and_immVal_reg:  MAIZE_ALU_IV(alu_and,  instr::and_opcode)
                    LBL_or_immVal_reg:   MAIZE_ALU_IV(alu_or,   instr::or_opcode)
                    LBL_nor_immVal_reg:  MAIZE_ALU_IV(alu_nor,  instr::nor_opcode)
                    LBL_nand_immVal_reg: MAIZE_ALU_IV(alu_nand, instr::nand_opcode)
                    LBL_xor_immVal_reg:  MAIZE_ALU_IV(alu_xor,  instr::xor_opcode)
                    LBL_shl_immVal_reg:  MAIZE_ALU_IV(alu_shl,  instr::shl_opcode)
                    LBL_shr_immVal_reg:  MAIZE_ALU_IV(alu_shr,  instr::shr_opcode)
                    LBL_sar_immVal_reg:  MAIZE_ALU_IV(alu_sar,  instr::sar_opcode)
                    LBL_cmp_immVal_reg:  MAIZE_ALU_IV_NW(alu_sub, instr::cmp_opcode)
                    LBL_test_immVal_reg: MAIZE_ALU_IV_NW(alu_and, instr::test_opcode)

                    LBL_add_regAddr_reg:  MAIZE_ALU_RA(alu_add,  instr::add_opcode)
                    LBL_sub_regAddr_reg:  MAIZE_ALU_RA(alu_sub,  instr::sub_opcode)
                    LBL_mul_regAddr_reg:  MAIZE_ALU_RA(alu_mul,  instr::mul_opcode)
                    LBL_div_regAddr_reg:  MAIZE_ALU_RA(alu_div,  instr::div_opcode)
                    LBL_mod_regAddr_reg:  MAIZE_ALU_RA(alu_mod,  instr::mod_opcode)
                    LBL_udiv_regAddr_reg: MAIZE_ALU_RA(alu_udiv, instr::udiv_opcode)
                    LBL_adc_regAddr_reg:  MAIZE_ALU_RA(alu_adc,  instr::adc_opcode)
                    LBL_sbb_regAddr_reg:  MAIZE_ALU_RA(alu_sbb,  instr::sbb_opcode)
                    LBL_umod_regAddr_reg: MAIZE_ALU_RA(alu_umod, instr::umod_opcode)
                    LBL_and_regAddr_reg:  MAIZE_ALU_RA(alu_and,  instr::and_opcode)
                    LBL_or_regAddr_reg:   MAIZE_ALU_RA(alu_or,   instr::or_opcode)
                    LBL_nor_regAddr_reg:  MAIZE_ALU_RA(alu_nor,  instr::nor_opcode)
                    LBL_nand_regAddr_reg: MAIZE_ALU_RA(alu_nand, instr::nand_opcode)
                    LBL_xor_regAddr_reg:  MAIZE_ALU_RA(alu_xor,  instr::xor_opcode)
                    LBL_shl_regAddr_reg:  MAIZE_ALU_RA(alu_shl,  instr::shl_opcode)
                    LBL_shr_regAddr_reg:  MAIZE_ALU_RA(alu_shr,  instr::shr_opcode)
                    LBL_sar_regAddr_reg:  MAIZE_ALU_RA(alu_sar,  instr::sar_opcode)
                    LBL_cmp_regAddr_reg:  MAIZE_ALU_RA_NW(alu_sub, instr::cmp_opcode)
                    LBL_test_regAddr_reg: MAIZE_ALU_RA_NW(alu_and, instr::test_opcode)

                    LBL_add_immAddr_reg:  MAIZE_ALU_IA(alu_add,  instr::add_opcode)
                    LBL_sub_immAddr_reg:  MAIZE_ALU_IA(alu_sub,  instr::sub_opcode)
                    LBL_mul_immAddr_reg:  MAIZE_ALU_IA(alu_mul,  instr::mul_opcode)
                    LBL_div_immAddr_reg:  MAIZE_ALU_IA(alu_div,  instr::div_opcode)
                    LBL_mod_immAddr_reg:  MAIZE_ALU_IA(alu_mod,  instr::mod_opcode)
                    LBL_udiv_immAddr_reg: MAIZE_ALU_IA(alu_udiv, instr::udiv_opcode)
                    LBL_adc_immAddr_reg:  MAIZE_ALU_IA(alu_adc,  instr::adc_opcode)
                    LBL_sbb_immAddr_reg:  MAIZE_ALU_IA(alu_sbb,  instr::sbb_opcode)
                    LBL_umod_immAddr_reg: MAIZE_ALU_IA(alu_umod, instr::umod_opcode)
                    LBL_and_immAddr_reg:  MAIZE_ALU_IA(alu_and,  instr::and_opcode)
                    LBL_or_immAddr_reg:   MAIZE_ALU_IA(alu_or,   instr::or_opcode)
                    LBL_nor_immAddr_reg:  MAIZE_ALU_IA(alu_nor,  instr::nor_opcode)
                    LBL_nand_immAddr_reg: MAIZE_ALU_IA(alu_nand, instr::nand_opcode)
                    LBL_xor_immAddr_reg:  MAIZE_ALU_IA(alu_xor,  instr::xor_opcode)
                    LBL_shl_immAddr_reg:  MAIZE_ALU_IA(alu_shl,  instr::shl_opcode)
                    LBL_shr_immAddr_reg:  MAIZE_ALU_IA(alu_shr,  instr::shr_opcode)
                    LBL_sar_immAddr_reg:  MAIZE_ALU_IA(alu_sar,  instr::sar_opcode)
                    LBL_cmp_immAddr_reg:  MAIZE_ALU_IA_NW(alu_sub, instr::cmp_opcode)
                    LBL_test_immAddr_reg: MAIZE_ALU_IA_NW(alu_and, instr::test_opcode)

#undef MAIZE_ALU_RV
#undef MAIZE_ALU_RV_NW
#undef MAIZE_ALU_IV
#undef MAIZE_ALU_IV_NW
#undef MAIZE_ALU_RA
#undef MAIZE_ALU_RA_NW
#undef MAIZE_ALU_IA
#undef MAIZE_ALU_IA_NW

                    /* Packed unary ALU family (card maize-64): INC ($31) / DEC ($71) /
                       NOT ($B1) / NEG ($F1) share base slot $31, distinguished by the
                       condition-style row bits. run_alu dispatches on alu.b0() & opflag_code,
                       so translate the row to a low-6-unique micro-op selector first. */
                    LBL_inc_opcode:
                    LBL_dec_opcode:
                    LBL_not_opcode:
                    LBL_neg_opcode: {
                        regs::rp.w0 += 1;
                        static const u_byte uop_sel[4] {alu_uop_inc, alu_uop_dec, alu_uop_not, alu_uop_neg};
                        u_byte row = (regs::ri.b0() & opcode_flag) >> 6;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.set_b0(uop_sel[row]);
                        alu.set_b1(op1_subreg_size());
                        alu.set_b2(op1_subreg_size());
                        run_alu();
                        /* Value writeback is flag-neutral (card maize-4); the ALU's flags stand. */
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op1_reg(), op1_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_cmpind_immVal_regAddr:
                    LBL_testind_immVal_regAddr:
                    {
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0, access_kind::fetch);
                        copy_regaddr_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, op2_subreg_flag());
                        regs::rp.w0 += src_size;
                        alu.set_b0(regs::ri.b0());
                        alu.set_b1(src_size);
                        alu.set_b2(op1_subreg_size());
                        run_alu();
                        MAIZE_NEXT();
                    }

                    LBL_cmpind_regVal_regAddr:
                    LBL_testind_regVal_regAddr:
                    {
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_subreg_size();
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regaddr_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, op2_subreg_flag());
                        alu.set_b0(regs::ri.b0());
                        alu.set_b1(src_size);
                        alu.set_b2(op1_subreg_size());
                        run_alu();
                        MAIZE_NEXT();
                    }

                    LBL_cmpxchg_regVal_regreg: {
                        regs::rp.w0 += 3;
                        /* card maize-197: CMPXCHG writes only Z, leaving N/C/V as they were.
                           Resolve any pending descriptor first so those bits are concrete
                           before the narrow Z write, and so a later materialize cannot undo it. */
                        materialize_flags();

                        if (cmp_regval_reg(op3_reg(), op3_subreg_flag(), op2_reg(), op2_subreg_flag())) {
                            zero_flag = 1;
                            copy_regval_reg(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        }
                        else {
                            zero_flag = 0;
                            copy_regval_reg(op2_reg(), op2_subreg_flag(), op3_reg(), op3_subreg_flag());
                        }

                        MAIZE_NEXT();
                    }

                    LBL_cmpxchg_regAddr_regreg: {
                        regs::rp.w0 += 3;
                        /* card maize-197: resolve pending flags before CMPXCHG's narrow Z write. */
                        materialize_flags();

                        if (cmp_regval_reg(op3_reg(), op3_subreg_flag(), op2_reg(), op2_subreg_flag())) {
                            zero_flag = 1;
                            copy_regaddr_reg(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        }
                        else {
                            zero_flag = 0;
                            copy_regval_reg(op2_reg(), op2_subreg_flag(), op3_reg(), op3_subreg_flag());
                        }

                        MAIZE_NEXT();
                    }

                    LBL_cmpxchg_immVal_regreg: {
                        regs::rp.w0 += 3;
                        u_byte src_size = op1_imm_size();
                        regs::rp.w0 += src_size;
                        /* card maize-197: resolve pending flags before CMPXCHG's narrow Z write. */
                        materialize_flags();

                        if (cmp_regval_reg(op3_reg(), op3_subreg_flag(), op2_reg(), op2_subreg_flag())) {
                            zero_flag = 1;
                            copy_memval_reg(regs::rp.w0, src_size, op2_reg(), op2_subreg_flag(), access_kind::fetch);
                        }
                        else {
                            zero_flag = 0;
                            copy_regval_reg(op2_reg(), op2_subreg_flag(), op3_reg(), op3_subreg_flag());
                        }

                        MAIZE_NEXT();
                    }

                    LBL_cmpxchg_immAddr_regreg: {
                        regs::rp.w0 += 3;
                        u_byte src_size = op1_imm_size();
                        regs::rp.w0 += src_size;
                        /* card maize-197: resolve pending flags before CMPXCHG's narrow Z write. */
                        materialize_flags();

                        if (cmp_regval_reg(op3_reg(), op3_subreg_flag(), op2_reg(), op2_subreg_flag())) {
                            zero_flag = 1;
                            copy_memaddr_reg(regs::rp.w0, src_size, op2_reg(), op2_subreg_flag());
                        }
                        else {
                            zero_flag = 0;
                            copy_regval_reg(op2_reg(), op2_subreg_flag(), op3_reg(), op3_subreg_flag());
                        }

                        MAIZE_NEXT();
                    }

                    LBL_lea_regVal_regreg: {
                        regs::rp.w0 += 3;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.set_b0(instr::add_regVal_reg);
                        alu.set_b1(op1_subreg_size());
                        alu.set_b2(op2_subreg_size());
                        /* LEA computes an effective address and must not disturb the flags
                           (card maize-4). The add runs through the ALU, so snapshot and restore
                           FL (RF.H0) around it. */
                        /* card maize-197: under lazy flags the internal ADD stages a NEW
                           pending descriptor instead of writing RF eagerly, so restoring the
                           raw RF.H0 bits is no longer enough. Snapshot and restore the WHOLE
                           pending_flags_t as well, or a later consumer would materialize LEA's
                           internal add and corrupt the flags LEA must leave untouched. */
                        pending_flags_t saved_pending = pending_flags;
                        u_hword saved_fl = regs::rf.h0();
                        run_alu();
                        pending_flags = saved_pending;
                        regs::rf.set_h0(saved_fl);
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_lea_regAddr_regreg: {
                        regs::rp.w0 += 3;
                        u_byte src_size = op1_imm_size();
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.set_b0(instr::add_regVal_reg);
                        alu.set_b1(src_size);
                        alu.set_b2(op2_subreg_size());
                        /* LEA is flag-neutral (card maize-4); preserve FL across the ALU add. */
                        /* card maize-197: under lazy flags the internal ADD stages a NEW
                           pending descriptor instead of writing RF eagerly, so restoring the
                           raw RF.H0 bits is no longer enough. Snapshot and restore the WHOLE
                           pending_flags_t as well, or a later consumer would materialize LEA's
                           internal add and corrupt the flags LEA must leave untouched. */
                        pending_flags_t saved_pending = pending_flags;
                        u_hword saved_fl = regs::rf.h0();
                        run_alu();
                        pending_flags = saved_pending;
                        regs::rf.set_h0(saved_fl);
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_lea_immVal_regreg: {
                        regs::rp.w0 += 3;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0, access_kind::fetch);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.set_b0(instr::add_immVal_reg);
                        alu.set_b1(src_size);
                        alu.set_b2(op2_subreg_size());
                        /* LEA is flag-neutral (card maize-4); preserve FL across the ALU add. */
                        /* card maize-197: under lazy flags the internal ADD stages a NEW
                           pending descriptor instead of writing RF eagerly, so restoring the
                           raw RF.H0 bits is no longer enough. Snapshot and restore the WHOLE
                           pending_flags_t as well, or a later consumer would materialize LEA's
                           internal add and corrupt the flags LEA must leave untouched. */
                        pending_flags_t saved_pending = pending_flags;
                        u_hword saved_fl = regs::rf.h0();
                        run_alu();
                        pending_flags = saved_pending;
                        regs::rf.set_h0(saved_fl);
                        regs::rp.w0 += src_size;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_lea_immAddr_regreg: {
                        regs::rp.w0 += 3;
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.set_b0(instr::add_immAddr_reg);
                        alu.set_b1(src_size);
                        alu.set_b2(op2_subreg_size());
                        /* LEA is flag-neutral (card maize-4); preserve FL across the ALU add. */
                        /* card maize-197: under lazy flags the internal ADD stages a NEW
                           pending descriptor instead of writing RF eagerly, so restoring the
                           raw RF.H0 bits is no longer enough. Snapshot and restore the WHOLE
                           pending_flags_t as well, or a later consumer would materialize LEA's
                           internal add and corrupt the flags LEA must leave untouched. */
                        pending_flags_t saved_pending = pending_flags;
                        u_hword saved_fl = regs::rf.h0();
                        run_alu();
                        pending_flags = saved_pending;
                        regs::rf.set_h0(saved_fl);
                        regs::rp.w0 += src_size;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        MAIZE_NEXT();
                    }

                    /* Wide multiply (card maize-7): 3-operand src/dst/hi form modeled on LEA, but
                       flag-setting (run_alu leaves C/N/V/Z as computed) and writing BOTH dst (low
                       half, from alu.op2_reg) and hi (high half, from alu.op1_reg). The operation
                       width is dst's subregister (op2_subreg_size); hi is written at that width. */
                    LBL_mulw_regVal_regreg:
                    LBL_umulw_regVal_regreg: {
                        regs::rp.w0 += 3;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.set_b0(regs::ri.b0());
                        alu.set_b1(op1_subreg_size());
                        alu.set_b2(op2_subreg_size());
                        run_alu();
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        copy_regval_reg(alu.op1_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_mulw_immVal_regreg:
                    LBL_umulw_immVal_regreg: {
                        regs::rp.w0 += 3;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0, access_kind::fetch);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.set_b0(regs::ri.b0());
                        alu.set_b1(src_size);
                        alu.set_b2(op2_subreg_size());
                        run_alu();
                        regs::rp.w0 += src_size;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        copy_regval_reg(alu.op1_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_mulw_regAddr_regreg:
                    LBL_umulw_regAddr_regreg: {
                        regs::rp.w0 += 3;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.set_b0(regs::ri.b0());
                        alu.set_b1(op1_subreg_size());
                        alu.set_b2(op2_subreg_size());
                        run_alu();
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        copy_regval_reg(alu.op1_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_mulw_immAddr_regreg:
                    LBL_umulw_immAddr_regreg: {
                        regs::rp.w0 += 3;
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.set_b0(regs::ri.b0());
                        alu.set_b1(src_size);
                        alu.set_b2(op2_subreg_size());
                        run_alu();
                        regs::rp.w0 += src_size;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        copy_regval_reg(alu.op1_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_xchg_opcode: {
                        regs::rp.w0 += 2;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), operand1, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), op1_reg(), op1_subreg_flag());
                        copy_regval_reg(operand1, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        MAIZE_NEXT();
                    }

                    /* OUT / OUTR / IN (card maize-21): all twelve dispatch sites are
                       privileged and route their device access through the single shared
                       find_device helper (via port_write / port_read), which applies the
                       frozen read-0 / write-discard outcome on an unpopulated port instead
                       of the old devices[id] value-initialize-null-then-dereference crash.
                       The privilege gate is at the head of each case: executed with the RF
                       privilege bit clear (user mode) the instruction raises the cause-4
                       privileged-op fault before any device access. */
                    LBL_out_regVal_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte dst_size = op2_imm_size();
                        copy_memval_reg(regs::rp.w0, dst_size, operand2, subreg_enum::w0, access_kind::fetch);
                        port_write(operand2.q0(), op1_reg(), op1_subreg_flag());
                        regs::rp.w0 += dst_size;
                        MAIZE_NEXT();
                    }

                    LBL_out_immVal_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, operand1, subreg_enum::w0, access_kind::fetch);
                        regs::rp.w0 += src_size;
                        u_byte dst_size = op2_imm_size();
                        copy_memval_reg(regs::rp.w0, dst_size, operand2, subreg_enum::w0, access_kind::fetch);
                        port_write(operand2.q0(), operand1, subreg_enum::w0);
                        regs::rp.w0 += dst_size;
                        MAIZE_NEXT();
                    }

                    /* $94 out_regAddr_imm (card maize-21): write operand1 (the value
                       loaded from the source address at line above) to the device, NOT
                       op1_reg() (the raw source address). This corrects the dead-load
                       defect isolated to this form; the sibling address forms already
                       write operand1. */
                    LBL_out_regAddr_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), operand1, subreg_enum::w0);
                        u_byte dst_size = op2_imm_size();
                        copy_memval_reg(regs::rp.w0, dst_size, operand2, subreg_enum::w0, access_kind::fetch);
                        port_write(operand2.q0(), operand1, subreg_enum::w0);
                        regs::rp.w0 += dst_size;
                        MAIZE_NEXT();
                    }

                    LBL_out_immAddr_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, src_size, operand1, subreg_enum::w0);
                        regs::rp.w0 += src_size;
                        u_byte dst_size = op2_imm_size();
                        copy_memval_reg(regs::rp.w0, dst_size, operand2, subreg_enum::w0, access_kind::fetch);
                        port_write(operand2.q0(), operand1, subreg_enum::w0);
                        regs::rp.w0 += dst_size;
                        MAIZE_NEXT();
                    }

                    /* OUTR/IN (card maize-10, Decision D6464): mirror OUT's four-case pattern
                       against the same port table, but the port is a register operand (op2
                       for OUTR, op1 for IN) rather than an immediate literal. The port id is
                       always the register's/temp's .q0() field, matching OUT's own
                       immediate-port convention of using only the low 16 bits regardless of
                       the encoded field width. IN's copy direction is device-to-register,
                       the mirror image of OUT's register-to-device direction. */
                    LBL_outr_regVal_reg: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        port_write(op2_reg().q0(), op1_reg(), op1_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_outr_immVal_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, operand1, subreg_enum::w0, access_kind::fetch);
                        regs::rp.w0 += src_size;
                        port_write(op2_reg().q0(), operand1, subreg_enum::w0);
                        MAIZE_NEXT();
                    }

                    LBL_outr_regAddr_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), operand1, subreg_enum::w0);
                        port_write(op2_reg().q0(), operand1, subreg_enum::w0);
                        MAIZE_NEXT();
                    }

                    LBL_outr_immAddr_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, src_size, operand1, subreg_enum::w0);
                        regs::rp.w0 += src_size;
                        port_write(op2_reg().q0(), operand1, subreg_enum::w0);
                        MAIZE_NEXT();
                    }

                    LBL_in_regVal_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        port_read(op1_reg().q0(), op2_reg(), op2_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_in_immVal_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, operand1, subreg_enum::w0, access_kind::fetch);
                        regs::rp.w0 += src_size;
                        port_read(operand1.q0(), op2_reg(), op2_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_in_regAddr_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), operand1, subreg_enum::w0);
                        port_read(operand1.q0(), op2_reg(), op2_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_in_immAddr_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, src_size, operand1, subreg_enum::w0);
                        regs::rp.w0 += src_size;
                        port_read(operand1.q0(), op2_reg(), op2_subreg_flag());
                        MAIZE_NEXT();
                    }

                    /* SYS provider select (card maize-24, D9 Shape B). With
                       syscall_guest_flag CLEAR (boot default) SYS calls the native
                       sys::call provider exactly as v1.0, byte-identical. With the flag
                       SET, SYS instead traps through the shared trap table at cause 7
                       (cause_syscall) into the guest-installed handler: the syscall
                       number rides the frame's aux word, args stay in R0/R1/R2, and the
                       result is returned in RV by the guest (per toolchain/rt/SYSCALL-ABI.md).
                       regs::rp.w0 is fully advanced past the SYS instruction here, so the
                       saved PC is the correct IRET-resume address. deliver_vectored halts
                       (halt_no_interrupt_handler) if no cause-7 handler is installed, the
                       same uniform no-handler rule as every other vector. */
                    LBL_sys_immVal: {
                        regs::rp.w0 += 1;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, operand1, subreg_enum::w0, access_kind::fetch);
                        regs::rp.w0 += src_size;
                        if (syscall_guest_flag) {
                            deliver_vectored(trap::cause_syscall, 0, operand1.b0(), regs::rp.w0);
                        } else {
                            regs::rv.w0 = sys::call(operand1.b0());
                        }
                        MAIZE_NEXT();
                    }

                    LBL_sys_regVal: {
                        regs::rp.w0 += 1;
                        if (syscall_guest_flag) {
                            deliver_vectored(trap::cause_syscall, 0, op1_reg().b0(), regs::rp.w0);
                        } else {
                            regs::rv.w0 = sys::call(op1_reg().b0());
                        }
                        MAIZE_NEXT();
                    }

                    LBL_pop_opcode: {
                        regs::rp.w0 += 1;
                        auto src_size = op1_subreg_size();
                        copy_memval_reg(regs::rs.w0, src_size, op1_reg(), op1_subreg_flag(), access_kind::load);
                        regs::rs.w0 += src_size;
                        MAIZE_NEXT();
                    }

                    LBL_push_regVal: {
                        regs::rp.w0 += 1;
                        u_byte src_size = op1_subreg_size();
                        /* Fault-atomic push (maize-194 review #2605, major 1): stage the
                           decremented stack pointer in a temp and let copy_regval_regaddr
                           translate+write it. A page fault on the translated stack write
                           (the stack-auto-grow demand-paging trigger) leaves regs::rs
                           unmutated, so an IRET-and-re-execute of this PUSH decrements rs
                           exactly once. rs is committed ONLY after the write succeeds. */
                        reg new_rs;
                        new_rs.w0 = regs::rs.w0 - src_size;
                        copy_regval_regaddr(op1_reg(), op1_subreg_flag(), new_rs, subreg_enum::w0);
                        regs::rs.w0 = new_rs.w0;
                        MAIZE_NEXT();
                    }

                    LBL_push_immVal: {
                        regs::rp.w0 += 1;
                        u_byte src_size = op1_imm_size();
                        /* Fault-atomic push (maize-194 review #2605, major 1): see
                           LBL_push_regVal. copy_memval_regaddr reads the immediate (fetch)
                           then translates the stack slot (store); a fault in either leaves
                           regs::rs and regs::rp unmutated for a clean re-execute. */
                        reg new_rs;
                        new_rs.w0 = regs::rs.w0 - src_size;
                        copy_memval_regaddr(regs::rp.w0, src_size, new_rs, subreg_enum::w0);
                        regs::rs.w0 = new_rs.w0;
                        regs::rp.w0 += src_size;
                        MAIZE_NEXT();
                    }

                    LBL_call_regVal: {
                        /* Push the full 64-bit return address, then jump (maize-41).
                           Fault-atomic (maize-194 review #2605, major 1): stage the
                           decremented stack pointer in a temp so a page fault on the
                           translated return-address write leaves regs::rs unmutated; commit
                           rs only after the write succeeds. */
                        regs::rp.w0 += 1;                                          // past the register param -> return address
                        reg new_rs;
                        new_rs.w0 = regs::rs.w0 - subreg_size_map[static_cast<size_t>(subreg_enum::w0)];  // 8-byte return slot
                        copy_regval_regaddr(regs::rp, subreg_enum::w0, new_rs, subreg_enum::w0);
                        regs::rs.w0 = new_rs.w0;
                        copy_regval_reg_zext(op1_reg(), op1_subreg_flag(), regs::rp, subreg_enum::w0);
                        MAIZE_NEXT();
                    }

                    LBL_call_immVal: {
                        /* Read the target immediate at its encoded width (zero-extended), push the
                           full 64-bit return address, then jump (maize-41). Fault-atomic
                           (maize-194 review #2605, major 1): the target fetch precedes any rs
                           mutation, and rs is committed only after the return-address write. */
                        regs::rp.w0 += 1;                                          // past the param byte
                        u_byte src_size = op1_imm_size();
                        reg target;
                        mm.read(translate(regs::rp.w0, access_kind::fetch), target, src_size, 0);
                        regs::rp.w0 += src_size;                                   // PC now at the return address
                        reg new_rs;
                        new_rs.w0 = regs::rs.w0 - subreg_size_map[static_cast<size_t>(subreg_enum::w0)];  // 8-byte return slot
                        copy_regval_regaddr(regs::rp, subreg_enum::w0, new_rs, subreg_enum::w0);
                        regs::rs.w0 = new_rs.w0;
                        regs::rp.w0 = target.w0;
                        MAIZE_NEXT();
                    }

                    /* CALL indirect (card maize-10, Decision D6463): combines the
                       return-address-push sequence above with the target-resolution logic
                       already in jmp_regAddr/jmp_immAddr (below). */
                    LBL_call_regAddr: {
                        /* Fault-atomic (maize-194 review #2605, major 1): resolve the
                           indirect target into a temp FIRST (a fault on the target
                           dereference leaves rs unmutated), then stage the decremented stack
                           pointer, push the return address, and commit rs only on success.
                           Both faultable operations precede any rs commit, so an
                           IRET-and-re-execute decrements rs exactly once. */
                        regs::rp.w0 += 1;                                          // past the register param -> return address
                        reg target;
                        target.w0 = 0;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), target, subreg_enum::w0);  // deref target (load)
                        reg new_rs;
                        new_rs.w0 = regs::rs.w0 - subreg_size_map[static_cast<size_t>(subreg_enum::w0)];  // 8-byte return slot
                        copy_regval_regaddr(regs::rp, subreg_enum::w0, new_rs, subreg_enum::w0);
                        regs::rs.w0 = new_rs.w0;
                        regs::rp.w0 = target.w0;
                        MAIZE_NEXT();
                    }

                    LBL_call_immAddr: {
                        /* Read the address-literal at its encoded width, advance PC past it
                           (that's the return address), then double-dereference to the actual
                           jump target. Fault-atomic (maize-194 review #2605, major 1): both
                           the literal fetch and the target dereference precede any rs
                           mutation, and rs is committed only after the return-address write,
                           so a fault anywhere re-executes cleanly with rs decremented once. */
                        regs::rp.w0 += 1;                                          // past the param byte
                        u_byte src_size = op1_imm_size();
                        reg addr_literal;
                        mm.read(translate(regs::rp.w0, access_kind::fetch), addr_literal, src_size, 0);
                        regs::rp.w0 += src_size;                                   // PC now at the return address
                        reg target;
                        mm.read(translate(addr_literal.w0, access_kind::load), target, subreg_size_map[static_cast<size_t>(subreg_enum::w0)], 0);
                        reg new_rs;
                        new_rs.w0 = regs::rs.w0 - subreg_size_map[static_cast<size_t>(subreg_enum::w0)];  // 8-byte return slot
                        copy_regval_regaddr(regs::rp, subreg_enum::w0, new_rs, subreg_enum::w0);
                        regs::rs.w0 = new_rs.w0;
                        regs::rp.w0 = target.w0;
                        MAIZE_NEXT();
                    }

                    LBL_ret_opcode: {
                        /* Pop the full 64-bit return address (maize-41). */
                        u_byte src_size = subreg_size_map[static_cast<size_t>(subreg_enum::w0)];
                        copy_memval_reg(regs::rs.w0, src_size, regs::rp, subreg_enum::w0, access_kind::load);
                        regs::rs.w0 += src_size;
                        MAIZE_NEXT();
                    }

                    LBL_iret_opcode: {
                        /* IRET is privileged (card maize-180, §9). This closes the forged-RF
                           escalation: an unguarded IRET pops the full RF word (privilege bit
                           included) then RP off the user-writable RS, so a user process could
                           forge an RF word with the privilege bit set and IRET straight into
                           supervisor mode. Gating it (cause-4 in user mode) shuts that door;
                           every trap/interrupt handler runs supervisor (deliver_vectored
                           forces privilege_flag true on entry), so a handler's own IRET always
                           executes from supervisor and correctly drops to user by restoring a
                           saved RF whose privilege bit is clear. */
                        if (!privilege_flag) { raise_privileged_op(); }
                        auto src_size = subreg_size_map[static_cast<size_t>(subreg_enum::w0)];
                        /* Restart-safe two-word pop (maize-194 review #2605, major 1): read
                           BOTH saved words into temps at the IRET's own supervisor privilege
                           BEFORE restoring RF/RP/RS. This (a) keeps both stack reads
                           supervisor accesses even when the saved RF drops to user (restoring
                           a user RF must not retroactively re-classify the RP read as a user
                           access), and (b) leaves rf/rp/rs unmutated if either read page-faults
                           (the two slots can straddle a page boundary), so an IRET-and-
                           re-execute pops exactly once. Single-pop POP/RET are inherently
                           restart-safe (read then increment) and are left unchanged. */
                        reg saved_rf;
                        reg saved_rp;
                        saved_rf.w0 = 0;
                        saved_rp.w0 = 0;
                        copy_memval_reg(regs::rs.w0, src_size, saved_rf, subreg_enum::w0, access_kind::load);
                        copy_memval_reg(regs::rs.w0 + src_size, src_size, saved_rp, subreg_enum::w0, access_kind::load);
                        regs::rf.w0 = saved_rf.w0;
                        /* card maize-197: IRET restores the full RF word from the trap frame,
                           overwriting Z/N/C/V wholesale. Discard any pending descriptor so a
                           later materialize cannot clobber the restored flags. */
                        pending_flags.dirty = false;
                        regs::rp.w0 = saved_rp.w0;
                        regs::rs.w0 += src_size + src_size;
                        MAIZE_NEXT();
                    }

                    /* JMP (card maize-64): always targets the full 64-bit width. The
                       register forms read the whole register (subreg_enum::w0) regardless
                       of any encoded sub-register selection, folding in the full-width role
                       LNGJMP used to play; LNGJMP is removed. */
                    LBL_jmp_regVal: {
                        regs::rp.w0 += 1;
                        copy_regval_reg_zext(op1_reg(), subreg_enum::w0, regs::rp, subreg_enum::w0);
                        MAIZE_NEXT();
                    }

                    LBL_jmp_immVal: {
                        regs::rp.w0 += 1;
                        jump_to_immediate();
                        MAIZE_NEXT();
                    }

                    LBL_jmp_regAddr: {
                        regs::rp.w0 += 1;
                        copy_regaddr_reg(op1_reg(), subreg_enum::w0, regs::rp, subreg_enum::w0);
                        MAIZE_NEXT();
                    }

                    LBL_jmp_immAddr: {
                        regs::rp.w0 += 1;
                        copy_memaddr_reg(regs::rp.w0, op1_imm_size(), regs::rp, subreg_enum::w0);
                        MAIZE_NEXT();
                    }

                    /* Conditional branches (Jcc), card maize-64: IMMEDIATE target only. The
                       ten conditions share three base slots ($17/$18/$19); the condition is
                       decoded from the opcode's row/column bits and evaluated by the shared
                       eval_condition table (the SAME predicate table SETcc uses). On a taken
                       branch the immediate target is loaded into PC; otherwise PC steps over
                       the immediate. */
                    LBL_jz_opcode:
                    LBL_jnz_opcode:
                    LBL_jlt_opcode:
                    LBL_jb_opcode:
                    LBL_jgt_opcode:
                    LBL_ja_opcode:
                    LBL_jge_opcode:
                    LBL_jle_opcode:
                    LBL_jbe_opcode:
                    LBL_jae_opcode:
                    LBL_jp_opcode: {
                        regs::rp.w0 += 1;   // past the operand-descriptor (immediate-size) byte
                        if (eval_condition(decode_condition(instr::jcc_base))) {
                            jump_to_immediate();
                        }
                        else {
                            regs::rp.w0 += op1_imm_size();
                        }
                        MAIZE_NEXT();
                    }

                    /* No-operand carry manipulation (card maize-1). card maize-197: these
                       touch only the carry bit, leaving N/V/Z as they were. Resolve any
                       pending descriptor first so those bits are concrete before the narrow
                       carry write, and so a later materialize cannot overwrite this carry. */
                    LBL_setcry_opcode: {
                        materialize_flags();
                        carryout_flag = true;
                        MAIZE_NEXT();
                    }

                    LBL_clrcry_opcode: {
                        materialize_flags();
                        carryout_flag = false;
                        MAIZE_NEXT();
                    }

                    /* No-operand interrupt-enable manipulation (card maize-10, Decision D6461):
                       pure set/clear of interrupt_enabled_flag, mirroring setcry_opcode/
                       clrcry_opcode above exactly. No interrupt-vector-table or delivery work
                       included (Open Question O1). */
                    LBL_setint_opcode: {
                        if (!privilege_flag) { raise_privileged_op(); }   // privileged (card maize-180, §9)
                        interrupt_enabled_flag = true;
                        MAIZE_NEXT();
                    }

                    LBL_clrint_opcode: {
                        if (!privilege_flag) { raise_privileged_op(); }   // privileged (card maize-180, §9)
                        interrupt_enabled_flag = false;
                        MAIZE_NEXT();
                    }

                    /* Syscall-provider mode select (card maize-24, D9 Shape B): pure
                       set/clear of syscall_guest_flag, mirroring setint/clrint above
                       exactly. SET routes SYS through cause 7 to the guest handler;
                       CLEAR restores the native sys::call provider. Privileged
                       (card maize-180, §9): only supervisor code selects the provider. */
                    LBL_setsysg_opcode: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        syscall_guest_flag = true;
                        MAIZE_NEXT();
                    }

                    LBL_clrsysg_opcode: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        syscall_guest_flag = false;
                        MAIZE_NEXT();
                    }

                    /* Control-register + TLB control (card maize-180). All five forms are
                       privileged: the head-of-dispatch guard raises the cause-4
                       privileged-operation fault in user mode, exactly the IN / OUT / OUTR
                       precedent above. MOVTCR / MOVFCR mirror the OUT / IN operand shapes
                       with cr_write / cr_read in place of port_write / port_read. TLBINV /
                       TLBINVA are no-ops here (no software TLB exists yet); maize-194 gives
                       them bodies. */
                    LBL_movtcr_regVal_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte dst_size = op2_imm_size();
                        copy_memval_reg(regs::rp.w0, dst_size, operand2, subreg_enum::w0, access_kind::fetch);
                        cr_write(operand2.q0(), op1_reg(), op1_subreg_flag());
                        regs::rp.w0 += dst_size;
                        MAIZE_NEXT();
                    }

                    LBL_movtcr_immVal_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, operand1, subreg_enum::w0, access_kind::fetch);
                        regs::rp.w0 += src_size;
                        u_byte dst_size = op2_imm_size();
                        copy_memval_reg(regs::rp.w0, dst_size, operand2, subreg_enum::w0, access_kind::fetch);
                        cr_write(operand2.q0(), operand1, subreg_enum::w0);
                        regs::rp.w0 += dst_size;
                        MAIZE_NEXT();
                    }

                    LBL_movfcr_immVal_reg: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, operand1, subreg_enum::w0, access_kind::fetch);
                        regs::rp.w0 += src_size;
                        cr_read(operand1.q0(), op2_reg(), op2_subreg_flag());
                        MAIZE_NEXT();
                    }

                    LBL_tlbinv_opcode: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        /* Invalidate the entire software TLB (card maize-194). */
                        tlb_flush_all();
                        MAIZE_NEXT();
                    }

                    LBL_tlbinva_opcode: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 1;   // consume the register operand byte
                        /* Invalidate the single software-TLB entry whose tag is
                           VA(op1_reg()) >> 12 (card maize-194). */
                        tlb_flush_va(op1_reg().w0);
                        MAIZE_NEXT();
                    }

                    LBL_nop_opcode: {
                        /* Do nothing. */
                        MAIZE_NEXT();
                    }

                    /* BRK (card maize-78, Open Question O7, superseding maize-10 Decision
                       D6460): a defined breakpoint trap, NOT a no-op. tick() has already
                       advanced regs::rp.w0 past this single-byte opcode, so the captured
                       following-instruction PC (trap class) is already in place. With no
                       handler installed (the maize-21 vector table does not exist yet) the
                       trap halts the VM deterministically with the breakpoint cause
                       surfaced, through the same mechanism raise_divide_error uses. */
                    LBL_brk_opcode: {
                        raise_breakpoint();
                    }

                    /* INT ($24/$64) intentionally has no dispatch case here (card maize-10):
                       it needs a defined interrupt-vector-table format (location, entry width,
                       index bounds-checking) before it can be safely dispatched; closing it
                       without that design would just move the crash from "unknown opcode" to
                       undefined behavior at an arbitrary handler address (Open Question O1). It
                       falls through to the default "unknown opcode" case below until then.

                       DUP/SWAP were header-only encoding ghosts and are removed entirely
                       (card maize-64). */

                    /* ===== Floating-point ISA (card maize-122) =================
                       Zfinx: operands live in the integer register file; the
                       operation width (binary32 vs binary64) comes from the
                       destination subregister (H0/H1 => 4, W0 => 8). A B* or Q*
                       subregister on an FP operand is an illegal-operand trap. */

                    /* 3a. Arithmetic, register-value source. */
                    LBL_fadd_regVal_reg:
                    LBL_fsub_regVal_reg:
                    LBL_fmul_regVal_reg:
                    LBL_fdiv_regVal_reg: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FP destination subregister must be H0/H1 (binary32) or W0 (binary64)");
                        u_byte sw = fp_width_from_subreg(op1_subreg_flag());
                        if (!sw) raise_illegal_fp("FP source subregister must be H0/H1 or W0");
                        if (sw != w) raise_illegal_fp("FP operands must be the same width (binary32 with binary32, binary64 with binary64)");
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.set_b0(regs::ri.b0());
                        alu.set_b2(w);
                        run_fpu_arith();
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        MAIZE_NEXT();
                    }

                    /* 3a. Arithmetic, immediate-value source (raw float bits). */
                    LBL_fadd_immVal_reg:
                    LBL_fsub_immVal_reg:
                    LBL_fmul_immVal_reg:
                    LBL_fdiv_immVal_reg: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FP destination subregister must be H0/H1 (binary32) or W0 (binary64)");
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0, access_kind::fetch);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.set_b0(regs::ri.b0());
                        alu.set_b2(w);
                        run_fpu_arith();
                        regs::rp.w0 += src_size;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        MAIZE_NEXT();
                    }

                    /* 3a. Arithmetic, register-address source (load then operate). */
                    LBL_fadd_regAddr_reg:
                    LBL_fsub_regAddr_reg:
                    LBL_fmul_regAddr_reg:
                    LBL_fdiv_regAddr_reg: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FP destination subregister must be H0/H1 (binary32) or W0 (binary64)");
                        {
                            u_word addr = read_subreg_bits(op1_reg(), op1_subreg_flag());
                            reg tmp; tmp.w0 = 0;
                            mm.read(translate(addr, access_kind::load), tmp, w, 0);
                            alu.op1_reg.w0 = tmp.w0;
                        }
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.set_b0(regs::ri.b0());
                        alu.set_b2(w);
                        run_fpu_arith();
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        MAIZE_NEXT();
                    }

                    /* 3a. Arithmetic, immediate-address source. */
                    LBL_fadd_immAddr_reg:
                    LBL_fsub_immAddr_reg:
                    LBL_fmul_immAddr_reg:
                    LBL_fdiv_immAddr_reg: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FP destination subregister must be H0/H1 (binary32) or W0 (binary64)");
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.set_b0(regs::ri.b0());
                        alu.set_b2(w);
                        run_fpu_arith();
                        regs::rp.w0 += src_size;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        MAIZE_NEXT();
                    }

                    /* 3e. FCMP, all four addressing-mode source forms. op2 is `a`
                       (the register compared), op1/immediate is `src`. */
                    LBL_fcmp_regVal_reg: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FCMP operand subregister must be H0/H1 or W0");
                        u_byte sw = fp_width_from_subreg(op1_subreg_flag());
                        if (!sw) raise_illegal_fp("FCMP operand subregister must be H0/H1 or W0");
                        if (sw != w) raise_illegal_fp("FCMP operands must be the same width (binary32 with binary32, binary64 with binary64)");
                        u_word src = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        u_word a = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        do_fcmp(a, src, w);
                        MAIZE_NEXT();
                    }

                    LBL_fcmp_immVal_reg: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FCMP operand subregister must be H0/H1 or W0");
                        u_byte src_size = op1_imm_size();
                        reg tmp; tmp.w0 = 0;
                        mm.read(translate(regs::rp.w0, access_kind::fetch), tmp, src_size, 0);
                        u_word a = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        do_fcmp(a, tmp.w0, w);
                        regs::rp.w0 += src_size;
                        MAIZE_NEXT();
                    }

                    LBL_fcmp_regAddr_reg: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FCMP operand subregister must be H0/H1 or W0");
                        u_word addr = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        reg tmp; tmp.w0 = 0;
                        mm.read(translate(addr, access_kind::load), tmp, w, 0);
                        u_word a = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        do_fcmp(a, tmp.w0, w);
                        MAIZE_NEXT();
                    }

                    LBL_fcmp_immAddr_reg: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FCMP operand subregister must be H0/H1 or W0");
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0);
                        u_word a = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        do_fcmp(a, alu.op1_reg.w0, w);
                        regs::rp.w0 += src_size;
                        MAIZE_NEXT();
                    }

                    /* 3b. Unary register-only: FSQRT/FNEG/FABS. op1 = src, op2 = dst
                       (dst = f(src)). FNEG/FABS are exact sign-bit ops (no flags, no
                       rounding, NaN payloads preserved). */
                    LBL_fsqrt_opcode:
                    LBL_fneg_opcode:
                    LBL_fabs_opcode: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FP destination subregister must be H0/H1 or W0");
                        u_byte sw = fp_width_from_subreg(op1_subreg_flag());
                        if (!sw) raise_illegal_fp("FP source subregister must be H0/H1 or W0");
                        if (sw != w) raise_illegal_fp("FP operands must be the same width (binary32 with binary32, binary64 with binary64)");
                        u_word src = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        fpu::fresult res;
                        switch (regs::ri.b0()) {
                            case instr::fsqrt_opcode: res = fpu::fp_sqrt(src, w, fp_checked_frm()); break;
                            case instr::fneg_opcode:  res = fpu::fp_neg(src, w); break;
                            default:                  res = fpu::fp_abs(src, w); break;
                        }
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op2_reg(), op2_subreg_flag(), res.bits);
                        MAIZE_NEXT();
                    }

                    /* 3d. Min/max register-only: FMIN/FMAX. op1 = src, op2 = dst
                       (dst = min/max(dst, src)). */
                    LBL_fmin_opcode:
                    LBL_fmax_opcode: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FP destination subregister must be H0/H1 or W0");
                        u_byte sw = fp_width_from_subreg(op1_subreg_flag());
                        if (!sw) raise_illegal_fp("FP source subregister must be H0/H1 or W0");
                        if (sw != w) raise_illegal_fp("FP operands must be the same width (binary32 with binary32, binary64 with binary64)");
                        u_word src = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        u_word dst = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        fpu::fresult res = (regs::ri.b0() == instr::fmin_opcode)
                            ? fpu::fp_min(dst, src, w) : fpu::fp_max(dst, src, w);
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op2_reg(), op2_subreg_flag(), res.bits);
                        MAIZE_NEXT();
                    }

                    /* 3f. Conversions register-only. op1 = src, op2 = dst. Widths
                       come from the two subregister fields; the float operand must be
                       H0/H1/W0, the integer operand may be any width. */
                    LBL_fcvtff_opcode: { // float <-> float
                        regs::rp.w0 += 2;
                        u_byte dw = fp_width_from_subreg(op2_subreg_flag());
                        u_byte sw = fp_width_from_subreg(op1_subreg_flag());
                        if (!dw || !sw) raise_illegal_fp("FCVTFF operand subregister must be H0/H1 or W0");
                        u_word src = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        fpu::fresult res = fpu::fp_cvt_ff(src, sw, dw, fp_checked_frm());
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op2_reg(), op2_subreg_flag(), res.bits);
                        MAIZE_NEXT();
                    }

                    LBL_fcvtfs_opcode:   // float -> signed integer
                    LBL_fcvtfu_opcode: { // float -> unsigned integer
                        regs::rp.w0 += 2;
                        u_byte sw = fp_width_from_subreg(op1_subreg_flag());
                        if (!sw) raise_illegal_fp("FCVTFS/FCVTFU source subregister must be H0/H1 or W0");
                        u_byte dw = op2_subreg_size(); // integer dst: any width
                        u_word src = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        bool is_signed = (regs::ri.b0() == instr::fcvtfs_opcode);
                        fpu::fresult res = fpu::fp_cvt_f_to_int(src, sw, dw, is_signed, fp_checked_frm());
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op2_reg(), op2_subreg_flag(), res.bits);
                        MAIZE_NEXT();
                    }

                    LBL_fcvtsf_opcode:   // signed integer -> float
                    LBL_fcvtuf_opcode: { // unsigned integer -> float
                        regs::rp.w0 += 2;
                        u_byte dw = fp_width_from_subreg(op2_subreg_flag());
                        if (!dw) raise_illegal_fp("FCVTSF/FCVTUF destination subregister must be H0/H1 or W0");
                        u_byte sw = op1_subreg_size(); // integer src: any width
                        u_word src = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        bool is_signed = (regs::ri.b0() == instr::fcvtsf_opcode);
                        fpu::fresult res = fpu::fp_cvt_int_to_f(src, sw, dw, is_signed, fp_checked_frm());
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op2_reg(), op2_subreg_flag(), res.bits);
                        MAIZE_NEXT();
                    }

                    /* 3c. FMA: op1 = a (flagged src), op2 = b, op3 = c (also the
                       destination accumulator): c = a*b (+/-) c, single-rounded. The
                       spec's 4-name FMADD dst,a,b,c collapses to a 3-operand multiply-
                       accumulate under the MULW-shaped encoding (op3 is both the
                       addend c and the destination dst). FNMADD/FNMSUB are synthesized
                       via the exact FNEG (not primitives). */
                    LBL_fmadd_regVal_regreg:
                    LBL_fmsub_regVal_regreg: {
                        regs::rp.w0 += 3;
                        u_byte w = fp_width_from_subreg(op3_subreg_flag());
                        if (!w) raise_illegal_fp("FMA destination subregister must be H0/H1 or W0");
                        u_byte aw = fp_width_from_subreg(op1_subreg_flag());
                        u_byte bw = fp_width_from_subreg(op2_subreg_flag());
                        if (!aw || !bw) raise_illegal_fp("FMA multiplicand subregister must be H0/H1 or W0");
                        if (aw != w || bw != w) raise_illegal_fp("FMA operands must be the same width (binary32 with binary32, binary64 with binary64)");
                        u_word a = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        u_word b = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        u_word c = read_subreg_bits(op3_reg(), op3_subreg_flag());
                        bool sub = ((regs::ri.b0() & arithmetic_logic_unit::opflag_code) == instr::fmsub_opcode);
                        fpu::fresult res = fpu::fp_fma(a, b, c, w, fp_checked_frm(), sub);
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op3_reg(), op3_subreg_flag(), res.bits);
                        MAIZE_NEXT();
                    }

                    LBL_fmadd_immVal_regreg:
                    LBL_fmsub_immVal_regreg: {
                        regs::rp.w0 += 3;
                        u_byte w = fp_width_from_subreg(op3_subreg_flag());
                        if (!w) raise_illegal_fp("FMA destination subregister must be H0/H1 or W0");
                        {
                            u_byte bw = fp_width_from_subreg(op2_subreg_flag());
                            if (!bw) raise_illegal_fp("FMA multiplicand subregister must be H0/H1 or W0");
                            if (bw != w) raise_illegal_fp("FMA operands must be the same width (binary32 with binary32, binary64 with binary64)");
                        }
                        u_byte src_size = op1_imm_size();
                        reg tmp; tmp.w0 = 0;
                        mm.read(translate(regs::rp.w0, access_kind::fetch), tmp, src_size, 0);
                        u_word a = tmp.w0;
                        u_word b = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        u_word c = read_subreg_bits(op3_reg(), op3_subreg_flag());
                        bool sub = ((regs::ri.b0() & arithmetic_logic_unit::opflag_code) == instr::fmsub_opcode);
                        fpu::fresult res = fpu::fp_fma(a, b, c, w, fp_checked_frm(), sub);
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op3_reg(), op3_subreg_flag(), res.bits);
                        regs::rp.w0 += src_size;
                        MAIZE_NEXT();
                    }

                    LBL_fmadd_regAddr_regreg:
                    LBL_fmsub_regAddr_regreg: {
                        regs::rp.w0 += 3;
                        u_byte w = fp_width_from_subreg(op3_subreg_flag());
                        if (!w) raise_illegal_fp("FMA destination subregister must be H0/H1 or W0");
                        {
                            u_byte bw = fp_width_from_subreg(op2_subreg_flag());
                            if (!bw) raise_illegal_fp("FMA multiplicand subregister must be H0/H1 or W0");
                            if (bw != w) raise_illegal_fp("FMA operands must be the same width (binary32 with binary32, binary64 with binary64)");
                        }
                        u_word addr = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        reg tmp; tmp.w0 = 0;
                        mm.read(translate(addr, access_kind::load), tmp, w, 0);
                        u_word a = tmp.w0;
                        u_word b = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        u_word c = read_subreg_bits(op3_reg(), op3_subreg_flag());
                        bool sub = ((regs::ri.b0() & arithmetic_logic_unit::opflag_code) == instr::fmsub_opcode);
                        fpu::fresult res = fpu::fp_fma(a, b, c, w, fp_checked_frm(), sub);
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op3_reg(), op3_subreg_flag(), res.bits);
                        MAIZE_NEXT();
                    }

                    LBL_fmadd_immAddr_regreg:
                    LBL_fmsub_immAddr_regreg: {
                        regs::rp.w0 += 3;
                        u_byte w = fp_width_from_subreg(op3_subreg_flag());
                        if (!w) raise_illegal_fp("FMA destination subregister must be H0/H1 or W0");
                        {
                            u_byte bw = fp_width_from_subreg(op2_subreg_flag());
                            if (!bw) raise_illegal_fp("FMA multiplicand subregister must be H0/H1 or W0");
                            if (bw != w) raise_illegal_fp("FMA operands must be the same width (binary32 with binary32, binary64 with binary64)");
                        }
                        u_byte src_size = op1_imm_size();
                        /* FMADD/FMSUB immAddr reads the FMA `a` operand from its address
                           literal; the dereference is translated inside copy_memaddr_reg
                           (choke point #2). This is the FMA immAddr form, not FCMP. */
                        copy_memaddr_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0);
                        u_word a = alu.op1_reg.w0;
                        u_word b = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        u_word c = read_subreg_bits(op3_reg(), op3_subreg_flag());
                        bool sub = ((regs::ri.b0() & arithmetic_logic_unit::opflag_code) == instr::fmsub_opcode);
                        fpu::fresult res = fpu::fp_fma(a, b, c, w, fp_checked_frm(), sub);
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op3_reg(), op3_subreg_flag(), res.bits);
                        regs::rp.w0 += src_size;
                        MAIZE_NEXT();
                    }

                    /* FCSR access (card maize-122). FGETCSR dst: dst = FCSR (the
                       whole 8-bit FRM+FFLAGS byte). FSETCSR src: FCSR = src (low 8
                       bits; the upper reserved trap-enable region stays 0 in v1.0). */
                    LBL_fgetcsr_opcode: {
                        regs::rp.w0 += 1;
                        write_subreg_bits(op1_reg(), op1_subreg_flag(),
                            static_cast<u_word>(static_cast<u_byte>(regs::fcsr.b0())));
                        MAIZE_NEXT();
                    }

                    LBL_fsetcsr_opcode: {
                        regs::rp.w0 += 1;
                        u_word v = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        regs::fcsr.w0 = v & 0xFF;
                        MAIZE_NEXT();
                    }

                    LBL_default: {
                        /* Unknown opcode. The message-building (a non-trivial std::stringstream)
                           is in an out-of-line [[noreturn]] helper: clang forbids an indirect
                           goto (the threaded dispatch) from crossing a non-trivial variable's
                           scope, so no such variable may live in the dispatch body. */
                        raise_unknown_opcode(regs::ri.b0());
                    }
            tick_exit: ;
#undef MAIZE_NEXT
        }

        /* --show-perf: enable the per-instruction counter (off by default so normal runs are
           unperturbed) and read the running guest-instruction count for the MIPS readout. */
        void enable_perf_counter() {
            perf_count_enabled = true;
        }

        u_word instruction_count() {
            return perf_insn_count;
        }

        /* Stop the VM: drop out of tick()'s instruction loop (running_flag) and
           out of run()'s power loop (is_power_on) so run() returns rather than
           blocking on int_event.wait(). Shared by the HALT handler and by
           SYS $3C (sys_exit); both file-local flags live in cpu-internal
           anonymous namespaces, so this is the single exported stop primitive. */
        void power_off() {
            /* Notify int_event under int_mutex: a core parked in HALT (run()'s wait-for-
               interrupt loop) is released only by a notify, and once the display loop has
               stopped there are no further vsync raises to wake it, so without this notify
               the window-close path (power_off then guest.join()) would hang. */
            std::lock_guard<std::mutex> lk(int_mutex);
            running_flag = false;
            is_power_on = false;
            int_event.notify_all();
        }

        void run() {
            {
                std::lock_guard<std::mutex> lk(int_mutex);
                is_power_on = true;
            }

            privilege_flag = true;
            int_event.notify_all();

            while (is_power_on) {
                try {
                    tick();
                }
                catch (const page_fault_redirect&) {
                    /* A page fault aborted the faulting instruction mid-flight;
                       deliver_vectored has already installed the cause-8 frame and pointed
                       RP at the guest handler. Re-enter tick() to run it (card maize-194). */
                    continue;
                }

                {
                    std::unique_lock<std::mutex> lk(int_mutex);

                    if (is_power_on) {
                        /* Wait-for-interrupt park: a core that dropped out of tick() with
                           power still on (running_flag clear, is_power_on set) waits here
                           until a DELIVERABLE interrupt arrives (a pending IRQ with
                           interrupts enabled) or power is cut. Waiting on the deliverable
                           condition (not merely "pending") means a masked pending IRQ
                           keeps the core parked rather than spinning or resuming execution
                           from the current PC. On a deliverable wake it acknowledges,
                           builds the shared four-word aux / cause / RF / PC frame, and
                           re-enters tick() so the handler runs.

                           HALT reaches this park: with interrupts enabled it clears
                           running_flag but leaves is_power_on set (a wait-for-interrupt
                           halt), so tick() returns here and the core sleeps at ~0 MIPS
                           until a source raises a deliverable IRQ (keyboard from the SDL
                           thread, vsync from the display thread). With interrupts disabled
                           HALT instead calls power_off() (a permanent halt with no possible
                           wake), so HALT-to-end programs still exit. */
                        while (is_power_on && !(irq_pending && interrupt_enabled_flag)) {
                            int_event.wait(lk);
                        }

                        if (is_power_on) {
                            u_byte vector = irq_pending_vector;
                            irq_pending = false;
                            interrupt_set_flag = false;
                            lk.unlock();
                            /* running_flag is a bit in RF (bit_running). HALT parked by clearing
                               it, so RF currently has running=false. deliver_vectored saves RF
                               into the interrupt frame; the handler's IRET restores it. Set
                               running=true BEFORE deliver captures RF, otherwise IRET would
                               restore running=false and tick() would exit again the instant the
                               handler returns, re-parking without the guest ever advancing (an
                               infinite park-deliver loop). The in-tick delivery path never hit
                               this because there the guest is already running (RF running=true).*/
                            running_flag = true;
                            deliver_vectored(vector, 0, 0, regs::rp.w0);
                        }
                    }
                }
            }
        }

    } // namespace cpu; 
} // namespace maize
