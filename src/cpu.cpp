#include "maize_cpu.h"
#include "maize_cpu.h"
#include "maize_sys.h"
#include <unordered_map>
#include <sstream>
#include <exception>

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

            /* Maps the value of a subreg_enum to the size, in bytes, of the corresponding subregister. */
            std::unordered_map<subreg_enum, u_byte> subreg_size_map {
                {subreg_enum::b0, 1},
                {subreg_enum::b1, 1},
                {subreg_enum::b2, 1},
                {subreg_enum::b3, 1},
                {subreg_enum::b4, 1},
                {subreg_enum::b5, 1},
                {subreg_enum::b6, 1},
                {subreg_enum::b7, 1},
                {subreg_enum::q0, 2},
                {subreg_enum::q1, 2},
                {subreg_enum::q2, 2},
                {subreg_enum::q3, 2},
                {subreg_enum::h0, 4},
                {subreg_enum::h1, 4},
                {subreg_enum::w0, 8}
            };

            /* Maps the value of a subreg_enum to the offset of the corresponding subregister in the register's 64-bit value. */
            std::unordered_map<subreg_enum, maize::u_byte> subreg_offset_map {
                {subreg_enum::b0,  0}, // b0
                {subreg_enum::b1,  8}, // b1
                {subreg_enum::b2, 16}, // b2
                {subreg_enum::b3, 24}, // b3
                {subreg_enum::b4, 32}, // b4
                {subreg_enum::b5, 40}, // b5
                {subreg_enum::b6, 48}, // b6
                {subreg_enum::b7, 56}, // b7
                {subreg_enum::q0,  0}, // q0
                {subreg_enum::q1, 16}, // q1
                {subreg_enum::q2, 32}, // q2
                {subreg_enum::q3, 48}, // q3
                {subreg_enum::h0,  0}, // h0
                {subreg_enum::h1, 32}, // h1
                {subreg_enum::w0,  0}  // w0
            };

            std::unordered_map<subreg_enum, maize::u_byte> subreg_index_map {
                {subreg_enum::b0, 0}, // b0
                {subreg_enum::b1, 1}, // b1
                {subreg_enum::b2, 2}, // b2
                {subreg_enum::b3, 3}, // b3
                {subreg_enum::b4, 4}, // b4
                {subreg_enum::b5, 5}, // b5
                {subreg_enum::b6, 6}, // b6
                {subreg_enum::b7, 7}, // b7
                {subreg_enum::q0, 0}, // q0
                {subreg_enum::q1, 2}, // q1
                {subreg_enum::q2, 4}, // q2
                {subreg_enum::q3, 6}, // q3
                {subreg_enum::h0, 0}, // h0
                {subreg_enum::h1, 4}, // h1
                {subreg_enum::w0, 0}  // w0
            };

            std::unordered_map<maize::u_byte, subreg_enum> imm_size_subreg_mask_map = {
                {1, subreg_enum::b0},
                {1, subreg_enum::b1},
                {1, subreg_enum::b2},
                {1, subreg_enum::b3},
                {1, subreg_enum::b4},
                {1, subreg_enum::b5},
                {1, subreg_enum::b6},
                {1, subreg_enum::b7},
                {2, subreg_enum::q0},
                {2, subreg_enum::q1},
                {2, subreg_enum::q2},
                {2, subreg_enum::q3},
                {4, subreg_enum::h0},
                {4, subreg_enum::h1},
                {8, subreg_enum::w0}
            };

            /* Maps an instruction's immediate-size flag to a subreg_enum for the subregister that will contain the immediate value. */
            std::unordered_map <size_t, subreg_enum> imm_size_subreg_map = {
                {1, subreg_enum::b0}, // flag is 0x00 for 1-byte immediate size
                {2, subreg_enum::q0}, // flag is 0x01 for 2-byte immediate size
                {4, subreg_enum::h0}, // flag is 0x02 for 4-byte immediate size
                {8, subreg_enum::w0}  // flag is 0x03 for 8-byte immediate size
            };

            /* Maps a subreg_enum value to a mask for the value of that register.  */
            std::unordered_map<subreg_enum, subreg_mask_enum> subreg_mask_map {
                {subreg_enum::b0, subreg_mask_enum::b0},
                {subreg_enum::b1, subreg_mask_enum::b1},
                {subreg_enum::b2, subreg_mask_enum::b2},
                {subreg_enum::b3, subreg_mask_enum::b3},
                {subreg_enum::b4, subreg_mask_enum::b4},
                {subreg_enum::b5, subreg_mask_enum::b5},
                {subreg_enum::b6, subreg_mask_enum::b6},
                {subreg_enum::b7, subreg_mask_enum::b7},
                {subreg_enum::q0, subreg_mask_enum::q0},
                {subreg_enum::q1, subreg_mask_enum::q1},
                {subreg_enum::q2, subreg_mask_enum::q2},
                {subreg_enum::q3, subreg_mask_enum::q3},
                {subreg_enum::h0, subreg_mask_enum::h0},
                {subreg_enum::h1, subreg_mask_enum::h1},
                {subreg_enum::w0, subreg_mask_enum::w0}
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
        
            template <typename T>
            static constexpr bool is_mul_overflow(const T &a, const T &b) {
                return ((b >= 0) && (a >= 0) && (a > std::numeric_limits<T>::max() / b))
                    || ((b < 0) && (a < 0) && (a < std::numeric_limits<T>::max() / b));
            }

            template <typename T>
            static constexpr bool is_mul_underflow(const T &a, const T &b) {
                return ((b >= 0) && (a < 0) && (a < std::numeric_limits<T>::min() / b))
                    || ((b < 0) && (a >= 0) && (a > std::numeric_limits<T>::min() / b));
            }

        }

        u_hword memory_module::write_byte(reg_value address, u_byte value) {
            set_cache_address(address);
            cache[cache_address.b0] = value;
            return sizeof(u_byte);
        }

        u_hword memory_module::write_qword(reg_value address, u_qword value) {
            u_byte b {0};
            set_cache_address(address);

            b = value & 0xff;
            cache[cache_address.b0] = b;
            ++cache_address.b0;

            value >>= 0x08;
            b = value & 0xff;
            cache[cache_address.b0] = b;
            ++cache_address.b0;

            return sizeof(u_qword);
        }

        u_hword memory_module::write_hword(reg_value address, u_hword value) {
            u_byte b {0};
            set_cache_address(address);

            b = value & 0xff;
            cache[cache_address.b0] = b;
            ++cache_address.b0;

            value >>= 0x08;
            b = value & 0xff;
            cache[cache_address.b0] = b;
            ++cache_address.b0;

            value >>= 0x08;
            b = value & 0xff;
            cache[cache_address.b0] = b;
            ++cache_address.b0;

            value >>= 0x08;
            b = value & 0xff;
            cache[cache_address.b0] = b;
            ++cache_address.b0;

            return sizeof(u_hword);
        }

        u_hword memory_module::write_word(reg_value address, u_word value) {
            u_byte b {0};
            set_cache_address(address);

            b = value & 0xff;
            cache[cache_address.b0] = b;
            ++cache_address.b0;

            value >>= 0x08;
            b = value & 0xff;
            cache[cache_address.b0] = b;
            ++cache_address.b0;

            value >>= 0x08;
            b = value & 0xff;
            cache[cache_address.b0] = b;
            ++cache_address.b0;

            value >>= 0x08;
            b = value & 0xff;
            cache[cache_address.b0] = b;
            ++cache_address.b0;

            value >>= 0x08;
            b = value & 0xff;
            cache[cache_address.b0] = b;
            ++cache_address.b0;

            value >>= 0x08;
            b = value & 0xff;
            cache[cache_address.b0] = b;
            ++cache_address.b0;

            value >>= 0x08;
            b = value & 0xff;
            cache[cache_address.b0] = b;
            ++cache_address.b0;

            value >>= 0x08;
            b = value & 0xff;
            cache[cache_address.b0] = b;
            ++cache_address.b0;

            return sizeof(u_word);
        }

        size_t memory_module::read(reg_value address, u_hword count, std::vector<u_byte> &retval) {
            size_t read_count {0};

            if (count) {
                retval.clear();
                retval.reserve(count);

                do {
                    auto rem = set_cache_address(address);
                    size_t idx = cache_address.b0;

                    if (rem >= count) {
                        while (count && idx <= 0xFF) {
                            retval.push_back(cache[idx]);
                            ++idx;
                            --count;
                            ++read_count;
                        }
                    }
                    else {
                        while (count) {
                            retval.push_back(cache[cache_address.b0]);
                            set_cache_address(++address.h0);
                            --count;
                            ++read_count;
                        }
                    }

                } while (count);
            }

            return read_count;
        }

        size_t memory_module::read(u_word address, reg_value &reg, subreg_enum subreg) {
            auto count = subreg_size_map[subreg];
            size_t dst_idx {subreg_index_map[subreg]};
            return read(address, reg, count, dst_idx);
        }

        size_t memory_module::read(reg_value const &address, reg_value &reg, subreg_enum subreg) {
            auto count = subreg_size_map[subreg];
            size_t dst_idx {subreg_index_map[subreg]};
            return read(address.w0, reg, count, dst_idx);
        }

        size_t memory_module::read(reg_value const &address, reg_value &reg, size_t count, size_t dst_idx) {
            return read(address.w0, reg, count, dst_idx);
        }

        size_t memory_module::read(u_word address, reg_value &reg, size_t count, size_t dst_idx) {
            size_t read_count {0};

            do {
                size_t rem {set_cache_address(address)};
                size_t idx {cache_address.b0};

                if (rem >= count) {
                    while (count && idx <= 0xFF) {
                        reg[dst_idx] = cache[idx];
                        ++idx;
                        ++dst_idx;
                        --count;
                        ++read_count;
                    }
                }
                else {
                    while (count) {
                        reg[dst_idx] = cache[cache_address.b0];
                        set_cache_address(++address);
                        ++dst_idx;
                        --count;
                        ++read_count;
                    }
                }

            } while (count);

            return read_count;
        }

        std::vector<u_byte> memory_module::read(reg_value address, u_hword count) {
            std::vector<u_byte> retval;

            if (count) {
                retval.reserve(count);

                do {
                    auto rem = set_cache_address(address);
                    size_t idx = cache_address.b0;

                    if (rem >= count) {
                        while (count && idx <= 0xFF) {
                            retval.push_back(cache[idx]);
                            ++idx;
                            --count;
                        }
                    }
                    else {
                        while (count) {
                            retval.push_back(cache[cache_address.b0]);
                            set_cache_address(++address.h0);
                            --count;
                        }
                    }

                } while (count);
            }

            return retval;
        }

        u_byte memory_module::read_byte(u_word address) {
            u_byte retval {};

            auto rem = set_cache_address(address);
            size_t idx = cache_address.b0;

            if (rem >= 1) {
                if (idx <= 0xFF) {
                    retval = cache[idx];
                }
            }
            else {
                retval = cache[cache_address.b0];
                set_cache_address(++address);
            }

            return retval;
        }

        u_word memory_module::last_block() const {
            auto last = memory_map.rbegin()->first;
            return last;
        }

        size_t memory_module::set_cache_address(u_word address) {
            if (cache_base != (address & address_mask)) {
                cache_base = address & address_mask;

                if (!memory_map.contains(cache_base)) {
                    memory_map[cache_base] = new u_byte[block_size] {0};
                }

                cache = memory_map[cache_base];
            }

            cache_address.w0 = address;
            return block_size - cache_address.b0;
        }

        void reg::increment(u_byte value, subreg_enum subreg) {
            // increment_array.push_back(std::make_pair(std::make_pair(this, subreg), value));
        }

        void reg::decrement(u_byte value, subreg_enum subreg) {
            // increment_array.push_back(std::make_pair(std::make_pair(this, subreg), -value));
        }

        namespace regs {
            // The CPU's general registers are defined here
            reg a;
            reg b;
            reg c;
            reg d;
            reg e;
            reg g;
            reg h;
            reg j;
            reg k;
            reg l;
            reg m;
            reg z;
            reg f; // flags register
            reg in; // instruction register
            reg p {0x0000000000000000}; // program execution register
            reg s; // stack register
        }

        namespace {
            flag<bit_carryout> carryout_flag {regs::f};
            flag<bit_negative> negative_flag {regs::f};
            flag<bit_overflow> overflow_flag {regs::f};
            flag<bit_parity> parity_flag {regs::f};
            flag<bit_zero> zero_flag {regs::f};
            flag<bit_sign> sign_flag {regs::f};
            flag<bit_privilege> privilege_flag {regs::f};
            flag<bit_interrupt_enabled> interrupt_enabled_flag {regs::f};
            flag<bit_interrupt_set> interrupt_set_flag {regs::f};
            flag<bit_running> running_flag {regs::f};

            /* Map an instruction's register-flag nybble value to an actual register reference. */
            std::array<reg*, 16> reg_map {
                &regs::a,
                &regs::b,
                &regs::c,
                &regs::d,
                &regs::e,
                &regs::g,
                &regs::h,
                &regs::j,
                &regs::k,
                &regs::l,
                &regs::m,
                &regs::z,
                &regs::f,
                &regs::in,
                &regs::p,
                &regs::s
            };

            size_t op1_imm_size_flag() {
                return regs::in.b1 & opflag_imm_size;
            }

            u_byte op1_imm_size() {
                return u_byte(1) << (regs::in.b1 & opflag_imm_size);
            }

            u_byte op1_reg_flag() {
                return regs::in.b1 & opflag_reg;
            }

            u_byte op1_reg_index() {
                return (regs::in.b1 & opflag_reg) >> 4;
            }

            u_byte op1_subreg_index() {
                return regs::in.b1 & opflag_subreg;
            }

            subreg_enum op1_subreg_flag() {
                return static_cast<subreg_enum>(regs::in.b1 & opflag_subreg);
            }

            u_byte op1_subreg_size() {
                return subreg_size_map[static_cast<subreg_enum>(regs::in.b1 & opflag_subreg)];
            }

            reg& op1_reg() {
                return *reg_map[(regs::in.b1 & opflag_reg) >> 4];
            }

            u_byte op2_imm_size_flag() {
                return regs::in.b2 & opflag_imm_size;
            }

            u_byte op2_imm_size() {
                return 1 << (regs::in.b2 & opflag_imm_size);
            }

            u_byte op2_reg_flag() {
                return regs::in.b2 & opflag_reg;
            }

            u_byte op2_reg_index() {
                return (regs::in.b2 & opflag_reg) >> 4;
            }

            u_byte op2_subreg_index() {
                return regs::in.b2 & opflag_subreg;
            }

            subreg_enum op2_subreg_flag() {
                return static_cast<subreg_enum>(regs::in.b2 & opflag_subreg);
            }

            u_byte op2_subreg_size() {
                return subreg_size_map[static_cast<subreg_enum>(regs::in.b2 & opflag_subreg)];
            }

            reg& op2_reg() {
                return *reg_map[(regs::in.b2 & opflag_reg) >> 4];
            }

            u_byte op3_reg_flag() {
                return regs::in.b3 & opflag_reg;
            }

            u_byte op3_reg_index() {
                return (regs::in.b3 & opflag_reg) >> 4;
            }

            u_byte op3_subreg_index() {
                return regs::in.b3 & opflag_subreg;
            }

            subreg_enum op3_subreg_flag() {
                return static_cast<subreg_enum>(regs::in.b3 & opflag_subreg);
            }

            u_byte op3_subreg_size() {
                return subreg_size_map[static_cast<subreg_enum>(regs::in.b3 & opflag_subreg)];
            }

            reg &op3_reg() {
                return *reg_map[(regs::in.b3 & opflag_reg) >> 4];
            }

            subreg_enum pc_src_imm_subreg_flag() {
                return imm_size_subreg_map[op1_imm_size_flag()];
            }

            subreg_enum pc_dst_imm_subreg_flag() {
                return imm_size_subreg_map[op2_imm_size_flag()];
            }

            void clr_reg(reg_value &dst, subreg_enum dst_subreg) {
                auto dst_offset = subreg_offset_map[dst_subreg];
                auto dst_mask = subreg_mask_map[dst_subreg];

                u_word src_value = 0;
                zero_flag = true;
                dst.w0 = (~static_cast<u_word>(dst_mask) & dst.w0) | (src_value << dst_offset) & static_cast<u_word>(dst_mask);
            }

            bool cmp_regval_reg(reg_value const &src, subreg_enum src_subreg, reg_value &dst, subreg_enum dst_subreg) {
                auto src_offset = subreg_offset_map[src_subreg];
                auto src_mask = subreg_mask_map[src_subreg];

                auto dst_offset = subreg_offset_map[dst_subreg];
                auto dst_mask = subreg_mask_map[dst_subreg];

                u_word src_value = (src.w0 & static_cast<u_word>(src_mask)) >> src_offset;
                u_word dst_value = (dst.w0 & static_cast<u_word>(dst_mask)) >> dst_offset;

                return src_value == dst_value;
            }

            /* TODO: Maybe I should update flags in all the copy functions... */
            /*
                zero_flag = src_value == 0;
                negative_flag = src_value & (negative_bit_for_size);
                overflow_flag = src_value < dst_value;
            */

            void copy_regval_reg(reg_value const &src, subreg_enum src_subreg, reg_value &dst, subreg_enum dst_subreg) {
                auto src_offset = subreg_offset_map[src_subreg];
                auto src_mask = subreg_mask_map[src_subreg];
                
                auto dst_offset = subreg_offset_map[dst_subreg];
                auto dst_mask = subreg_mask_map[dst_subreg];

                u_word src_value = (src.w0 & static_cast<u_word>(src_mask)) >> src_offset;

                negative_flag = (src_value & subreg_sign_bit[static_cast<int>(src_subreg)]) != 0;

                if (negative_flag) {
                    src_value |= subreg_neg_bits[static_cast<int>(src_subreg)];
                }

                dst.w0 = (~static_cast<u_word>(dst_mask) & dst.w0) | (src_value << dst_offset) & static_cast<u_word>(dst_mask);
                zero_flag = ((src_value << dst_offset) & static_cast<u_word>(dst_mask)) == 0;
                // overflow_flag = src_value > max value for size;
            }

            void copy_regval_reg_zext(reg_value const &src, subreg_enum src_subreg, reg_value &dst, subreg_enum dst_subreg) {
                auto src_offset = subreg_offset_map[src_subreg];
                auto src_mask = subreg_mask_map[src_subreg];

                auto dst_offset = subreg_offset_map[dst_subreg];
                auto dst_mask = subreg_mask_map[dst_subreg];

                u_word src_value = (src.w0 & static_cast<u_word>(src_mask)) >> src_offset;

                dst.w0 = (~static_cast<u_word>(dst_mask) & dst.w0) | (src_value << dst_offset) & static_cast<u_word>(dst_mask);
                zero_flag = ((src_value << dst_offset) & static_cast<u_word>(dst_mask)) == 0;
                negative_flag = (dst.w0 & subreg_sign_bit[static_cast<int>(dst_subreg)]) != 0;
                // overflow_flag = src_value > max value for size;
            }

            void copy_memval_reg(u_word address, size_t size, reg_value &op2_reg, subreg_enum dst_subreg) {
                auto dst_index = subreg_index_map[dst_subreg];
                mm.read(address, op2_reg, size, dst_index);
            }

            void copy_regaddr_reg(reg_value const &src, subreg_enum src_subreg, reg_value &dst, subreg_enum dst_subreg) {
                auto src_offset = subreg_offset_map[src_subreg];
                auto src_mask = subreg_mask_map[src_subreg];

                auto dst_offset = subreg_offset_map[dst_subreg];
                auto dst_mask = subreg_mask_map[dst_subreg];

                u_word src_address = (static_cast<u_word>(src_mask) & src.w0) >> src_offset;
                reg src_data;
                mm.read(src_address, src_data, subreg_enum::w0);
                
                dst.w0 = (~static_cast<u_word>(dst_mask) & dst.w0) | (src_data.w0 << dst_offset) & static_cast<u_word>(dst_mask);
                zero_flag = ((src_data.w0 << dst_offset) & static_cast<u_word>(dst_mask)) == 0;
                negative_flag = (dst.w0 & subreg_sign_bit[static_cast<int>(dst_subreg)]) != 0;
                // overflow_flag = dst_value > max value for size;
            }

            void copy_memaddr_reg(u_word address, size_t size, reg_value &dst, subreg_enum dst_subreg) {

                reg src_address;
                mm.read(address, src_address, subreg_enum::w0);

                reg src_data;
                mm.read(src_address.h0, src_data, subreg_enum::w0);

                auto dst_offset = subreg_offset_map[dst_subreg];
                auto dst_mask = subreg_mask_map[dst_subreg];

                dst.w0 = (~static_cast<u_word>(dst_mask) & dst.w0) | (src_data.w0 << dst_offset) & static_cast<u_word>(dst_mask);

                zero_flag = ((src_data.w0 << dst_offset) & static_cast<u_word>(dst_mask)) == 0;
                negative_flag = (dst.w0 & subreg_sign_bit[static_cast<int>(dst_subreg)]) != 0;
                // overflow_flag = dst_value > max value for size;
            }

            void copy_regval_regaddr(reg_value const &src, subreg_enum src_subreg, reg_value const &dst, subreg_enum dst_subreg) {
                auto src_offset = subreg_offset_map[src_subreg];
                auto src_mask = subreg_mask_map[src_subreg];
                auto size = subreg_size_map[src_subreg];

                auto dst_offset = subreg_offset_map[dst_subreg];
                auto dst_mask = subreg_mask_map[dst_subreg];

                reg_value src_data;
                src_data.w0 = static_cast<s_word>((src.w0 & static_cast<u_word>(src_mask)) >> src_offset);
                u_word dst_address = (dst.w0 & static_cast<u_word>(dst_mask)) >> dst_offset;

                zero_flag = src_data == 0;
                negative_flag = (src_data.w0 & subreg_sign_bit[static_cast<int>(src_subreg)]) != 0;

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
                auto dst_offset = subreg_offset_map[dst_subreg];
                auto dst_mask = subreg_mask_map[dst_subreg];

                reg_value src_data;
                mm.read(address, src_data, size, 0);
                u_word dst_address = (dst.w0 & static_cast<u_word>(dst_mask)) >> dst_offset;

                zero_flag = src_data == 0;
                negative_flag = (src_data.w0 & subreg_sign_bit[static_cast<int>(subreg_enum::w0)]) != 0;

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

        void run_alu() {
            u_byte op_size = alu.b2; // Destination size

            switch (alu.b0 & arithmetic_logic_unit::opflag_code) {
                case instr::add_opcode: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0 + alu.op1_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result < alu.op2_reg.b0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0 + alu.op1_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result < alu.op2_reg.q0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0 + alu.op1_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result < alu.op2_reg.h0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 + alu.op1_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result < alu.op2_reg.w0;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::cmp_opcode:
                case instr::cmpind_opcode:
                case instr::sub_opcode: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0 - alu.op1_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result > alu.op2_reg.b0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0 - alu.op1_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result > alu.op2_reg.q0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0 - alu.op1_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result > alu.op2_reg.h0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 - alu.op1_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result > alu.op2_reg.w0;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::mul_opcode: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0 - alu.op1_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = is_mul_overflow(alu.op1_reg.b0, alu.op2_reg.b0) || is_mul_underflow(alu.op1_reg.b0, alu.op2_reg.b0);
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0 - alu.op1_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = is_mul_overflow(alu.op1_reg.q0, alu.op2_reg.q0) || is_mul_underflow(alu.op1_reg.q0, alu.op2_reg.q0);
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0 - alu.op1_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = is_mul_overflow(alu.op1_reg.h0, alu.op2_reg.h0) || is_mul_underflow(alu.op1_reg.h0, alu.op2_reg.h0);
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 - alu.op1_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = is_mul_overflow(alu.op1_reg.h0, alu.op2_reg.h0) || is_mul_underflow(alu.op1_reg.h0, alu.op2_reg.h0);
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::div_opcode: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0 / alu.op1_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0 / alu.op1_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0 / alu.op1_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 / alu.op1_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::mod_opcode: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0 % alu.op1_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0 % alu.op1_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0 % alu.op1_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 % alu.op1_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::test_opcode: 
                case instr::testind_opcode: 
                case instr::and_opcode: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0 & alu.op1_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0 & alu.op1_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0 & alu.op1_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 & alu.op1_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::or_opcode: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0 | alu.op1_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0 | alu.op1_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0 | alu.op1_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 | alu.op1_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::nor_opcode: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            u_byte result = ~(alu.op2_reg.b0 | alu.op1_reg.b0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = ~(alu.op2_reg.q0 | alu.op1_reg.q0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = ~(alu.op2_reg.h0 | alu.op1_reg.h0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = ~(alu.op2_reg.w0 | alu.op1_reg.w0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::nand_opcode: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            u_byte result = ~(alu.op2_reg.b0 & alu.op1_reg.b0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = ~(alu.op2_reg.q0 & alu.op1_reg.q0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = ~(alu.op2_reg.h0 & alu.op1_reg.h0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = ~(alu.op2_reg.w0 & alu.op1_reg.w0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::xor_opcode: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0 ^ alu.op1_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0 ^ alu.op1_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0 ^ alu.op1_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 ^ alu.op1_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::shl_opcode: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0 << alu.op1_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result < alu.op2_reg.b0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0 << alu.op1_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result < alu.op2_reg.q0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0 << alu.op1_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result < alu.op2_reg.h0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 << alu.op1_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result < alu.op2_reg.w0;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::shr_opcode: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0 >> alu.op1_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result < alu.op2_reg.b0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0 >> alu.op1_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result < alu.op2_reg.q0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0 >> alu.op1_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result < alu.op2_reg.h0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 >> alu.op1_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result < alu.op2_reg.w0;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::inc_opcode: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0 + u_byte(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result < alu.op2_reg.b0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0 + u_qword(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result < alu.op2_reg.q0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0 + u_hword(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result < alu.op2_reg.h0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 + u_word(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result < alu.op2_reg.w0;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::dec_opcode: {
                    switch (op_size) {
                        case 1: {
                            u_byte result = alu.op2_reg.b0 - u_byte(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result > alu.op2_reg.b0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = alu.op2_reg.q0 - u_qword(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result > alu.op2_reg.q0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = alu.op2_reg.h0 - u_hword(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result > alu.op2_reg.h0;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = alu.op2_reg.w0 - u_word(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result > alu.op2_reg.w0;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case instr::not_opcode: {
                    overflow_flag = 0;

                    switch (op_size) {
                        case 1: {
                            u_byte result = ~alu.op2_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            u_qword result = ~alu.op2_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            u_hword result = ~alu.op2_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            alu.op2_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            u_word result = ~alu.op2_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            alu.op2_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }
            }
        }

        /* This is the state machine that implements the machine-code instructions. */
        void tick() {
            running_flag = true;

            while (running_flag) {
                /* Decode next instruction */
                mm.read(regs::p.h0, regs::in, subreg_enum::w0);
                ++regs::p.h0;
                run_state = run_states::execute;

                /* Execute instruction */
                switch (regs::in.b0) {
                    case instr::halt_opcode: {
                        running_flag = false;
                        is_power_on = false; // just temporary until I get "device" interaction working
                        break;
                    }

                    case instr::clr_regVal: {
                        regs::p.h0 += 1;
                        clr_reg(op1_reg(), op1_subreg_flag());
                        break;
                    }

                    case instr::ld_regVal_reg: {
                        regs::p.h0 += 2;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::ld_immVal_reg: {
                        regs::p.h0 += 2;
                        u_hword imm_size = op1_imm_size();
                        copy_memval_reg(regs::p.h0, imm_size, op2_reg(), op2_subreg_flag());
                        regs::p.h0 += imm_size;
                        break;
                    }

                    case instr::ld_regAddr_reg: {
                        regs::p.h0 += 2;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::ld_immAddr_reg: {
                        regs::p.h0 += 2;
                        u_hword imm_size = op1_imm_size();
                        u_hword dst_size = op2_subreg_size();
                        copy_memaddr_reg(regs::p.h0, dst_size, op2_reg(), op2_subreg_flag());
                        regs::p.h0 += imm_size;
                        break;
                    }

                    case instr::ldz_regVal_reg: {
                        regs::p.h0 += 2;
                        copy_regval_reg_zext(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::ldz_immVal_reg: {
                        regs::p.h0 += 2;
                        u_hword imm_size = op1_imm_size();
                        copy_memval_reg(regs::p.h0, imm_size, op2_reg(), op2_subreg_flag());
                        regs::p.h0 += imm_size;
                        break;
                    }

                    case instr::ldz_regAddr_reg: {
                        regs::p.h0 += 2;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::ldz_immAddr_reg: {
                        regs::p.h0 += 2;
                        u_hword imm_size = op1_imm_size();
                        u_hword dst_size = op2_subreg_size();
                        copy_memaddr_reg(regs::p.h0, dst_size, op2_reg(), op2_subreg_flag());
                        regs::p.h0 += imm_size;
                        break;
                    }

                    case instr::st_regVal_regAddr: {
                        regs::p.h0 += 2;
                        copy_regval_regaddr(op1_reg(), op1_subreg_flag(), op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::st_immVal_regAddr: {
                        regs::p.h0 += 2;
                        u_hword imm_size = op1_imm_size();
                        copy_memval_regaddr(regs::p.h0, imm_size, op2_reg(), op2_subreg_flag());
                        regs::p.h0 += imm_size;
                        break;
                    }

                    case instr::add_regVal_reg:
                    case instr::sub_regVal_reg:
                    case instr::mul_regVal_reg:
                    case instr::div_regVal_reg:
                    case instr::mod_regVal_reg:
                    case instr::and_regVal_reg:
                    case instr::or_regVal_reg:
                    case instr::nor_regVal_reg:
                    case instr::nand_regVal_reg:
                    case instr::xor_regVal_reg:
                    case instr::shl_regVal_reg:
                    case instr::shr_regVal_reg:
                    case instr::cmp_regVal_reg:
                    case instr::test_regVal_reg: {
                        regs::p.h0 += 2;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::in.b0;
                        alu.b1 = op1_subreg_size();
                        alu.b2 = op2_subreg_size();
                        run_alu();
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::add_immVal_reg:
                    case instr::sub_immVal_reg:
                    case instr::mul_immVal_reg:
                    case instr::div_immVal_reg:
                    case instr::mod_immVal_reg:
                    case instr::and_immVal_reg:
                    case instr::or_immVal_reg:
                    case instr::nor_immVal_reg:
                    case instr::nand_immVal_reg:
                    case instr::xor_immVal_reg:
                    case instr::shl_immVal_reg:
                    case instr::shr_immVal_reg:
                    case instr::cmp_immVal_reg:
                    case instr::test_immVal_reg: {
                        regs::p.h0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::p.h0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::in.b0;
                        alu.b1 = src_size;
                        alu.b2 = op2_subreg_size();
                        run_alu();
                        regs::p.h0 += src_size;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::add_regAddr_reg:
                    case instr::sub_regAddr_reg:
                    case instr::mul_regAddr_reg:
                    case instr::div_regAddr_reg:
                    case instr::mod_regAddr_reg:
                    case instr::and_regAddr_reg:
                    case instr::or_regAddr_reg:
                    case instr::nor_regAddr_reg:
                    case instr::nand_regAddr_reg:
                    case instr::xor_regAddr_reg:
                    case instr::shl_regAddr_reg:
                    case instr::shr_regAddr_reg:
                    case instr::cmp_regAddr_reg:
                    case instr::test_regAddr_reg: {
                        regs::p.h0 += 2;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::in.b0;
                        alu.b1 = op1_subreg_size();
                        alu.b2 = op2_subreg_size();
                        run_alu();
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::add_immAddr_reg:
                    case instr::sub_immAddr_reg:
                    case instr::mul_immAddr_reg:
                    case instr::div_immAddr_reg:
                    case instr::mod_immAddr_reg:
                    case instr::and_immAddr_reg:
                    case instr::or_immAddr_reg:
                    case instr::nor_immAddr_reg:
                    case instr::nand_immAddr_reg:
                    case instr::xor_immAddr_reg:
                    case instr::shl_immAddr_reg:
                    case instr::shr_immAddr_reg:
                    case instr::cmp_immAddr_reg:
                    case instr::test_immAddr_reg: {
                        regs::p.h0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::p.h0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::in.b0;
                        alu.b1 = src_size;
                        alu.b2 = op2_subreg_size();
                        run_alu();
                        regs::p.h0 += src_size;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::inc_regVal:
                    case instr::dec_regVal:
                    case instr::not_regVal: {
                        regs::p.h0 += 1;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = regs::in.b0;
                        alu.b1 = op1_subreg_size();
                        alu.b2 = op1_subreg_size();
                        run_alu();
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op1_reg(), op1_subreg_flag());
                        break;
                    }

                    case instr::cmpind_immVal_regAddr:
                    case instr::testind_immVal_regAddr:
                    {
                        regs::p.h0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::p.h0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regaddr_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, op2_subreg_flag());
                        regs::p.h0 += src_size;
                        alu.b0 = regs::in.b0;
                        alu.b1 = src_size;
                        alu.b2 = op1_subreg_size();
                        run_alu();
                        break;
                    }

                    case instr::cmpind_regVal_regAddr:
                    case instr::testind_regVal_regAddr:
                    {
                        regs::p.h0 += 2;
                        u_byte src_size = op1_subreg_size();
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regaddr_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, op2_subreg_flag());
                        alu.b0 = regs::in.b0;
                        alu.b1 = src_size;
                        alu.b2 = op1_subreg_size();
                        run_alu();
                        break;
                    }

                    case instr::cmpxchg_regVal_regreg: {
                        regs::p.h0 += 3;

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
                        regs::p.h0 += 3;

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
                        regs::p.h0 += 3;
                        u_byte src_size = op1_imm_size();
                        regs::p.h0 += src_size;

                        if (cmp_regval_reg(op3_reg(), op3_subreg_flag(), op2_reg(), op2_subreg_flag())) {
                            zero_flag = 1;
                            copy_memval_reg(regs::p.h0, src_size, op2_reg(), op2_subreg_flag());
                        }
                        else {
                            zero_flag = 0;
                            copy_regval_reg(op2_reg(), op2_subreg_flag(), op3_reg(), op3_subreg_flag());
                        }

                        break;
                    }

                    case instr::cmpxchg_immAddr_regreg: {
                        regs::p.h0 += 3;
                        u_byte src_size = op1_imm_size();
                        regs::p.h0 += src_size;

                        if (cmp_regval_reg(op3_reg(), op3_subreg_flag(), op2_reg(), op2_subreg_flag())) {
                            zero_flag = 1;
                            copy_memaddr_reg(regs::p.h0, src_size, op2_reg(), op2_subreg_flag());
                        }
                        else {
                            zero_flag = 0;
                            copy_regval_reg(op2_reg(), op2_subreg_flag(), op3_reg(), op3_subreg_flag());
                        }

                        break;
                    }

                    case instr::lea_regVal_regreg: {
                        regs::p.h0 += 3;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = instr::add_regVal_reg;
                        alu.b1 = op1_subreg_size();
                        alu.b2 = op2_subreg_size();
                        run_alu();
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        break;
                    }

                    case instr::lea_regAddr_regreg: {
                        regs::p.h0 += 3;
                        u_byte src_size = op1_imm_size();
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = instr::add_regVal_reg;
                        alu.b1 = src_size;
                        alu.b2 = op2_subreg_size();
                        run_alu();
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        break;
                    }

                    case instr::lea_immVal_regreg: {
                        regs::p.h0 += 3;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::p.h0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = instr::add_immVal_reg;
                        alu.b1 = src_size;
                        alu.b2 = op2_subreg_size();
                        run_alu();
                        regs::p.h0 += src_size;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        break;
                    }

                    case instr::lea_immAddr_regreg: {
                        regs::p.h0 += 3;
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::p.h0, src_size, alu.op1_reg, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), alu.op2_reg, subreg_enum::w0);
                        alu.b0 = instr::add_immAddr_reg;
                        alu.b1 = src_size;
                        alu.b2 = op2_subreg_size();
                        run_alu();
                        regs::p.h0 += src_size;
                        copy_regval_reg(alu.op2_reg, subreg_enum::w0, op3_reg(), op3_subreg_flag());
                        break;
                    }

                    case instr::xchg_opcode: {
                        regs::p.h0 += 2;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), operand1, subreg_enum::w0);
                        copy_regval_reg(op2_reg(), op2_subreg_flag(), op1_reg(), op1_subreg_flag());
                        copy_regval_reg(operand1, subreg_enum::w0, op2_reg(), op2_subreg_flag());
                        break;
                    }

                    case instr::out_regVal_imm: {
                        regs::p.h0 += 2;
                        u_byte dst_size = op2_imm_size();
                        copy_memval_reg(regs::p.h0, dst_size, operand2, subreg_enum::w0);
                        device *pdst_dev = devices[operand2.q0];
                        device &dst_dev = *(pdst_dev);
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), dst_dev, subreg_enum::w0);
                        regs::p.h0 += dst_size;
                        break;
                    }

                    case instr::out_immVal_imm: {
                        regs::p.h0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::p.h0, src_size, operand1, subreg_enum::w0);
                        regs::p.h0 += src_size;
                        u_byte dst_size = op2_imm_size();
                        copy_memval_reg(regs::p.h0, dst_size, operand2, subreg_enum::w0);
                        device *pdst_dev = devices[operand2.q0];
                        device &dst_dev = *(pdst_dev);
                        copy_regval_reg(operand1, subreg_enum::w0, dst_dev, subreg_enum::w0);
                        regs::p.h0 += dst_size;
                        break;
                    }

                    case instr::out_regAddr_imm: {
                        regs::p.h0 += 2;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), operand1, subreg_enum::w0);
                        u_byte dst_size = op2_imm_size();
                        copy_memval_reg(regs::p.h0, dst_size, operand2, subreg_enum::w0);
                        device *pdst_dev = devices[operand2.q0];
                        device &dst_dev = *(pdst_dev);
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), dst_dev, subreg_enum::w0);
                        regs::p.h0 += dst_size;
                        break;
                    }

                    case instr::out_immAddr_imm: {
                        regs::p.h0 += 2;
                        u_byte src_size = op1_imm_size();
                        copy_memaddr_reg(regs::p.h0, src_size, operand1, subreg_enum::w0);
                        regs::p.h0 += src_size;
                        u_byte dst_size = op2_imm_size();
                        copy_memval_reg(regs::p.h0, dst_size, operand2, subreg_enum::w0);
                        device *pdst_dev = devices[operand2.q0];
                        device &dst_dev = *(pdst_dev);
                        copy_regval_reg(operand1, subreg_enum::w0, dst_dev, subreg_enum::w0);
                        regs::p.h0 += dst_size;
                        break;
                    }

                    case instr::sys_immVal: {
                        regs::p.h0 += 1;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::p.h0, src_size, operand1, subreg_enum::w0);
                        regs::p.h0 += src_size;
                        regs::a.w0 = sys::call(operand1.b0);
                        break;
                    }

                    case instr::sys_regVal: {
                        regs::p.h0 += 1;
                        regs::a.w0 = sys::call(op1_reg().b0);
                        break;
                    }

                    case instr::pop_regVal: {
                        regs::p.h0 += 1;
                        auto src_size = op1_subreg_size();
                        copy_memval_reg(regs::s.h0, src_size, op1_reg(), op1_subreg_flag());
                        regs::s.h0 += src_size;
                        break;
                    }

                    case instr::push_regVal: {
                        regs::p.h0 += 1;
                        u_byte src_size = op1_subreg_size();
                        regs::s.h0 -= src_size;
                        copy_regval_regaddr(op1_reg(), op1_subreg_flag(), regs::s, subreg_enum::h0);
                        break;
                    }

                    case instr::push_immVal: {
                        regs::p.h0 += 1;
                        u_byte src_size = op1_imm_size();
                        regs::s.h0 -= src_size;
                        copy_memval_regaddr(regs::p.h0, src_size, regs::s, subreg_enum::h0);
                        regs::p.h0 += src_size;
                        break;
                    }

                    case instr::call_regVal: {
                        regs::p.h0 += 1;
                        regs::s.h0 -= subreg_size_map[subreg_enum::h0];
                        copy_regval_regaddr(regs::p, subreg_enum::h0, regs::s, subreg_enum::h0);
                        regs::p.h0 = op1_reg().h0;
                        break;
                    }

                    case instr::call_immVal: {
                        regs::p.h0 += 1;
                        u_byte src_size = op1_imm_size();
                        copy_memval_reg(regs::p.h0, src_size, operand1, subreg_enum::h0);
                        regs::s.h0 -= subreg_size_map[subreg_enum::h0];
                        regs::p.h0 += src_size;
                        copy_regval_regaddr(regs::p, subreg_enum::h0, regs::s, subreg_enum::h0);
                        regs::p.h0 = operand1.h0;
                        break;
                    }

                    case instr::ret_opcode: {
                        u_byte src_size = subreg_size_map[subreg_enum::h0];
                        copy_memval_reg(regs::s.h0, src_size, regs::p, subreg_enum::h0);
                        regs::s.h0 += src_size;
                        break;
                    }

                    case instr::iret_opcode: {
                        auto src_size = subreg_size_map[subreg_enum::w0];
                        copy_memval_reg(regs::s.h0, src_size, regs::f, subreg_enum::w0);
                        regs::s.h0 += src_size;
                        copy_memval_reg(regs::s.h0, src_size, regs::p, subreg_enum::w0);
                        regs::s.h0 += src_size;
                        break;
                    }

                    case instr::jmp_regVal: {
                        regs::p.h0 += 1;
                        copy_regval_reg(op1_reg(), op1_subreg_flag(), regs::p, subreg_enum::h0);
                        break;
                    }

                    case instr::jmp_immVal: {
                        regs::p.h0 += 1;
                        copy_memval_reg(regs::p.h0, subreg_size_map[subreg_enum::h0], regs::p, subreg_enum::h0);
                        break;
                    }

                    case instr::jmp_regAddr: {
                        regs::p.h0 += 1;
                        copy_regaddr_reg(op1_reg(), op1_subreg_flag(), regs::p, subreg_enum::h0);
                        break;
                    }

                    case instr::jmp_immAddr: {
                        regs::p.h0 += 1;
                        copy_memaddr_reg(regs::p.h0, subreg_size_map[subreg_enum::h0], regs::p, subreg_enum::h0);
                        break;
                    }

                    case instr::jz_regVal: {
                        regs::p.h0 += 1;

                        if (cpu::zero_flag) {
                            copy_regval_reg(op1_reg(), op1_subreg_flag(), regs::p, subreg_enum::h0);
                        }

                        break;
                    }

                    case instr::jz_immVal: {
                        regs::p.h0 += 1;

                        if (cpu::zero_flag) {
                            copy_memval_reg(regs::p.h0, subreg_size_map[subreg_enum::h0], regs::p, subreg_enum::h0);
                        }
                        else {
                            u_byte src_size = op1_imm_size();
                            regs::p.h0 += src_size;
                        }

                        break;
                    }

                    case instr::jz_regAddr: {
                        regs::p.h0 += 1;

                        if (cpu::zero_flag) {
                            copy_regaddr_reg(op1_reg(), op1_subreg_flag(), regs::p, subreg_enum::h0);
                        }
                        else {
                            u_byte src_size = op1_imm_size();
                            regs::p.h0 += src_size;
                        }
                        break;
                    }

                    case instr::jz_immAddr: {
                        regs::p.h0 += 1;

                        if (cpu::zero_flag) {
                            copy_memaddr_reg(regs::p.h0, subreg_size_map[subreg_enum::h0], regs::p, subreg_enum::h0);
                        }
                        else {
                            u_byte src_size = op1_imm_size();
                            regs::p.h0 += src_size;
                        }
                        break;
                    }

                    case instr::jnz_regVal: {
                        regs::p.h0 += 1;

                        if (!cpu::zero_flag) {
                            copy_regval_reg(op1_reg(), op1_subreg_flag(), regs::p, subreg_enum::h0);
                        }

                        break;
                    }

                    case instr::jnz_immVal: {
                        regs::p.h0 += 1;

                        if (!cpu::zero_flag) {
                            copy_memval_reg(regs::p.h0, subreg_size_map[subreg_enum::h0], regs::p, subreg_enum::h0);
                        }
                        else {
                            u_byte src_size = op1_imm_size();
                            regs::p.h0 += src_size;
                        }

                        break;
                    }

                    case instr::jnz_regAddr: {
                        regs::p.h0 += 1;

                        if (!cpu::zero_flag) {
                            copy_regaddr_reg(op1_reg(), op1_subreg_flag(), regs::p, subreg_enum::h0);
                        }
                        else {
                            u_byte src_size = op1_imm_size();
                            regs::p.h0 += src_size;
                        }
                        break;
                    }

                    case instr::jnz_immAddr: {
                        regs::p.h0 += 1;

                        if (!cpu::zero_flag) {
                            copy_memaddr_reg(regs::p.h0, subreg_size_map[subreg_enum::h0], regs::p, subreg_enum::h0);
                        }
                        else {
                            u_byte src_size = op1_imm_size();
                            regs::p.h0 += src_size;
                        }
                        break;
                    }

                    case instr::nop_opcode: {
                        /* Do nothing. */
                        break;
                    }

                    default: {
                        std::stringstream err {};
                        err << "unknown opcode: " << std::hex << regs::in.b0;
                        throw std::logic_error(err.str());
                        break;
                    }
                }
            }
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
                        /* Wait for interrupt */
                        int_event.wait(lk);

                        /*
                        If interrupt set
                            look up interrupt handler
                            push regs::p
                            push regs::f
                            regs::p.h1 = interrupt handler segment
                            regs::p.h0 = interrupt handler address
                            running_flag = true
                        */
                    }
                }
            }
        }

    } // namespace cpu; 
} // namespace maize
