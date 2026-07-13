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
                        reg.storage_ = val;
                    }
                    else {
                        u_word mask = (count >= 8) ? ~u_word {0}
                                                   : ((u_word {1} << (count * 8)) - 1);
                        mask <<= (dst_idx * 8);
                        reg.storage_ = (reg.storage_ & ~mask) | ((val << (dst_idx * 8)) & mask);
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

        namespace {
            flag<bit_carryout> carryout_flag {regs::rf};
            flag<bit_negative> negative_flag {regs::rf};
            flag<bit_overflow> overflow_flag {regs::rf};
            flag<bit_parity> parity_flag {regs::rf};
            flag<bit_zero> zero_flag {regs::rf};
            flag<bit_sign> sign_flag {regs::rf};
            flag<bit_privilege> privilege_flag {regs::rf};
            flag<bit_interrupt_enabled> interrupt_enabled_flag {regs::rf};
            flag<bit_interrupt_set> interrupt_set_flag {regs::rf};
            flag<bit_running> running_flag {regs::rf};

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
                return regs::ri.b1 & opflag_imm_size;
            }

            u_byte op1_imm_size() {
                return u_byte(1) << (regs::ri.b1 & opflag_imm_size);
            }

            u_byte op1_reg_flag() {
                return regs::ri.b1 & opflag_reg;
            }

            u_byte op1_reg_index() {
                return (regs::ri.b1 & opflag_reg) >> 4;
            }

            u_byte op1_subreg_index() {
                return regs::ri.b1 & opflag_subreg;
            }

            subreg_enum op1_subreg_flag() {
                return static_cast<subreg_enum>(regs::ri.b1 & opflag_subreg);
            }

            u_byte op1_subreg_size() {
                return subreg_size_map[static_cast<size_t>(static_cast<subreg_enum>(regs::ri.b1 & opflag_subreg))];
            }

            reg& op1_reg() {
                return *reg_map[(regs::ri.b1 & opflag_reg) >> 4];
            }

            u_byte op2_imm_size_flag() {
                return regs::ri.b2 & opflag_imm_size;
            }

            u_byte op2_imm_size() {
                return 1 << (regs::ri.b2 & opflag_imm_size);
            }

            u_byte op2_reg_flag() {
                return regs::ri.b2 & opflag_reg;
            }

            u_byte op2_reg_index() {
                return (regs::ri.b2 & opflag_reg) >> 4;
            }

            u_byte op2_subreg_index() {
                return regs::ri.b2 & opflag_subreg;
            }

            subreg_enum op2_subreg_flag() {
                return static_cast<subreg_enum>(regs::ri.b2 & opflag_subreg);
            }

            u_byte op2_subreg_size() {
                return subreg_size_map[static_cast<size_t>(static_cast<subreg_enum>(regs::ri.b2 & opflag_subreg))];
            }

            reg& op2_reg() {
                return *reg_map[(regs::ri.b2 & opflag_reg) >> 4];
            }

            u_byte op3_reg_flag() {
                return regs::ri.b3 & opflag_reg;
            }

            u_byte op3_reg_index() {
                return (regs::ri.b3 & opflag_reg) >> 4;
            }

            u_byte op3_subreg_index() {
                return regs::ri.b3 & opflag_subreg;
            }

            subreg_enum op3_subreg_flag() {
                return static_cast<subreg_enum>(regs::ri.b3 & opflag_subreg);
            }

            u_byte op3_subreg_size() {
                return subreg_size_map[static_cast<size_t>(static_cast<subreg_enum>(regs::ri.b3 & opflag_subreg))];
            }

            reg &op3_reg() {
                return *reg_map[(regs::ri.b3 & opflag_reg) >> 4];
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

            void write_subreg_bits(reg_value &dst, subreg_enum dst_subreg, u_word bits) {
                auto off = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];
                dst.w0 = (~static_cast<u_word>(mask) & dst.w0) | ((bits << off) & static_cast<u_word>(mask));
            }

            void clr_reg(reg_value &dst, subreg_enum dst_subreg) {
                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];

                u_word src_value = 0;
                dst.w0 = (~static_cast<u_word>(dst_mask) & dst.w0) | (src_value << dst_offset) & static_cast<u_word>(dst_mask);
            }

            /* SETcc write (card maize-55): identical masked write to clr_reg, but
               src_value is the condition (0 or 1) instead of the constant 0. The
               named destination subregister field becomes 0/1 and the rest of the
               register is preserved. Flag-neutral: RF is never touched here. */
            void set_reg(reg_value &dst, subreg_enum dst_subreg, bool condition) {
                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];

                u_word src_value = condition ? 1 : 0;
                dst.w0 = (~static_cast<u_word>(dst_mask) & dst.w0) | (src_value << dst_offset) & static_cast<u_word>(dst_mask);
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

                dst.w0 = (~static_cast<u_word>(dst_mask) & dst.w0) | (src_value << dst_offset) & static_cast<u_word>(dst_mask);
            }

            void copy_regval_reg_zext(reg_value const &src, subreg_enum src_subreg, reg_value &dst, subreg_enum dst_subreg) {
                auto src_offset = subreg_offset_map[static_cast<size_t>(src_subreg)];
                auto src_mask = subreg_mask_map[static_cast<size_t>(src_subreg)];

                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];

                u_word src_value = (src.w0 & static_cast<u_word>(src_mask)) >> src_offset;

                dst.w0 = (~static_cast<u_word>(dst_mask) & dst.w0) | (src_value << dst_offset) & static_cast<u_word>(dst_mask);
            }

            void copy_memval_reg(u_word address, size_t size, reg_value &op2_reg, subreg_enum dst_subreg) {
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
                mm.read_into(address, reinterpret_cast<u_byte*>(&raw), size);
                subreg_enum src_subreg = imm_size_subreg_map[static_cast<size_t>(size)];
                if (raw & subreg_sign_bit[static_cast<int>(src_subreg)]) {
                    raw |= subreg_neg_bits[static_cast<int>(src_subreg)];
                }
                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];
                op2_reg.w0 = (~static_cast<u_word>(dst_mask) & op2_reg.w0)
                    | ((raw << dst_offset) & static_cast<u_word>(dst_mask));
            }

            void copy_memval_reg_zext(u_word address, size_t size, reg_value &op2_reg, subreg_enum dst_subreg) {
                /* Zero-extending immediate value for CPZ (card maize-29): a narrow immediate fills
                   the destination's upper bytes with zero, the unsigned counterpart to
                   copy_memval_reg's sign-extension. read_into into a zeroed word is already the
                   zero-extended value; no reg_value temporary. */
                u_word raw = 0;
                mm.read_into(address, reinterpret_cast<u_byte*>(&raw), size);
                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];
                op2_reg.w0 = (~static_cast<u_word>(dst_mask) & op2_reg.w0)
                    | ((raw << dst_offset) & static_cast<u_word>(dst_mask));
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
                mm.read(src_address, src_data, subreg_size_map[static_cast<size_t>(dst_subreg)], 0);

                dst.w0 = (~static_cast<u_word>(dst_mask) & dst.w0) | (src_data.w0 << dst_offset) & static_cast<u_word>(dst_mask);
            }

            void copy_memaddr_reg(u_word address, size_t size, reg_value &dst, subreg_enum dst_subreg) {

                /* `size` is the width of the immediate ADDRESS operand in the code stream
                   (op1_imm_size). Read exactly that many bytes, zero-extended: reading a fixed
                   8 bytes over-reads into the following instruction bytes and computes a garbage
                   address when the address is encoded in fewer than 8 bytes (card maize-40). */
                reg src_address;
                mm.read(address, src_address, size, 0);

                /* The load width is the destination subregister size, not a fixed 8 (card
                   maize-29): read exactly as many bytes from the target address as the destination
                   holds, so `LD @imm R0.B0` reads one byte and the rest of R0 is preserved. */
                reg src_data;
                src_data.w0 = 0;
                mm.read(src_address.w0, src_data, subreg_size_map[static_cast<size_t>(dst_subreg)], 0);

                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];

                dst.w0 = (~static_cast<u_word>(dst_mask) & dst.w0) | (src_data.w0 << dst_offset) & static_cast<u_word>(dst_mask);

            }

            /* Set PC (RP) from an immediate jump/branch target in the code stream at PC, encoded
               at op1_imm_size() width (maize-41). Read exactly that many bytes zero-extended into
               a fresh register, then replace PC. Honors the size flag so a target can reach the
               full 64-bit address space instead of the old hardcoded 4 bytes. */
            void jump_to_immediate() {
                u_byte imm_size = op1_imm_size();
                reg target;
                mm.read(regs::rp.w0, target, imm_size, 0);
                regs::rp.w0 = target.w0;
            }

            /* Shared condition machinery (card maize-64). The two high opcode bits
               select the condition "row" and the base slot selects the "column";
               decode_condition folds them into a single index that eval_condition
               maps to a flag predicate. This ONE predicate table drives BOTH Jcc
               and SETcc, so the flag formulas have a single source of truth (no
               copy-pasted per-condition expressions in the dispatch cases). */
            u_byte decode_condition(u_byte base) {
                u_byte row = (regs::ri.b0 & opcode_flag) >> 6;
                u_byte col = static_cast<u_byte>((regs::ri.b0 & 0x3F) - base);
                return static_cast<u_byte>(row * 3 + col);
            }

            bool eval_condition(u_byte cond) {
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
                        err << "unallocated condition encoding: " << std::hex << static_cast<unsigned>(regs::ri.b0);
                        throw std::logic_error(err.str());
                    }
                }
            }

            /* ALU micro-op selectors for the packed unary family (card maize-64).
               The packed unary instruction bytes share their low-6 bits ($31 for
               INC/DEC/NOT/NEG, $32 for CLR/POP), so run_alu (which dispatches on
               alu.b0 & opflag_code) cannot decode them off the raw opcode. tick()
               translates the condition-style row bits to one of these low-6-unique
               selectors before calling run_alu. */
            const u_byte alu_uop_inc {0x31};
            const u_byte alu_uop_dec {0x32};
            const u_byte alu_uop_not {0x33};
            const u_byte alu_uop_neg {0x2A};

            void copy_regval_regaddr(reg_value const &src, subreg_enum src_subreg, reg_value const &dst, subreg_enum dst_subreg) {
                auto src_offset = subreg_offset_map[static_cast<size_t>(src_subreg)];
                auto src_mask = subreg_mask_map[static_cast<size_t>(src_subreg)];
                auto size = subreg_size_map[static_cast<size_t>(src_subreg)];

                auto dst_offset = subreg_offset_map[static_cast<size_t>(dst_subreg)];
                auto dst_mask = subreg_mask_map[static_cast<size_t>(dst_subreg)];

                reg_value src_data;
                src_data.w0 = static_cast<s_word>((src.w0 & static_cast<u_word>(src_mask)) >> src_offset);
                u_word dst_address = (dst.w0 & static_cast<u_word>(dst_mask)) >> dst_offset;

                switch (size) {
                    case 1: {
                        mm.write_byte(dst_address, src_data.b0);
                        break;
                    }

                    case 2: {
                        mm.write_qword(dst_address, src_data.q0);
                        break;
                    }

                    case 4: {
                        mm.write_hword(dst_address, src_data.h0);
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
                mm.read(address, src_data, size, 0);
                u_word dst_address = (dst.w0 & static_cast<u_word>(dst_mask)) >> dst_offset;

                switch (size) {
                    case 1: {
                        mm.write_byte(dst_address, src_data.b0);
                        break;
                    }

                    case 2: {
                        mm.write_qword(dst_address, src_data.q0);
                        break;
                    }

                    case 4: {
                        mm.write_hword(dst_address, src_data.h0);
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

            enum class run_states {
                decode,
                execute,
                syscall,
                arithmetic_logic_unit
            };

            run_states run_state = run_states::decode;

            std::mutex int_mutex;
            std::condition_variable int_event;

            std::mutex io_set_mutex;
            std::condition_variable io_set_event;

            reg operand1;
            reg operand2;

            bool is_power_on = false;

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
               unless a host input device is selected as the sole stdin consumer; the run
               loop calls its on_input_tick() once per executed instruction so it pulls its
               bytes and raises its IRQ on the CPU thread. */
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
            u_word saved_rf = regs::rf.w0;
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
            if (!interrupt_enabled_flag || !irq_pending) {
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
           while a one-shot timer clears its own enable bit. */
        void timer_device::on_instruction_tick() {
            bool enable = (control_reg.w0 & 0x1) != 0;
            if (!enable) {
                return;
            }
            if ((status_reg.w0 & 0x1) != 0) {
                /* Tick pending, waiting for the handler's ack: paused. */
                return;
            }
            if (counter == 0) {
                /* Freshly programmed or re-armed after an ack: (re)load the period. A
                   zero period is inert (an unprogrammed / disabled countdown). */
                counter = period_reg.w0;
                if (counter == 0) {
                    return;
                }
            }
            --counter;
            if (counter == 0) {
                status_reg.w0 |= 0x1;   // set tick-pending
                raise_irq(irq_vector);
                bool periodic = (control_reg.w0 & 0x2) != 0;
                if (!periodic) {
                    control_reg.w0 &= ~static_cast<u_word>(0x1);   // one-shot: disable
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
                return (static_cast<u_byte>(regs::fcsr.b0) >> 5) & 0x07;
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
                regs::fcsr.b0 = static_cast<u_byte>(regs::fcsr.b0) | (fflag_bits & 0x1F);
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
                if (c.nv) {
                    fcsr_raise(fpu::fflag_nv);
                }
            }
        }

        void run_alu() {
            u_byte op_size = alu.b2; // Destination size
            u_byte alu_op = alu.b0 & arithmetic_logic_unit::opflag_code;
            u_word alu_op2_entry = alu.op2_reg.w0; // preserved to restore for compare/test ops

            switch (alu_op) {
                case instr::add_opcode: {
                    /* Carry (C) is the unsigned carry-out; overflow (V) is the signed-overflow
                       test (same-sign operands, result sign differs). See card maize-1. */
                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0;
                            u_byte src = alu.op1_reg.b0;
                            u_byte result = dst_before + src;
                            bool z = result == 0;
                            bool n = result & 0x80;
                            bool c = result < dst_before;
                            bool v = (~(dst_before ^ src) & (dst_before ^ result)) & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0;
                            u_qword src = alu.op1_reg.q0;
                            u_qword result = dst_before + src;
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            bool c = result < dst_before;
                            bool v = (~(dst_before ^ src) & (dst_before ^ result)) & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0;
                            u_hword src = alu.op1_reg.h0;
                            u_hword result = dst_before + src;
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            bool c = result < dst_before;
                            bool v = (~(dst_before ^ src) & (dst_before ^ result)) & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word src = alu.op1_reg.w0;
                            u_word result = dst_before + src;
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            bool c = result < dst_before;
                            bool v = (~(dst_before ^ src) & (dst_before ^ result)) & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
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
                       from the minuend's sign). See card maize-1. */
                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0;
                            u_byte src = alu.op1_reg.b0;
                            u_byte result = dst_before - src;
                            bool z = result == 0;
                            bool n = result & 0x80;
                            bool c = result > dst_before;
                            bool v = ((dst_before ^ src) & (dst_before ^ result)) & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0;
                            u_qword src = alu.op1_reg.q0;
                            u_qword result = dst_before - src;
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            bool c = result > dst_before;
                            bool v = ((dst_before ^ src) & (dst_before ^ result)) & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0;
                            u_hword src = alu.op1_reg.h0;
                            u_hword result = dst_before - src;
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            bool c = result > dst_before;
                            bool v = ((dst_before ^ src) & (dst_before ^ result)) & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word src = alu.op1_reg.w0;
                            u_word result = dst_before - src;
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            bool c = result > dst_before;
                            bool v = ((dst_before ^ src) & (dst_before ^ result)) & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
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
                    unsigned carry_in = (bool)carryout_flag ? 1u : 0u;

                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0;
                            u_byte src = alu.op1_reg.b0;
                            u_byte sum1 = dst_before + src;
                            u_byte result = sum1 + static_cast<u_byte>(carry_in);
                            bool z = result == 0;
                            bool n = result & 0x80;
                            bool c = (sum1 < dst_before) || (result < sum1);
                            bool v = (~(dst_before ^ src) & (dst_before ^ result)) & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0;
                            u_qword src = alu.op1_reg.q0;
                            u_qword sum1 = dst_before + src;
                            u_qword result = sum1 + static_cast<u_qword>(carry_in);
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            bool c = (sum1 < dst_before) || (result < sum1);
                            bool v = (~(dst_before ^ src) & (dst_before ^ result)) & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0;
                            u_hword src = alu.op1_reg.h0;
                            u_hword sum1 = dst_before + src;
                            u_hword result = sum1 + static_cast<u_hword>(carry_in);
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            bool c = (sum1 < dst_before) || (result < sum1);
                            bool v = (~(dst_before ^ src) & (dst_before ^ result)) & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word src = alu.op1_reg.w0;
                            u_word sum1 = dst_before + src;
                            u_word result = sum1 + static_cast<u_word>(carry_in);
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            bool c = (sum1 < dst_before) || (result < sum1);
                            bool v = (~(dst_before ^ src) & (dst_before ^ result)) & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::sbb_opcode: {
                    /* Subtract with borrow: dst - src - C (card maize-6). C = unsigned borrow
                       (x86 convention), V = signed overflow, N = sign, Z = this word's result. */
                    unsigned borrow_in = (bool)carryout_flag ? 1u : 0u;

                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0;
                            u_byte src = alu.op1_reg.b0;
                            u_byte diff1 = dst_before - src;
                            u_byte result = diff1 - static_cast<u_byte>(borrow_in);
                            bool z = result == 0;
                            bool n = result & 0x80;
                            bool c = (diff1 > dst_before) || (result > diff1);
                            bool v = ((dst_before ^ src) & (dst_before ^ result)) & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0;
                            u_qword src = alu.op1_reg.q0;
                            u_qword diff1 = dst_before - src;
                            u_qword result = diff1 - static_cast<u_qword>(borrow_in);
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            bool c = (diff1 > dst_before) || (result > diff1);
                            bool v = ((dst_before ^ src) & (dst_before ^ result)) & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0;
                            u_hword src = alu.op1_reg.h0;
                            u_hword diff1 = dst_before - src;
                            u_hword result = diff1 - static_cast<u_hword>(borrow_in);
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            bool c = (diff1 > dst_before) || (result > diff1);
                            bool v = ((dst_before ^ src) & (dst_before ^ result)) & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word src = alu.op1_reg.w0;
                            u_word diff1 = dst_before - src;
                            u_word result = diff1 - static_cast<u_word>(borrow_in);
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            bool c = (diff1 > dst_before) || (result > diff1);
                            bool v = ((dst_before ^ src) & (dst_before ^ result)) & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::mul_opcode: {
                    /* Overflow (V) is the signed-overflow test on the pre-op operands. Carry (C) mirrors
                       V until the wide-multiply card (97821c447640) lands a high-half product; see card
                       maize-1 decision. The width-8 case reads the .w0 (64-bit) subregisters that its own
                       multiply uses, not the .h0 (32-bit) subregisters. */
                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0 * alu.op1_reg.b0;
                            bool z = result == 0;
                            bool n = result & 0x80;
                            bool ovf = is_mul_overflow(static_cast<u_byte>(alu.op1_reg.b0), static_cast<u_byte>(alu.op2_reg.b0)) || is_mul_underflow(static_cast<u_byte>(alu.op1_reg.b0), static_cast<u_byte>(alu.op2_reg.b0));
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (ovf ? bit_carryout : 0) | (ovf ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0 * alu.op1_reg.q0;
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            bool ovf = is_mul_overflow(static_cast<u_qword>(alu.op1_reg.q0), static_cast<u_qword>(alu.op2_reg.q0)) || is_mul_underflow(static_cast<u_qword>(alu.op1_reg.q0), static_cast<u_qword>(alu.op2_reg.q0));
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (ovf ? bit_carryout : 0) | (ovf ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0 * alu.op1_reg.h0;
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            bool ovf = is_mul_overflow(static_cast<u_hword>(alu.op1_reg.h0), static_cast<u_hword>(alu.op2_reg.h0)) || is_mul_underflow(static_cast<u_hword>(alu.op1_reg.h0), static_cast<u_hword>(alu.op2_reg.h0));
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (ovf ? bit_carryout : 0) | (ovf ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 * alu.op1_reg.w0;
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            bool ovf = is_mul_overflow(alu.op1_reg.w0, alu.op2_reg.w0) || is_mul_underflow(alu.op1_reg.w0, alu.op2_reg.w0);
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (ovf ? bit_carryout : 0) | (ovf ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
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
                            s_qword p = static_cast<s_qword>(static_cast<s_byte>(alu.op2_reg.b0)) * static_cast<s_qword>(static_cast<s_byte>(alu.op1_reg.b0));
                            u_qword up = static_cast<u_qword>(p);
                            u_word lo = up & 0xFF;
                            u_word hi = (up >> 8) & 0xFF;
                            bool z = (lo == 0 && hi == 0);
                            bool n = hi & 0x80;
                            bool c = hi != 0;
                            bool v = (p < -128 || p > 127);
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = lo;
                            alu.op1_reg.w0 = hi;
                            break;
                        }

                        case 2: {
                            s_hword p = static_cast<s_hword>(static_cast<s_qword>(alu.op2_reg.q0)) * static_cast<s_hword>(static_cast<s_qword>(alu.op1_reg.q0));
                            u_hword up = static_cast<u_hword>(p);
                            u_word lo = up & 0xFFFF;
                            u_word hi = (up >> 16) & 0xFFFF;
                            bool z = (lo == 0 && hi == 0);
                            bool n = hi & 0x8000;
                            bool c = hi != 0;
                            bool v = (p < -32768 || p > 32767);
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = lo;
                            alu.op1_reg.w0 = hi;
                            break;
                        }

                        case 4: {
                            s_word p = static_cast<s_word>(static_cast<s_hword>(alu.op2_reg.h0)) * static_cast<s_word>(static_cast<s_hword>(alu.op1_reg.h0));
                            u_word up = static_cast<u_word>(p);
                            u_word lo = up & 0xFFFFFFFF;
                            u_word hi = (up >> 32) & 0xFFFFFFFF;
                            bool z = (lo == 0 && hi == 0);
                            bool n = hi & 0x80000000;
                            bool c = hi != 0;
                            bool v = (p < INT32_MIN || p > INT32_MAX);
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = lo;
                            alu.op1_reg.w0 = hi;
                            break;
                        }

                        case 8: {
                            __int128 p = static_cast<__int128>(static_cast<s_word>(alu.op2_reg.w0)) * static_cast<__int128>(static_cast<s_word>(alu.op1_reg.w0));
                            unsigned __int128 up = static_cast<unsigned __int128>(p);
                            u_word lo = static_cast<u_word>(up);
                            u_word hi = static_cast<u_word>(up >> 64);
                            bool z = (lo == 0 && hi == 0);
                            bool n = hi & 0x8000000000000000;
                            bool c = hi != 0;
                            bool v = (p < static_cast<__int128>(INT64_MIN) || p > static_cast<__int128>(INT64_MAX));
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = lo;
                            alu.op1_reg.w0 = hi;
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
                            u_qword up = static_cast<u_qword>(alu.op2_reg.b0) * static_cast<u_qword>(alu.op1_reg.b0);
                            u_word lo = up & 0xFF;
                            u_word hi = (up >> 8) & 0xFF;
                            bool z = (lo == 0 && hi == 0);
                            bool n = hi & 0x80;
                            bool c = hi != 0;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (c ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = lo;
                            alu.op1_reg.w0 = hi;
                            break;
                        }

                        case 2: {
                            u_hword up = static_cast<u_hword>(alu.op2_reg.q0) * static_cast<u_hword>(alu.op1_reg.q0);
                            u_word lo = up & 0xFFFF;
                            u_word hi = (up >> 16) & 0xFFFF;
                            bool z = (lo == 0 && hi == 0);
                            bool n = hi & 0x8000;
                            bool c = hi != 0;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (c ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = lo;
                            alu.op1_reg.w0 = hi;
                            break;
                        }

                        case 4: {
                            u_word up = static_cast<u_word>(alu.op2_reg.h0) * static_cast<u_word>(alu.op1_reg.h0);
                            u_word lo = up & 0xFFFFFFFF;
                            u_word hi = (up >> 32) & 0xFFFFFFFF;
                            bool z = (lo == 0 && hi == 0);
                            bool n = hi & 0x80000000;
                            bool c = hi != 0;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (c ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = lo;
                            alu.op1_reg.w0 = hi;
                            break;
                        }

                        case 8: {
                            unsigned __int128 up = static_cast<unsigned __int128>(alu.op2_reg.w0) * static_cast<unsigned __int128>(alu.op1_reg.w0);
                            u_word lo = static_cast<u_word>(up);
                            u_word hi = static_cast<u_word>(up >> 64);
                            bool z = (lo == 0 && hi == 0);
                            bool n = hi & 0x8000000000000000;
                            bool c = hi != 0;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (c ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = lo;
                            alu.op1_reg.w0 = hi;
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
                            s_byte divisor = alu.op1_reg.b0;
                            s_byte dividend = alu.op2_reg.b0;
                            if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                            if (divisor == -1 && dividend == std::numeric_limits<s_byte>::min()) { raise_divide_error("signed division overflow"); }
                            s_byte result = dividend / divisor;
                            bool z = result == 0;
                            bool n = result & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = static_cast<u_byte>(result);
                            break;
                        }

                        case 2: {
                            s_qword divisor = alu.op1_reg.q0;
                            s_qword dividend = alu.op2_reg.q0;
                            if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                            if (divisor == -1 && dividend == std::numeric_limits<s_qword>::min()) { raise_divide_error("signed division overflow"); }
                            s_qword result = dividend / divisor;
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = static_cast<u_qword>(result);
                            break;
                        }

                        case 4: {
                            s_hword divisor = alu.op1_reg.h0;
                            s_hword dividend = alu.op2_reg.h0;
                            if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                            if (divisor == -1 && dividend == std::numeric_limits<s_hword>::min()) { raise_divide_error("signed division overflow"); }
                            s_hword result = dividend / divisor;
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = static_cast<u_hword>(result);
                            break;
                        }

                        case 8: {
                            s_word divisor = alu.op1_reg.w0;
                            s_word dividend = alu.op2_reg.w0;
                            if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                            if (divisor == -1 && dividend == std::numeric_limits<s_word>::min()) { raise_divide_error("signed division overflow"); }
                            s_word result = dividend / divisor;
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
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
                            s_byte divisor = alu.op1_reg.b0;
                            s_byte dividend = alu.op2_reg.b0;
                            if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                            if (divisor == -1 && dividend == std::numeric_limits<s_byte>::min()) { raise_divide_error("signed division overflow"); }
                            s_byte result = dividend % divisor;
                            bool z = result == 0;
                            bool n = result & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = static_cast<u_byte>(result);
                            break;
                        }

                        case 2: {
                            s_qword divisor = alu.op1_reg.q0;
                            s_qword dividend = alu.op2_reg.q0;
                            if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                            if (divisor == -1 && dividend == std::numeric_limits<s_qword>::min()) { raise_divide_error("signed division overflow"); }
                            s_qword result = dividend % divisor;
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = static_cast<u_qword>(result);
                            break;
                        }

                        case 4: {
                            s_hword divisor = alu.op1_reg.h0;
                            s_hword dividend = alu.op2_reg.h0;
                            if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                            if (divisor == -1 && dividend == std::numeric_limits<s_hword>::min()) { raise_divide_error("signed division overflow"); }
                            s_hword result = dividend % divisor;
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = static_cast<u_hword>(result);
                            break;
                        }

                        case 8: {
                            s_word divisor = alu.op1_reg.w0;
                            s_word dividend = alu.op2_reg.w0;
                            if (divisor == 0) { raise_divide_error("signed divide by zero"); }
                            if (divisor == -1 && dividend == std::numeric_limits<s_word>::min()) { raise_divide_error("signed division overflow"); }
                            s_word result = dividend % divisor;
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
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
                            u_byte divisor = alu.op1_reg.b0;
                            if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                            u_byte result = alu.op2_reg.b0 / divisor;
                            bool z = result == 0;
                            bool n = result & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword divisor = alu.op1_reg.q0;
                            if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                            u_qword result = alu.op2_reg.q0 / divisor;
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword divisor = alu.op1_reg.h0;
                            if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                            u_hword result = alu.op2_reg.h0 / divisor;
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word divisor = alu.op1_reg.w0;
                            if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                            u_word result = alu.op2_reg.w0 / divisor;
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
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
                            u_byte divisor = alu.op1_reg.b0;
                            if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                            u_byte result = alu.op2_reg.b0 % divisor;
                            bool z = result == 0;
                            bool n = result & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword divisor = alu.op1_reg.q0;
                            if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                            u_qword result = alu.op2_reg.q0 % divisor;
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword divisor = alu.op1_reg.h0;
                            if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                            u_hword result = alu.op2_reg.h0 % divisor;
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word divisor = alu.op1_reg.w0;
                            if (divisor == 0) { raise_divide_error("unsigned divide by zero"); }
                            u_word result = alu.op2_reg.w0 % divisor;
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
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
                            u_byte result = alu.op2_reg.b0 & alu.op1_reg.b0;
                            bool z = result == 0;
                            bool n = result & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0 & alu.op1_reg.q0;
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0 & alu.op1_reg.h0;
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 & alu.op1_reg.w0;
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::or_opcode: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0 | alu.op1_reg.b0;
                            bool z = result == 0;
                            bool n = result & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0 | alu.op1_reg.q0;
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0 | alu.op1_reg.h0;
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 | alu.op1_reg.w0;
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::nor_opcode: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = ~(alu.op2_reg.b0 | alu.op1_reg.b0);
                            bool z = result == 0;
                            bool n = result & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = ~(alu.op2_reg.q0 | alu.op1_reg.q0);
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = ~(alu.op2_reg.h0 | alu.op1_reg.h0);
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = ~(alu.op2_reg.w0 | alu.op1_reg.w0);
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::nand_opcode: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = ~(alu.op2_reg.b0 & alu.op1_reg.b0);
                            bool z = result == 0;
                            bool n = result & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = ~(alu.op2_reg.q0 & alu.op1_reg.q0);
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = ~(alu.op2_reg.h0 & alu.op1_reg.h0);
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = ~(alu.op2_reg.w0 & alu.op1_reg.w0);
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::xor_opcode: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0 ^ alu.op1_reg.b0;
                            bool z = result == 0;
                            bool n = result & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0 ^ alu.op1_reg.q0;
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0 ^ alu.op1_reg.h0;
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 ^ alu.op1_reg.w0;
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
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
                       Never pass an out-of-range count to C++'s << (that is undefined behavior). */
                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0;
                            u_word n = alu.op1_reg.b0;
                            const u_word bits = 8;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n <= bits) {
                                u_byte result = (n == bits) ? u_byte(0) : u_byte(dst_before << n);
                                bool z = result == 0;
                                bool nf = result & 0x80;
                                bool c = (dst_before >> (bits - n)) & 1;
                                bool v = (n == 1) && (((dst_before >> (bits - 1)) & 1) != ((dst_before >> (bits - 2)) & 1));
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                    | (z ? bit_zero : 0) | (nf ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = result;
                            }
                            else {
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow)) | bit_zero;
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = 0;
                            }
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0;
                            u_word n = alu.op1_reg.q0;
                            const u_word bits = 16;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n <= bits) {
                                u_qword result = (n == bits) ? u_qword(0) : u_qword(dst_before << n);
                                bool z = result == 0;
                                bool nf = result & 0x8000;
                                bool c = (dst_before >> (bits - n)) & 1;
                                bool v = (n == 1) && (((dst_before >> (bits - 1)) & 1) != ((dst_before >> (bits - 2)) & 1));
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                    | (z ? bit_zero : 0) | (nf ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = result;
                            }
                            else {
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow)) | bit_zero;
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = 0;
                            }
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0;
                            u_word n = alu.op1_reg.h0;
                            const u_word bits = 32;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n <= bits) {
                                u_hword result = (n == bits) ? u_hword(0) : u_hword(dst_before << n);
                                bool z = result == 0;
                                bool nf = result & 0x80000000;
                                bool c = (dst_before >> (bits - n)) & 1;
                                bool v = (n == 1) && (((dst_before >> (bits - 1)) & 1) != ((dst_before >> (bits - 2)) & 1));
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                    | (z ? bit_zero : 0) | (nf ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = result;
                            }
                            else {
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow)) | bit_zero;
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = 0;
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
                                bool z = result == 0;
                                bool nf = result & 0x8000000000000000;
                                bool c = (dst_before >> (bits - n)) & 1;
                                bool v = (n == 1) && (((dst_before >> (bits - 1)) & 1) != ((dst_before >> (bits - 2)) & 1));
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                    | (z ? bit_zero : 0) | (nf ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = result;
                            }
                            else {
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow)) | bit_zero;
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = 0;
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
                            u_byte dst_before = alu.op2_reg.b0;
                            u_word n = alu.op1_reg.b0;
                            const u_word bits = 8;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n <= bits) {
                                u_byte result = (n == bits) ? u_byte(0) : u_byte(dst_before >> n);
                                bool z = result == 0;
                                bool nf = result & 0x80;
                                bool c = (dst_before >> (n - 1)) & 1;
                                bool v = (n == 1) && (((dst_before >> (bits - 1)) & 1) != 0);
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                    | (z ? bit_zero : 0) | (nf ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = result;
                            }
                            else {
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow)) | bit_zero;
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = 0;
                            }
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0;
                            u_word n = alu.op1_reg.q0;
                            const u_word bits = 16;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n <= bits) {
                                u_qword result = (n == bits) ? u_qword(0) : u_qword(dst_before >> n);
                                bool z = result == 0;
                                bool nf = result & 0x8000;
                                bool c = (dst_before >> (n - 1)) & 1;
                                bool v = (n == 1) && (((dst_before >> (bits - 1)) & 1) != 0);
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                    | (z ? bit_zero : 0) | (nf ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = result;
                            }
                            else {
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow)) | bit_zero;
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = 0;
                            }
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0;
                            u_word n = alu.op1_reg.h0;
                            const u_word bits = 32;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n <= bits) {
                                u_hword result = (n == bits) ? u_hword(0) : u_hword(dst_before >> n);
                                bool z = result == 0;
                                bool nf = result & 0x80000000;
                                bool c = (dst_before >> (n - 1)) & 1;
                                bool v = (n == 1) && (((dst_before >> (bits - 1)) & 1) != 0);
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                    | (z ? bit_zero : 0) | (nf ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = result;
                            }
                            else {
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow)) | bit_zero;
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = 0;
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
                                bool z = result == 0;
                                bool nf = result & 0x8000000000000000;
                                bool c = (dst_before >> (n - 1)) & 1;
                                bool v = (n == 1) && (((dst_before >> (bits - 1)) & 1) != 0);
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                    | (z ? bit_zero : 0) | (nf ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = result;
                            }
                            else {
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow)) | bit_zero;
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = 0;
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
                            u_byte dst_before = alu.op2_reg.b0;
                            u_word n = alu.op1_reg.b0;
                            const u_word bits = 8;
                            bool sign = (dst_before >> (bits - 1)) & 1;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n < bits) {
                                u_byte result = u_byte(s_byte(dst_before) >> n);
                                bool z = result == 0;
                                bool nf = result & 0x80;
                                bool c = (dst_before >> (n - 1)) & 1;
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                    | (z ? bit_zero : 0) | (nf ? bit_negative : 0) | (c ? bit_carryout : 0);
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = result;
                            }
                            else {
                                u_byte result = sign ? u_byte(0xFF) : u_byte(0);
                                bool z = result == 0;
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                    | (z ? bit_zero : 0) | (sign ? bit_negative : 0) | (sign ? bit_carryout : 0);
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = result;
                            }
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0;
                            u_word n = alu.op1_reg.q0;
                            const u_word bits = 16;
                            bool sign = (dst_before >> (bits - 1)) & 1;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n < bits) {
                                u_qword result = u_qword(s_qword(dst_before) >> n);
                                bool z = result == 0;
                                bool nf = result & 0x8000;
                                bool c = (dst_before >> (n - 1)) & 1;
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                    | (z ? bit_zero : 0) | (nf ? bit_negative : 0) | (c ? bit_carryout : 0);
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = result;
                            }
                            else {
                                u_qword result = sign ? u_qword(0xFFFF) : u_qword(0);
                                bool z = result == 0;
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                    | (z ? bit_zero : 0) | (sign ? bit_negative : 0) | (sign ? bit_carryout : 0);
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = result;
                            }
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0;
                            u_word n = alu.op1_reg.h0;
                            const u_word bits = 32;
                            bool sign = (dst_before >> (bits - 1)) & 1;
                            if (n == 0) {
                                alu.op2_reg.w0 = dst_before;
                            }
                            else if (n < bits) {
                                u_hword result = u_hword(s_hword(dst_before) >> n);
                                bool z = result == 0;
                                bool nf = result & 0x80000000;
                                bool c = (dst_before >> (n - 1)) & 1;
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                    | (z ? bit_zero : 0) | (nf ? bit_negative : 0) | (c ? bit_carryout : 0);
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = result;
                            }
                            else {
                                u_hword result = sign ? u_hword(0xFFFFFFFF) : u_hword(0);
                                bool z = result == 0;
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                    | (z ? bit_zero : 0) | (sign ? bit_negative : 0) | (sign ? bit_carryout : 0);
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = result;
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
                                bool z = result == 0;
                                bool nf = result & 0x8000000000000000;
                                bool c = (dst_before >> (n - 1)) & 1;
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                    | (z ? bit_zero : 0) | (nf ? bit_negative : 0) | (c ? bit_carryout : 0);
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = result;
                            }
                            else {
                                u_word result = sign ? u_word(0xFFFFFFFFFFFFFFFF) : u_word(0);
                                bool z = result == 0;
                                u_word f = regs::rf.w0;
                                f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                    | (z ? bit_zero : 0) | (sign ? bit_negative : 0) | (sign ? bit_carryout : 0);
                                regs::rf.w0 = f;
                                alu.op2_reg.w0 = result;
                            }
                            break;
                        }
                    }

                    break;
                }

                case alu_uop_inc: {
                    /* INC is ADD with src = 1; C and V follow the ADD family (card maize-1). */
                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0;
                            u_byte result = dst_before + u_byte(1);
                            bool z = result == 0;
                            bool n = result & 0x80;
                            bool c = result < dst_before;
                            bool v = (~(dst_before ^ u_byte(1)) & (dst_before ^ result)) & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0;
                            u_qword result = dst_before + u_qword(1);
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            bool c = result < dst_before;
                            bool v = (~(dst_before ^ u_qword(1)) & (dst_before ^ result)) & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0;
                            u_hword result = dst_before + u_hword(1);
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            bool c = result < dst_before;
                            bool v = (~(dst_before ^ u_hword(1)) & (dst_before ^ result)) & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word result = dst_before + u_word(1);
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            bool c = result < dst_before;
                            bool v = (~(dst_before ^ u_word(1)) & (dst_before ^ result)) & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu_uop_dec: {
                    /* DEC is SUB with src = 1; C and V follow the SUB family (card maize-1). */
                    switch (op_size) {
                        case 1: {
                            u_byte dst_before = alu.op2_reg.b0;
                            u_byte result = dst_before - u_byte(1);
                            bool z = result == 0;
                            bool n = result & 0x80;
                            bool c = result > dst_before;
                            bool v = ((dst_before ^ u_byte(1)) & (dst_before ^ result)) & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword dst_before = alu.op2_reg.q0;
                            u_qword result = dst_before - u_qword(1);
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            bool c = result > dst_before;
                            bool v = ((dst_before ^ u_qword(1)) & (dst_before ^ result)) & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword dst_before = alu.op2_reg.h0;
                            u_hword result = dst_before - u_hword(1);
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            bool c = result > dst_before;
                            bool v = ((dst_before ^ u_hword(1)) & (dst_before ^ result)) & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word dst_before = alu.op2_reg.w0;
                            u_word result = dst_before - u_word(1);
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            bool c = result > dst_before;
                            bool v = ((dst_before ^ u_word(1)) & (dst_before ^ result)) & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu_uop_not: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = ~alu.op2_reg.b0;
                            bool z = result == 0;
                            bool n = result & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = ~alu.op2_reg.q0;
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = ~alu.op2_reg.h0;
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = ~alu.op2_reg.w0;
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0);
                            regs::rf.w0 = f;
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
                            u_byte x = alu.op2_reg.b0;
                            u_byte result = static_cast<u_byte>(u_byte(0) - x);
                            bool z = result == 0;
                            bool n = result & 0x80;
                            bool c = result != 0;
                            bool v = (x & result) & 0x80;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword x = alu.op2_reg.q0;
                            u_qword result = static_cast<u_qword>(u_qword(0) - x);
                            bool z = result == 0;
                            bool n = result & 0x8000;
                            bool c = result != 0;
                            bool v = (x & result) & 0x8000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword x = alu.op2_reg.h0;
                            u_hword result = static_cast<u_hword>(u_hword(0) - x);
                            bool z = result == 0;
                            bool n = result & 0x80000000;
                            bool c = result != 0;
                            bool v = (x & result) & 0x80000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word x = alu.op2_reg.w0;
                            u_word result = static_cast<u_word>(u_word(0) - x);
                            bool z = result == 0;
                            bool n = result & 0x8000000000000000;
                            bool c = result != 0;
                            bool v = (x & result) & 0x8000000000000000;
                            u_word f = regs::rf.w0;
                            f = (f & ~(bit_zero | bit_negative | bit_carryout | bit_overflow))
                                | (z ? bit_zero : 0) | (n ? bit_negative : 0) | (c ? bit_carryout : 0) | (v ? bit_overflow : 0);
                            regs::rf.w0 = f;
                            alu.op2_reg.w0 = result;
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

        /* Floating-point arithmetic runner (card maize-122), the FP analogue of
           run_alu for the four two-operand arithmetic ops. The caller loads the
           src operand into alu.op1_reg and the dst operand into alu.op2_reg (both
           as raw bit patterns), sets alu.b0 to the opcode byte and alu.b2 to the
           operation width (4 = binary32, 8 = binary64), and reads the result back
           from alu.op2_reg. Result = dst OP src (op2 OP op1), matching the integer
           ALU operand convention. FFLAGS are OR-ed into FCSR (sticky); the integer
           RF flags C/N/V/Z are left untouched (a dedicated FCSR, decision recorded). */
        void run_fpu_arith() {
            u_byte base = alu.b0 & arithmetic_logic_unit::opflag_code;
            u_byte width = alu.b2;
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

        /* This is the state machine that implements the machine-code instructions. */
        void tick() {
            running_flag = true;

            /* Host input is drained every input_poll_stride instructions rather than every
               one: on_input_tick is a virtual call, and in windowed mode (a real keyboard
               attached) paying it per instruction is pure overhead. A key press produces its
               scancode within a few microseconds of stride instructions, far below human
               perception, and the injection stays fully deterministic (just coarser). */
            constexpr unsigned input_poll_stride {64};
            unsigned input_poll_countdown {1};

            while (running_flag) {
                /* Interrupt delivery is checked at the instruction boundary (card
                   maize-21). A pending, enabled IRQ is delivered before the next
                   instruction decodes; the handler's first instruction runs on the next
                   iteration. The fast path is two RF-bit reads, so an idle machine pays
                   almost nothing. */
                if (try_deliver_interrupt()) {
                    continue;
                }

                /* Advance the instruction-tick timer once per executed instruction
                   (OQ3). Disabled/absent timer is an early return, so existing fixtures
                   are unaffected. */
                if (active_timer_ptr != nullptr) {
                    active_timer_ptr->on_instruction_tick();
                }

                /* Advance the single active host input source (device-plugin API). It pulls a
                   byte from its host source and raises its IRQ on the CPU thread when it has
                   room. Hoisted off the per-instruction path (drained every input_poll_stride
                   instructions) so the windowed hot loop does not pay a virtual call each
                   instruction; null (the default) is a cheap early skip that also avoids the
                   countdown decrement. */
                if (active_input_ptr != nullptr && --input_poll_countdown == 0) {
                    input_poll_countdown = input_poll_stride;
                    active_input_ptr->on_input_tick();
                }

                /* Decode next instruction */
                mm.read(regs::rp.w0, regs::ri, subreg_enum::w0);
                ++regs::rp.w0;
                run_state = run_states::execute;

                /* Execute instruction */
                switch (regs::ri.b0) {
                    case instr::halt_opcode: {
                        /* HALT halts the core pending an interrupt; it does NOT
                           carry an exit status. The VM has no interrupt source,
                           so a halted core has nothing to wake it: clearing
                           running_flag stops tick() and clearing is_power_on
                           makes run() return instead of blocking on
                           int_event.wait(). With no recorded exit code, maize
                           exits 0. The status-carrying termination path is
                           SYS $3C (sys_exit), see src/sys.cpp. */
                        power_off();
                        break;
                    }

                    case instr::clr_opcode: {
                        regs::rp.w0 += 1;
                        clr_reg(op1_reg(), op1_subreg_flag());
                        break;
                    }

                    /* SETcc (cards maize-55 / maize-64): materialize a flag condition
                       as a 0/1 value in the single register operand. The condition is
                       decoded from the opcode's row/column bits and evaluated by the
                       shared eval_condition predicate table, the SAME table Jcc uses,
                       so the two families can never disagree. Flag-neutral: RF is read
                       but never written. */
                    case instr::setz_opcode:
                    case instr::setnz_opcode:
                    case instr::setlt_opcode:
                    case instr::setb_opcode:
                    case instr::setgt_opcode:
                    case instr::seta_opcode:
                    case instr::setge_opcode:
                    case instr::setle_opcode:
                    case instr::setbe_opcode:
                    case instr::setae_opcode:
                    case instr::setp_opcode: {
                        regs::rp.w0 += 1;
                        set_reg(op1_reg(), op1_subreg_flag(),
                            eval_condition(decode_condition(instr::setcc_base)));
                        break;
                    }

                    case instr::cp_regVal_reg: {
                        regs::rp.w0 += 2;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::cp_immVal_reg: {
                        regs::rp.w0 += 2;
                        u_hword imm_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, imm_size, op2_reg(), op2_subreg_flag());
                        regs::rp.w0 += imm_size;
                        break;
                    }

                    case instr::ld_regAddr_reg: {
                        regs::rp.w0 += 2;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::ld_immAddr_reg: {
                        regs::rp.w0 += 2;
                        u_hword imm_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, imm_size, op2_reg(), op2_subreg_flag());
                        regs::rp.w0 += imm_size;
                        break;
                    }

                    case instr::cpz_regVal_reg: {
                        regs::rp.w0 += 2;
                        copy_regval_reg_zext(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::cpz_immVal_reg: {
                        regs::rp.w0 += 2;
                        u_hword imm_size = op1_imm_size();
                        copy_memval_reg_zext(regs::rp.w0, imm_size, op2_reg(), op2_subreg_flag());
                        regs::rp.w0 += imm_size;
                        break;
                    }

                    /* LDZ (the zero-extending load, opcodes $93 / $D3) was removed as redundant
                       (card maize-29): LD reads exactly the destination width, so a load never
                       narrows and has nothing to zero-extend. CPZ remains as the zero-extending
                       copy; those address-form encodings are now reserved. */

                    case instr::st_regVal_regAddr: {
                        regs::rp.w0 += 2;
                        copy_regval_regaddr(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::st_immVal_regAddr: {
                        regs::rp.w0 += 2;
                        u_hword imm_size = op1_imm_size();
                        copy_memval_regaddr(regs::rp.w0, imm_size, op2_reg(), op2_subreg_flag());
                        regs::rp.w0 += imm_size;
                        break;
                    }

                    case instr::add_regVal_reg:
                    case instr::sub_regVal_reg:
                    case instr::mul_regVal_reg:
                    case instr::div_regVal_reg:
                    case instr::mod_regVal_reg:
                    case instr::udiv_regVal_reg:
                    case instr::adc_regVal_reg:
                    case instr::sbb_regVal_reg:
                    case instr::umod_regVal_reg:
                    case instr::and_regVal_reg:
                    case instr::or_regVal_reg:
                    case instr::nor_regVal_reg:
                    case instr::nand_regVal_reg:
                    case instr::xor_regVal_reg:
                    case instr::shl_regVal_reg:
                    case instr::shr_regVal_reg:
                    case instr::sar_regVal_reg:
                    case instr::cmp_regVal_reg:
                    case instr::test_regVal_reg: {
                        regs::rp.w0 += 2;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::ri.b0;
                        alu.b1 = op1_subreg_size();
                        alu.b2 = op2_subreg_size();
                        run_alu();
                        /* The value writeback is flag-neutral (card maize-4), so the ALU's
                           per-width C/N/V/Z stand as computed. */
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::add_immVal_reg:
                    case instr::sub_immVal_reg:
                    case instr::mul_immVal_reg:
                    case instr::div_immVal_reg:
                    case instr::mod_immVal_reg:
                    case instr::udiv_immVal_reg:
                    case instr::adc_immVal_reg:
                    case instr::sbb_immVal_reg:
                    case instr::umod_immVal_reg:
                    case instr::and_immVal_reg:
                    case instr::or_immVal_reg:
                    case instr::nor_immVal_reg:
                    case instr::nand_immVal_reg:
                    case instr::xor_immVal_reg:
                    case instr::shl_immVal_reg:
                    case instr::shr_immVal_reg:
                    case instr::sar_immVal_reg:
                    case instr::cmp_immVal_reg:
                    case instr::test_immVal_reg: {
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::ri.b0;
                        alu.b1 = src_size;
                        alu.b2 = op2_subreg_size();
                        run_alu();
                        regs::rp.w0 += src_size;
                        /* Value writeback is flag-neutral (card maize-4); the ALU's flags stand. */
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::add_regAddr_reg:
                    case instr::sub_regAddr_reg:
                    case instr::mul_regAddr_reg:
                    case instr::div_regAddr_reg:
                    case instr::mod_regAddr_reg:
                    case instr::udiv_regAddr_reg:
                    case instr::adc_regAddr_reg:
                    case instr::sbb_regAddr_reg:
                    case instr::umod_regAddr_reg:
                    case instr::and_regAddr_reg:
                    case instr::or_regAddr_reg:
                    case instr::nor_regAddr_reg:
                    case instr::nand_regAddr_reg:
                    case instr::xor_regAddr_reg:
                    case instr::shl_regAddr_reg:
                    case instr::shr_regAddr_reg:
                    case instr::sar_regAddr_reg:
                    case instr::cmp_regAddr_reg:
                    case instr::test_regAddr_reg: {
                        regs::rp.w0 += 2;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::ri.b0;
                        alu.b1 = op1_subreg_size();
                        alu.b2 = op2_subreg_size();
                        run_alu();
                        /* The value writeback is flag-neutral (card maize-4), so the ALU's
                           per-width C/N/V/Z stand as computed. */
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::add_immAddr_reg:
                    case instr::sub_immAddr_reg:
                    case instr::mul_immAddr_reg:
                    case instr::div_immAddr_reg:
                    case instr::mod_immAddr_reg:
                    case instr::udiv_immAddr_reg:
                    case instr::adc_immAddr_reg:
                    case instr::sbb_immAddr_reg:
                    case instr::umod_immAddr_reg:
                    case instr::and_immAddr_reg:
                    case instr::or_immAddr_reg:
                    case instr::nor_immAddr_reg:
                    case instr::nand_immAddr_reg:
                    case instr::xor_immAddr_reg:
                    case instr::shl_immAddr_reg:
                    case instr::shr_immAddr_reg:
                    case instr::sar_immAddr_reg:
                    case instr::cmp_immAddr_reg:
                    case instr::test_immAddr_reg: {
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::ri.b0;
                        alu.b1 = src_size;
                        alu.b2 = op2_subreg_size();
                        run_alu();
                        regs::rp.w0 += src_size;
                        /* Value writeback is flag-neutral (card maize-4); the ALU's flags stand. */
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    /* Packed unary ALU family (card maize-64): INC ($31) / DEC ($71) /
                       NOT ($B1) / NEG ($F1) share base slot $31, distinguished by the
                       condition-style row bits. run_alu dispatches on alu.b0 & opflag_code,
                       so translate the row to a low-6-unique micro-op selector first. */
                    case instr::inc_opcode:
                    case instr::dec_opcode:
                    case instr::not_opcode:
                    case instr::neg_opcode: {
                        regs::rp.w0 += 1;
                        static const u_byte uop_sel[4] {alu_uop_inc, alu_uop_dec, alu_uop_not, alu_uop_neg};
                        u_byte row = (regs::ri.b0 & opcode_flag) >> 6;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = uop_sel[row];
                        alu.b1 = op1_subreg_size();
                        alu.b2 = op1_subreg_size();
                        run_alu();
                        /* Value writeback is flag-neutral (card maize-4); the ALU's flags stand. */
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op1_reg(), op1_subreg_flag());
                        break;
                    }

                    case instr::cmpind_immVal_regAddr:
                    case instr::testind_immVal_regAddr:
                    {
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regaddr_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, op2_subreg_flag());
                        regs::rp.w0 += src_size;
                        alu.b0 = regs::ri.b0;
                        alu.b1 = src_size;
                        alu.b2 = op1_subreg_size();
                        run_alu();
                        break;
                    }

                    case instr::cmpind_regVal_regAddr:
                    case instr::testind_regVal_regAddr:
                    {
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_subreg_size();
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regaddr_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, op2_subreg_flag());
                        alu.b0 = regs::ri.b0;
                        alu.b1 = src_size;
                        alu.b2 = op1_subreg_size();
                        run_alu();
                        break;
                    }

                    case instr::cmpxchg_regVal_regreg: {
                        regs::rp.w0 += 3;

                        if (cmp_regval_reg(op3_reg(), op3_subreg_flag(), op2_reg(), op2_subreg_flag())) {
                            zero_flag = 1;
                            copy_regval_reg(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        }
                        else {
                            zero_flag = 0;
                            copy_regval_reg(op2_reg(), op2_subreg_flag(), op3_reg(), op3_subreg_flag());
                        }

                        break;
                    }

                    case instr::cmpxchg_regAddr_regreg: {
                        regs::rp.w0 += 3;

                        if (cmp_regval_reg(op3_reg(), op3_subreg_flag(), op2_reg(), op2_subreg_flag())) {
                            zero_flag = 1;
                            copy_regaddr_reg(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        }
                        else {
                            zero_flag = 0;
                            copy_regval_reg(op2_reg(), op2_subreg_flag(), op3_reg(), op3_subreg_flag());
                        }

                        break;
                    }

                    case instr::cmpxchg_immVal_regreg: {
                        regs::rp.w0 += 3;
                        u_byte src_size = op1_imm_size();
                        regs::rp.w0 += src_size;

                        if (cmp_regval_reg(op3_reg(), op3_subreg_flag(), op2_reg(), op2_subreg_flag())) {
                            zero_flag = 1;
                            copy_memval_reg(regs::rp.w0, src_size, op2_reg(), op2_subreg_flag());
                        }
                        else {
                            zero_flag = 0;
                            copy_regval_reg(op2_reg(), op2_subreg_flag(), op3_reg(), op3_subreg_flag());
                        }

                        break;
                    }

                    case instr::cmpxchg_immAddr_regreg: {
                        regs::rp.w0 += 3;
                        u_byte src_size = op1_imm_size();
                        regs::rp.w0 += src_size;

                        if (cmp_regval_reg(op3_reg(), op3_subreg_flag(), op2_reg(), op2_subreg_flag())) {
                            zero_flag = 1;
                            copy_memaddr_reg(regs::rp.w0, src_size, op2_reg(), op2_subreg_flag());
                        }
                        else {
                            zero_flag = 0;
                            copy_regval_reg(op2_reg(), op2_subreg_flag(), op3_reg(), op3_subreg_flag());
                        }

                        break;
                    }

                    case instr::lea_regVal_regreg: {
                        regs::rp.w0 += 3;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = instr::add_regVal_reg;
                        alu.b1 = op1_subreg_size();
                        alu.b2 = op2_subreg_size();
                        /* LEA computes an effective address and must not disturb the flags
                           (card maize-4). The add runs through the ALU, so snapshot and restore
                           FL (RF.H0) around it. */
                        u_hword saved_fl = regs::rf.h0;
                        run_alu();
                        regs::rf.h0 = saved_fl;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        break;
                    }

                    case instr::lea_regAddr_regreg: {
                        regs::rp.w0 += 3;
                        u_byte src_size = op1_imm_size();
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = instr::add_regVal_reg;
                        alu.b1 = src_size;
                        alu.b2 = op2_subreg_size();
                        /* LEA is flag-neutral (card maize-4); preserve FL across the ALU add. */
                        u_hword saved_fl = regs::rf.h0;
                        run_alu();
                        regs::rf.h0 = saved_fl;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        break;
                    }

                    case instr::lea_immVal_regreg: {
                        regs::rp.w0 += 3;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = instr::add_immVal_reg;
                        alu.b1 = src_size;
                        alu.b2 = op2_subreg_size();
                        /* LEA is flag-neutral (card maize-4); preserve FL across the ALU add. */
                        u_hword saved_fl = regs::rf.h0;
                        run_alu();
                        regs::rf.h0 = saved_fl;
                        regs::rp.w0 += src_size;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        break;
                    }

                    case instr::lea_immAddr_regreg: {
                        regs::rp.w0 += 3;
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = instr::add_immAddr_reg;
                        alu.b1 = src_size;
                        alu.b2 = op2_subreg_size();
                        /* LEA is flag-neutral (card maize-4); preserve FL across the ALU add. */
                        u_hword saved_fl = regs::rf.h0;
                        run_alu();
                        regs::rf.h0 = saved_fl;
                        regs::rp.w0 += src_size;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        break;
                    }

                    /* Wide multiply (card maize-7): 3-operand src/dst/hi form modeled on LEA, but
                       flag-setting (run_alu leaves C/N/V/Z as computed) and writing BOTH dst (low
                       half, from alu.op2_reg) and hi (high half, from alu.op1_reg). The operation
                       width is dst's subregister (op2_subreg_size); hi is written at that width. */
                    case instr::mulw_regVal_regreg:
                    case instr::umulw_regVal_regreg: {
                        regs::rp.w0 += 3;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::ri.b0;
                        alu.b1 = op1_subreg_size();
                        alu.b2 = op2_subreg_size();
                        run_alu();
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        copy_regval_reg(alu.op1_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        break;
                    }

                    case instr::mulw_immVal_regreg:
                    case instr::umulw_immVal_regreg: {
                        regs::rp.w0 += 3;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::ri.b0;
                        alu.b1 = src_size;
                        alu.b2 = op2_subreg_size();
                        run_alu();
                        regs::rp.w0 += src_size;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        copy_regval_reg(alu.op1_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        break;
                    }

                    case instr::mulw_regAddr_regreg:
                    case instr::umulw_regAddr_regreg: {
                        regs::rp.w0 += 3;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::ri.b0;
                        alu.b1 = op1_subreg_size();
                        alu.b2 = op2_subreg_size();
                        run_alu();
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        copy_regval_reg(alu.op1_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        break;
                    }

                    case instr::mulw_immAddr_regreg:
                    case instr::umulw_immAddr_regreg: {
                        regs::rp.w0 += 3;
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::ri.b0;
                        alu.b1 = src_size;
                        alu.b2 = op2_subreg_size();
                        run_alu();
                        regs::rp.w0 += src_size;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        copy_regval_reg(alu.op1_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        break;
                    }

                    case instr::xchg_opcode: {
                        regs::rp.w0 += 2;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), operand1, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), op1_reg(), op1_subreg_flag());
                        copy_regval_reg(operand1, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    /* OUT / OUTR / IN (card maize-21): all twelve dispatch sites are
                       privileged and route their device access through the single shared
                       find_device helper (via port_write / port_read), which applies the
                       frozen read-0 / write-discard outcome on an unpopulated port instead
                       of the old devices[id] value-initialize-null-then-dereference crash.
                       The privilege gate is at the head of each case: executed with the RF
                       privilege bit clear (user mode) the instruction raises the cause-4
                       privileged-op fault before any device access. */
                    case instr::out_regVal_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte dst_size = op2_imm_size();
                        copy_memval_reg(regs::rp.w0, dst_size, operand2, subreg_enum::w0);
                        port_write(operand2.q0, op1_reg(), op1_subreg_flag());
                        regs::rp.w0 += dst_size;
                        break;
                    }

                    case instr::out_immVal_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, operand1, subreg_enum::w0);
                        regs::rp.w0 += src_size;
                        u_byte dst_size = op2_imm_size();
                        copy_memval_reg(regs::rp.w0, dst_size, operand2, subreg_enum::w0);
                        port_write(operand2.q0, operand1, subreg_enum::w0);
                        regs::rp.w0 += dst_size;
                        break;
                    }

                    /* $94 out_regAddr_imm (card maize-21): write operand1 (the value
                       loaded from the source address at line above) to the device, NOT
                       op1_reg() (the raw source address). This corrects the dead-load
                       defect isolated to this form; the sibling address forms already
                       write operand1. */
                    case instr::out_regAddr_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), operand1, subreg_enum::w0);
                        u_byte dst_size = op2_imm_size();
                        copy_memval_reg(regs::rp.w0, dst_size, operand2, subreg_enum::w0);
                        port_write(operand2.q0, operand1, subreg_enum::w0);
                        regs::rp.w0 += dst_size;
                        break;
                    }

                    case instr::out_immAddr_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, src_size, operand1, subreg_enum::w0);
                        regs::rp.w0 += src_size;
                        u_byte dst_size = op2_imm_size();
                        copy_memval_reg(regs::rp.w0, dst_size, operand2, subreg_enum::w0);
                        port_write(operand2.q0, operand1, subreg_enum::w0);
                        regs::rp.w0 += dst_size;
                        break;
                    }

                    /* OUTR/IN (card maize-10, Decision D6464): mirror OUT's four-case pattern
                       against the same port table, but the port is a register operand (op2
                       for OUTR, op1 for IN) rather than an immediate literal. The port id is
                       always the register's/temp's .q0 field, matching OUT's own
                       immediate-port convention of using only the low 16 bits regardless of
                       the encoded field width. IN's copy direction is device-to-register,
                       the mirror image of OUT's register-to-device direction. */
                    case instr::outr_regVal_reg: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        port_write(op2_reg().q0, op1_reg(), op1_subreg_flag());
                        break;
                    }

                    case instr::outr_immVal_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, operand1, subreg_enum::w0);
                        regs::rp.w0 += src_size;
                        port_write(op2_reg().q0, operand1, subreg_enum::w0);
                        break;
                    }

                    case instr::outr_regAddr_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), operand1, subreg_enum::w0);
                        port_write(op2_reg().q0, operand1, subreg_enum::w0);
                        break;
                    }

                    case instr::outr_immAddr_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, src_size, operand1, subreg_enum::w0);
                        regs::rp.w0 += src_size;
                        port_write(op2_reg().q0, operand1, subreg_enum::w0);
                        break;
                    }

                    case instr::in_regVal_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        port_read(op1_reg().q0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::in_immVal_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, operand1, subreg_enum::w0);
                        regs::rp.w0 += src_size;
                        port_read(operand1.q0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::in_regAddr_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), operand1, subreg_enum::w0);
                        port_read(operand1.q0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::in_immAddr_imm: {
                        if (!privilege_flag) { raise_privileged_op(); }
                        regs::rp.w0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, src_size, operand1, subreg_enum::w0);
                        regs::rp.w0 += src_size;
                        port_read(operand1.q0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::sys_immVal: {
                        regs::rp.w0 += 1;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, operand1, subreg_enum::w0);
                        regs::rp.w0 += src_size;
                        regs::rv.w0 = sys::call(operand1.b0);
                        break;
                    }

                    case instr::sys_regVal: {
                        regs::rp.w0 += 1;
                        regs::rv.w0 = sys::call(op1_reg().b0);
                        break;
                    }

                    case instr::pop_opcode: {
                        regs::rp.w0 += 1;
                        auto src_size = op1_subreg_size();
                        copy_memval_reg(regs::rs.w0, src_size, op1_reg(), op1_subreg_flag());
                        regs::rs.w0 += src_size;
                        break;
                    }

                    case instr::push_regVal: {
                        regs::rp.w0 += 1;
                        u_byte src_size = op1_subreg_size();
                        regs::rs.w0 -= src_size;
                        copy_regval_regaddr(op1_reg(), op1_subreg_flag(), regs::rs, subreg_enum::w0);
                        break;
                    }

                    case instr::push_immVal: {
                        regs::rp.w0 += 1;
                        u_byte src_size = op1_imm_size();
                        regs::rs.w0 -= src_size;
                        copy_memval_regaddr(regs::rp.w0, src_size, regs::rs, subreg_enum::w0);
                        regs::rp.w0 += src_size;
                        break;
                    }

                    case instr::call_regVal: {
                        /* Push the full 64-bit return address, then jump (maize-41). */
                        regs::rp.w0 += 1;                                          // past the register param -> return address
                        regs::rs.w0 -= subreg_size_map[static_cast<size_t>(subreg_enum::w0)];           // 8-byte return slot
                        copy_regval_regaddr(regs::rp, subreg_enum::w0, regs::rs, subreg_enum::w0);
                        copy_regval_reg_zext(op1_reg(), op1_subreg_flag(), regs::rp, subreg_enum::w0);
                        break;
                    }

                    case instr::call_immVal: {
                        /* Read the target immediate at its encoded width (zero-extended), push the
                           full 64-bit return address, then jump (maize-41). */
                        regs::rp.w0 += 1;                                          // past the param byte
                        u_byte src_size = op1_imm_size();
                        reg target;
                        mm.read(regs::rp.w0, target, src_size, 0);
                        regs::rp.w0 += src_size;                                   // PC now at the return address
                        regs::rs.w0 -= subreg_size_map[static_cast<size_t>(subreg_enum::w0)];           // 8-byte return slot
                        copy_regval_regaddr(regs::rp, subreg_enum::w0, regs::rs, subreg_enum::w0);
                        regs::rp.w0 = target.w0;
                        break;
                    }

                    /* CALL indirect (card maize-10, Decision D6463): combines the
                       return-address-push sequence above with the target-resolution logic
                       already in jmp_regAddr/jmp_immAddr (below). */
                    case instr::call_regAddr: {
                        regs::rp.w0 += 1;                                          // past the register param -> return address
                        regs::rs.w0 -= subreg_size_map[static_cast<size_t>(subreg_enum::w0)];           // 8-byte return slot
                        copy_regval_regaddr(regs::rp, subreg_enum::w0, regs::rs, subreg_enum::w0);
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), regs::rp, subreg_enum::w0);
                        break;
                    }

                    case instr::call_immAddr: {
                        /* Read the address-literal at its encoded width, advance PC past it
                           (that's the return address), push the return address, then
                           double-dereference to the actual jump target. */
                        regs::rp.w0 += 1;                                          // past the param byte
                        u_byte src_size = op1_imm_size();
                        reg addr_literal;
                        mm.read(regs::rp.w0, addr_literal, src_size, 0);
                        regs::rp.w0 += src_size;                                   // PC now at the return address
                        regs::rs.w0 -= subreg_size_map[static_cast<size_t>(subreg_enum::w0)];           // 8-byte return slot
                        copy_regval_regaddr(regs::rp, subreg_enum::w0, regs::rs, subreg_enum::w0);
                        reg target;
                        mm.read(addr_literal.w0, target, subreg_size_map[static_cast<size_t>(subreg_enum::w0)], 0);
                        regs::rp.w0 = target.w0;
                        break;
                    }

                    case instr::ret_opcode: {
                        /* Pop the full 64-bit return address (maize-41). */
                        u_byte src_size = subreg_size_map[static_cast<size_t>(subreg_enum::w0)];
                        copy_memval_reg(regs::rs.w0, src_size, regs::rp, subreg_enum::w0);
                        regs::rs.w0 += src_size;
                        break;
                    }

                    case instr::iret_opcode: {
                        auto src_size = subreg_size_map[static_cast<size_t>(subreg_enum::w0)];
                        copy_memval_reg(regs::rs.w0, src_size, regs::rf, subreg_enum::w0);
                        regs::rs.w0 += src_size;
                        copy_memval_reg(regs::rs.w0, src_size, regs::rp, subreg_enum::w0);
                        regs::rs.w0 += src_size;
                        break;
                    }

                    /* JMP (card maize-64): always targets the full 64-bit width. The
                       register forms read the whole register (subreg_enum::w0) regardless
                       of any encoded sub-register selection, folding in the full-width role
                       LNGJMP used to play; LNGJMP is removed. */
                    case instr::jmp_regVal: {
                        regs::rp.w0 += 1;
                        copy_regval_reg_zext(op1_reg(), subreg_enum::w0, regs::rp, subreg_enum::w0);
                        break;
                    }

                    case instr::jmp_immVal: {
                        regs::rp.w0 += 1;
                        jump_to_immediate();
                        break;
                    }

                    case instr::jmp_regAddr: {
                        regs::rp.w0 += 1;
                        copy_regaddr_reg(op1_reg(), subreg_enum::w0, regs::rp, subreg_enum::w0);
                        break;
                    }

                    case instr::jmp_immAddr: {
                        regs::rp.w0 += 1;
                        copy_memaddr_reg(regs::rp.w0, op1_imm_size(), regs::rp, subreg_enum::w0);
                        break;
                    }

                    /* Conditional branches (Jcc), card maize-64: IMMEDIATE target only. The
                       ten conditions share three base slots ($17/$18/$19); the condition is
                       decoded from the opcode's row/column bits and evaluated by the shared
                       eval_condition table (the SAME predicate table SETcc uses). On a taken
                       branch the immediate target is loaded into PC; otherwise PC steps over
                       the immediate. */
                    case instr::jz_opcode:
                    case instr::jnz_opcode:
                    case instr::jlt_opcode:
                    case instr::jb_opcode:
                    case instr::jgt_opcode:
                    case instr::ja_opcode:
                    case instr::jge_opcode:
                    case instr::jle_opcode:
                    case instr::jbe_opcode:
                    case instr::jae_opcode:
                    case instr::jp_opcode: {
                        regs::rp.w0 += 1;   // past the operand-descriptor (immediate-size) byte
                        if (eval_condition(decode_condition(instr::jcc_base))) {
                            jump_to_immediate();
                        }
                        else {
                            regs::rp.w0 += op1_imm_size();
                        }
                        break;
                    }

                    /* No-operand carry manipulation (card maize-1). */
                    case instr::setcry_opcode: {
                        carryout_flag = true;
                        break;
                    }

                    case instr::clrcry_opcode: {
                        carryout_flag = false;
                        break;
                    }

                    /* No-operand interrupt-enable manipulation (card maize-10, Decision D6461):
                       pure set/clear of interrupt_enabled_flag, mirroring setcry_opcode/
                       clrcry_opcode above exactly. No interrupt-vector-table or delivery work
                       included (Open Question O1). */
                    case instr::setint_opcode: {
                        interrupt_enabled_flag = true;
                        break;
                    }

                    case instr::clrint_opcode: {
                        interrupt_enabled_flag = false;
                        break;
                    }

                    case instr::nop_opcode: {
                        /* Do nothing. */
                        break;
                    }

                    /* BRK (card maize-78, Open Question O7, superseding maize-10 Decision
                       D6460): a defined breakpoint trap, NOT a no-op. tick() has already
                       advanced regs::rp.w0 past this single-byte opcode, so the captured
                       following-instruction PC (trap class) is already in place. With no
                       handler installed (the maize-21 vector table does not exist yet) the
                       trap halts the VM deterministically with the breakpoint cause
                       surfaced, through the same mechanism raise_divide_error uses. */
                    case instr::brk_opcode: {
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
                    case instr::fadd_regVal_reg:
                    case instr::fsub_regVal_reg:
                    case instr::fmul_regVal_reg:
                    case instr::fdiv_regVal_reg: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FP destination subregister must be H0/H1 (binary32) or W0 (binary64)");
                        u_byte sw = fp_width_from_subreg(op1_subreg_flag());
                        if (!sw) raise_illegal_fp("FP source subregister must be H0/H1 or W0");
                        if (sw != w) raise_illegal_fp("FP operands must be the same width (binary32 with binary32, binary64 with binary64)");
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::ri.b0;
                        alu.b2 = w;
                        run_fpu_arith();
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    /* 3a. Arithmetic, immediate-value source (raw float bits). */
                    case instr::fadd_immVal_reg:
                    case instr::fsub_immVal_reg:
                    case instr::fmul_immVal_reg:
                    case instr::fdiv_immVal_reg: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FP destination subregister must be H0/H1 (binary32) or W0 (binary64)");
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::ri.b0;
                        alu.b2 = w;
                        run_fpu_arith();
                        regs::rp.w0 += src_size;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    /* 3a. Arithmetic, register-address source (load then operate). */
                    case instr::fadd_regAddr_reg:
                    case instr::fsub_regAddr_reg:
                    case instr::fmul_regAddr_reg:
                    case instr::fdiv_regAddr_reg: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FP destination subregister must be H0/H1 (binary32) or W0 (binary64)");
                        {
                            u_word addr = read_subreg_bits(op1_reg(), op1_subreg_flag());
                            reg tmp; tmp.w0 = 0;
                            mm.read(addr, tmp, w, 0);
                            alu.op1_reg.w0 = tmp.w0;
                        }
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::ri.b0;
                        alu.b2 = w;
                        run_fpu_arith();
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    /* 3a. Arithmetic, immediate-address source. */
                    case instr::fadd_immAddr_reg:
                    case instr::fsub_immAddr_reg:
                    case instr::fmul_immAddr_reg:
                    case instr::fdiv_immAddr_reg: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FP destination subregister must be H0/H1 (binary32) or W0 (binary64)");
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::ri.b0;
                        alu.b2 = w;
                        run_fpu_arith();
                        regs::rp.w0 += src_size;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    /* 3e. FCMP, all four addressing-mode source forms. op2 is `a`
                       (the register compared), op1/immediate is `src`. */
                    case instr::fcmp_regVal_reg: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FCMP operand subregister must be H0/H1 or W0");
                        u_byte sw = fp_width_from_subreg(op1_subreg_flag());
                        if (!sw) raise_illegal_fp("FCMP operand subregister must be H0/H1 or W0");
                        if (sw != w) raise_illegal_fp("FCMP operands must be the same width (binary32 with binary32, binary64 with binary64)");
                        u_word src = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        u_word a = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        do_fcmp(a, src, w);
                        break;
                    }

                    case instr::fcmp_immVal_reg: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FCMP operand subregister must be H0/H1 or W0");
                        u_byte src_size = op1_imm_size();
                        reg tmp; tmp.w0 = 0;
                        mm.read(regs::rp.w0, tmp, src_size, 0);
                        u_word a = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        do_fcmp(a, tmp.w0, w);
                        regs::rp.w0 += src_size;
                        break;
                    }

                    case instr::fcmp_regAddr_reg: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FCMP operand subregister must be H0/H1 or W0");
                        u_word addr = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        reg tmp; tmp.w0 = 0;
                        mm.read(addr, tmp, w, 0);
                        u_word a = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        do_fcmp(a, tmp.w0, w);
                        break;
                    }

                    case instr::fcmp_immAddr_reg: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FCMP operand subregister must be H0/H1 or W0");
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0);
                        u_word a = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        do_fcmp(a, alu.op1_reg.w0, w);
                        regs::rp.w0 += src_size;
                        break;
                    }

                    /* 3b. Unary register-only: FSQRT/FNEG/FABS. op1 = src, op2 = dst
                       (dst = f(src)). FNEG/FABS are exact sign-bit ops (no flags, no
                       rounding, NaN payloads preserved). */
                    case instr::fsqrt_opcode:
                    case instr::fneg_opcode:
                    case instr::fabs_opcode: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FP destination subregister must be H0/H1 or W0");
                        u_byte sw = fp_width_from_subreg(op1_subreg_flag());
                        if (!sw) raise_illegal_fp("FP source subregister must be H0/H1 or W0");
                        if (sw != w) raise_illegal_fp("FP operands must be the same width (binary32 with binary32, binary64 with binary64)");
                        u_word src = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        fpu::fresult res;
                        switch (regs::ri.b0) {
                            case instr::fsqrt_opcode: res = fpu::fp_sqrt(src, w, fp_checked_frm()); break;
                            case instr::fneg_opcode:  res = fpu::fp_neg(src, w); break;
                            default:                  res = fpu::fp_abs(src, w); break;
                        }
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op2_reg(), op2_subreg_flag(), res.bits);
                        break;
                    }

                    /* 3d. Min/max register-only: FMIN/FMAX. op1 = src, op2 = dst
                       (dst = min/max(dst, src)). */
                    case instr::fmin_opcode:
                    case instr::fmax_opcode: {
                        regs::rp.w0 += 2;
                        u_byte w = fp_width_from_subreg(op2_subreg_flag());
                        if (!w) raise_illegal_fp("FP destination subregister must be H0/H1 or W0");
                        u_byte sw = fp_width_from_subreg(op1_subreg_flag());
                        if (!sw) raise_illegal_fp("FP source subregister must be H0/H1 or W0");
                        if (sw != w) raise_illegal_fp("FP operands must be the same width (binary32 with binary32, binary64 with binary64)");
                        u_word src = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        u_word dst = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        fpu::fresult res = (regs::ri.b0 == instr::fmin_opcode)
                            ? fpu::fp_min(dst, src, w) : fpu::fp_max(dst, src, w);
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op2_reg(), op2_subreg_flag(), res.bits);
                        break;
                    }

                    /* 3f. Conversions register-only. op1 = src, op2 = dst. Widths
                       come from the two subregister fields; the float operand must be
                       H0/H1/W0, the integer operand may be any width. */
                    case instr::fcvtff_opcode: { // float <-> float
                        regs::rp.w0 += 2;
                        u_byte dw = fp_width_from_subreg(op2_subreg_flag());
                        u_byte sw = fp_width_from_subreg(op1_subreg_flag());
                        if (!dw || !sw) raise_illegal_fp("FCVTFF operand subregister must be H0/H1 or W0");
                        u_word src = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        fpu::fresult res = fpu::fp_cvt_ff(src, sw, dw, fp_checked_frm());
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op2_reg(), op2_subreg_flag(), res.bits);
                        break;
                    }

                    case instr::fcvtfs_opcode:   // float -> signed integer
                    case instr::fcvtfu_opcode: { // float -> unsigned integer
                        regs::rp.w0 += 2;
                        u_byte sw = fp_width_from_subreg(op1_subreg_flag());
                        if (!sw) raise_illegal_fp("FCVTFS/FCVTFU source subregister must be H0/H1 or W0");
                        u_byte dw = op2_subreg_size(); // integer dst: any width
                        u_word src = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        bool is_signed = (regs::ri.b0 == instr::fcvtfs_opcode);
                        fpu::fresult res = fpu::fp_cvt_f_to_int(src, sw, dw, is_signed, fp_checked_frm());
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op2_reg(), op2_subreg_flag(), res.bits);
                        break;
                    }

                    case instr::fcvtsf_opcode:   // signed integer -> float
                    case instr::fcvtuf_opcode: { // unsigned integer -> float
                        regs::rp.w0 += 2;
                        u_byte dw = fp_width_from_subreg(op2_subreg_flag());
                        if (!dw) raise_illegal_fp("FCVTSF/FCVTUF destination subregister must be H0/H1 or W0");
                        u_byte sw = op1_subreg_size(); // integer src: any width
                        u_word src = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        bool is_signed = (regs::ri.b0 == instr::fcvtsf_opcode);
                        fpu::fresult res = fpu::fp_cvt_int_to_f(src, sw, dw, is_signed, fp_checked_frm());
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op2_reg(), op2_subreg_flag(), res.bits);
                        break;
                    }

                    /* 3c. FMA: op1 = a (flagged src), op2 = b, op3 = c (also the
                       destination accumulator): c = a*b (+/-) c, single-rounded. The
                       spec's 4-name FMADD dst,a,b,c collapses to a 3-operand multiply-
                       accumulate under the MULW-shaped encoding (op3 is both the
                       addend c and the destination dst). FNMADD/FNMSUB are synthesized
                       via the exact FNEG (not primitives). */
                    case instr::fmadd_regVal_regreg:
                    case instr::fmsub_regVal_regreg: {
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
                        bool sub = ((regs::ri.b0 & arithmetic_logic_unit::opflag_code) == instr::fmsub_opcode);
                        fpu::fresult res = fpu::fp_fma(a, b, c, w, fp_checked_frm(), sub);
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op3_reg(), op3_subreg_flag(), res.bits);
                        break;
                    }

                    case instr::fmadd_immVal_regreg:
                    case instr::fmsub_immVal_regreg: {
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
                        mm.read(regs::rp.w0, tmp, src_size, 0);
                        u_word a = tmp.w0;
                        u_word b = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        u_word c = read_subreg_bits(op3_reg(), op3_subreg_flag());
                        bool sub = ((regs::ri.b0 & arithmetic_logic_unit::opflag_code) == instr::fmsub_opcode);
                        fpu::fresult res = fpu::fp_fma(a, b, c, w, fp_checked_frm(), sub);
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op3_reg(), op3_subreg_flag(), res.bits);
                        regs::rp.w0 += src_size;
                        break;
                    }

                    case instr::fmadd_regAddr_regreg:
                    case instr::fmsub_regAddr_regreg: {
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
                        mm.read(addr, tmp, w, 0);
                        u_word a = tmp.w0;
                        u_word b = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        u_word c = read_subreg_bits(op3_reg(), op3_subreg_flag());
                        bool sub = ((regs::ri.b0 & arithmetic_logic_unit::opflag_code) == instr::fmsub_opcode);
                        fpu::fresult res = fpu::fp_fma(a, b, c, w, fp_checked_frm(), sub);
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op3_reg(), op3_subreg_flag(), res.bits);
                        break;
                    }

                    case instr::fmadd_immAddr_regreg:
                    case instr::fmsub_immAddr_regreg: {
                        regs::rp.w0 += 3;
                        u_byte w = fp_width_from_subreg(op3_subreg_flag());
                        if (!w) raise_illegal_fp("FMA destination subregister must be H0/H1 or W0");
                        {
                            u_byte bw = fp_width_from_subreg(op2_subreg_flag());
                            if (!bw) raise_illegal_fp("FMA multiplicand subregister must be H0/H1 or W0");
                            if (bw != w) raise_illegal_fp("FMA operands must be the same width (binary32 with binary32, binary64 with binary64)");
                        }
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::rp.w0, src_size, alu.op1_reg, subreg_enum::w0);
                        u_word a = alu.op1_reg.w0;
                        u_word b = read_subreg_bits(op2_reg(), op2_subreg_flag());
                        u_word c = read_subreg_bits(op3_reg(), op3_subreg_flag());
                        bool sub = ((regs::ri.b0 & arithmetic_logic_unit::opflag_code) == instr::fmsub_opcode);
                        fpu::fresult res = fpu::fp_fma(a, b, c, w, fp_checked_frm(), sub);
                        if (res.flags) fcsr_raise(res.flags);
                        write_subreg_bits(op3_reg(), op3_subreg_flag(), res.bits);
                        regs::rp.w0 += src_size;
                        break;
                    }

                    /* FCSR access (card maize-122). FGETCSR dst: dst = FCSR (the
                       whole 8-bit FRM+FFLAGS byte). FSETCSR src: FCSR = src (low 8
                       bits; the upper reserved trap-enable region stays 0 in v1.0). */
                    case instr::fgetcsr_opcode: {
                        regs::rp.w0 += 1;
                        write_subreg_bits(op1_reg(), op1_subreg_flag(),
                            static_cast<u_word>(static_cast<u_byte>(regs::fcsr.b0)));
                        break;
                    }

                    case instr::fsetcsr_opcode: {
                        regs::rp.w0 += 1;
                        u_word v = read_subreg_bits(op1_reg(), op1_subreg_flag());
                        regs::fcsr.w0 = v & 0xFF;
                        break;
                    }

                    default: {
                        std::stringstream err {};
                        err << "unknown opcode: " << std::hex << regs::ri.b0;
                        throw std::logic_error(err.str());
                        break;
                    }
                }
            }
        }

        /* Stop the VM: drop out of tick()'s instruction loop (running_flag) and
           out of run()'s power loop (is_power_on) so run() returns rather than
           blocking on int_event.wait(). Shared by the HALT handler and by
           SYS $3C (sys_exit); both file-local flags live in cpu-internal
           anonymous namespaces, so this is the single exported stop primitive. */
        void power_off() {
            running_flag = false;
            is_power_on = false;
        }

        void run() {
            {
                std::lock_guard<std::mutex> lk(int_mutex);
                is_power_on = true;
            }

            privilege_flag = true;
            int_event.notify_all();

            while (is_power_on) {
                tick();

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

                           No shipped instruction parks with power on: HALT calls
                           power_off(), which clears is_power_on, so the machine exits
                           rather than waiting. This park is therefore currently
                           unreachable; it is written to be correct if a wait-for-interrupt
                           HALT (which keeps power on) is added later. */
                        while (is_power_on && !(irq_pending && interrupt_enabled_flag)) {
                            int_event.wait(lk);
                        }

                        if (is_power_on) {
                            u_byte vector = irq_pending_vector;
                            irq_pending = false;
                            interrupt_set_flag = false;
                            lk.unlock();
                            deliver_vectored(vector, 0, 0, regs::rp.w0);
                            running_flag = true;
                        }
                    }
                }
            }
        }

    } // namespace cpu; 
} // namespace maize
