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
            const word bit_carryout =           0b0000000000000000000000000000000000000000000000000000000000000001;
            const word bit_negative =           0b0000000000000000000000000000000000000000000000000000000000000010;
            const word bit_overflow =           0b0000000000000000000000000000000000000000000000000000000000000100;
            const word bit_parity =             0b0000000000000000000000000000000000000000000000000000000000001000;
            const word bit_zero =               0b0000000000000000000000000000000000000000000000000000000000010000;
            const word bit_sign =               0b0000000000000000000000000000000000000000000000000000000000100000;
            const word bit_reserved =           0b0000000000000000000000000000000000000000000000000000000001000000;
            const word bit_privilege =          0b0000000000000000000000000000000100000000000000000000000000000000;
            const word bit_interrupt_enabled =  0b0000000000000000000000000000001000000000000000000000000000000000;
            const word bit_interrupt_set =      0b0000000000000000000000000000010000000000000000000000000000000000;
            const word bit_running =            0b0000000000000000000000000000100000000000000000000000000000000000;

            /* Maps the value of a subreg_enum to the size, in bytes, of the corresponding subregister. */
            std::unordered_map<subreg_enum, byte> subreg_size_map {
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
            std::unordered_map<subreg_enum, maize::byte> subreg_offset_map {
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

            std::unordered_map<subreg_enum, maize::byte> subreg_index_map {
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

            const subreg_mask_enum imm_size_subreg_mask_map[] = {
                subreg_mask_enum::b0,
                subreg_mask_enum::q0,
                subreg_mask_enum::h0,
                subreg_mask_enum::w0
            };

            /* Maps an instruction's immediate-size flag to a subreg_enum for the subregister that will contain the immediate value. */
            const subreg_enum imm_size_subreg_map[] = {
                subreg_enum::b0, // flag is 0x00 for 1-byte immediate size
                subreg_enum::q0, // flag is 0x01 for 2-byte immediate size
                subreg_enum::h0, // flag is 0x02 for 4-byte immediate size
                subreg_enum::w0  // flag is 0x03 for 8-byte immediate size
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
                reg_op_info(bus* pbus, reg* preg, subreg_mask_enum mask, byte offset) :
                    pbus(pbus), preg(preg), mask(mask), offset(offset) {
                }
                reg_op_info(const reg_op_info&) = default;
                reg_op_info(reg_op_info&&) = default;
                reg_op_info& operator=(const reg_op_info&) = default;

                bus* pbus {nullptr};
                reg* preg {nullptr};
                subreg_mask_enum mask {subreg_mask_enum::w0};
                byte offset {0};
            };

            std::vector<reg_op_info> bus_enable_array;
            std::vector<reg_op_info> bus_set_array;
            std::vector<std::pair<std::pair<reg*, subreg_enum>, int8_t>> increment_array;
            std::map<qword, device*> devices;
        
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

        hword memory_module::write_byte(reg_value address, byte value) {
            set_cache_address(address);
            cache[cache_address.b0] = value;
            return sizeof(byte);
        }

        hword memory_module::write_qword(reg_value address, qword value) {
            byte b {0};
            set_cache_address(address);

            b = value & 0xff;
            cache[cache_address.b0] = b;
            ++cache_address.b0;

            value >>= 0x08;
            b = value & 0xff;
            cache[cache_address.b0] = b;
            ++cache_address.b0;

            return sizeof(qword);
        }

        hword memory_module::write_hword(reg_value address, hword value) {
            byte b {0};
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

            return sizeof(hword);
        }

        hword memory_module::write_word(reg_value address, word value) {
            byte b {0};
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

            return sizeof(word);
        }

        size_t memory_module::read(reg_value address, hword count, std::vector<byte> &retval) {
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

        size_t memory_module::read(word address, reg_value &reg, subreg_enum subreg) {
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

        size_t memory_module::read(word address, reg_value &reg, size_t count, size_t dst_idx) {
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

        std::vector<byte> memory_module::read(reg_value address, hword count) {
            std::vector<byte> retval;

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

        byte memory_module::read_byte(word address) {
            byte retval {};

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

        word memory_module::last_block() const {
            auto last = memory_map.rbegin()->first;
            return last;
        }

        size_t memory_module::set_cache_address(word address) {
            if (cache_base != (address & address_mask)) {
                cache_base = address & address_mask;

                if (!memory_map.contains(cache_base)) {
                    memory_map[cache_base] = new byte[block_size] {0};
                }

                cache = memory_map[cache_base];
            }

            cache_address.w0 = address;
            return block_size - cache_address.b0;
        }

        void reg::increment(byte value, subreg_enum subreg) {
            // increment_array.push_back(std::make_pair(std::make_pair(this, subreg), value));
        }

        void reg::decrement(byte value, subreg_enum subreg) {
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

            size_t src_imm_size_flag() {
                return regs::in.b1 & opflag_imm_size;
            }

            byte src_imm_size() {
                return byte(1) << (regs::in.b1 & opflag_imm_size);
            }

            byte src_reg_flag() {
                return regs::in.b1 & opflag_reg;
            }

            byte src_reg_index() {
                return (regs::in.b1 & opflag_reg) >> 4;
            }

            byte src_subreg_index() {
                return regs::in.b1 & opflag_subreg;
            }

            subreg_enum src_subreg_flag() {
                return static_cast<subreg_enum>(regs::in.b1 & opflag_subreg);
            }

            byte src_subreg_size() {
                return subreg_size_map[static_cast<subreg_enum>(regs::in.b1 & opflag_subreg)];
            }

            reg& src_reg() {
                return *reg_map[(regs::in.b1 & opflag_reg) >> 4];
            }

            byte dst_imm_size_flag() {
                return regs::in.b2 & opflag_imm_size;
            }

            byte dst_imm_size() {
                return 1 << (regs::in.b2 & opflag_imm_size);
            }

            byte dst_reg_flag() {
                return regs::in.b2 & opflag_reg;
            }

            byte dst_reg_index() {
                return (regs::in.b2 & opflag_reg) >> 4;
            }

            byte dst_subreg_index() {
                return regs::in.b2 & opflag_subreg;
            }

            subreg_enum dst_subreg_flag() {
                return static_cast<subreg_enum>(regs::in.b2 & opflag_subreg);
            }

            byte dst_subreg_size() {
                return subreg_size_map[static_cast<subreg_enum>(regs::in.b2 & opflag_subreg)];
            }

            reg& dst_reg() {
                return *reg_map[(regs::in.b2 & opflag_reg) >> 4];
            }

            subreg_enum pc_src_imm_subreg_flag() {
                return imm_size_subreg_map[src_imm_size_flag()];
            }

            subreg_enum pc_dst_imm_subreg_flag() {
                return imm_size_subreg_map[dst_imm_size_flag()];
            }

            void clr_reg(reg_value &dst, subreg_enum dst_subreg) {
                auto dst_offset = subreg_offset_map[dst_subreg];
                auto dst_mask = subreg_mask_map[dst_subreg];

                word src_value = 0;
                dst.w0 = (~static_cast<word>(dst_mask) & dst.w0) | (src_value << dst_offset) & static_cast<word>(dst_mask);
            }

            void copy_regval_reg(reg_value const &src, subreg_enum src_subreg, reg_value &dst, subreg_enum dst_subreg) {
                auto src_offset = subreg_offset_map[src_subreg];
                auto src_mask = subreg_mask_map[src_subreg];
                
                auto dst_offset = subreg_offset_map[dst_subreg];
                auto dst_mask = subreg_mask_map[dst_subreg];

                word src_value = (src.w0 & static_cast<word>(src_mask)) >> src_offset;
                dst.w0 = (~static_cast<word>(dst_mask) & dst.w0) | (src_value << dst_offset) & static_cast<word>(dst_mask);
            }

            void copy_memval_reg(word address, size_t size, reg_value &dst_reg, subreg_enum dst_subreg) {
                auto dst_index = subreg_index_map[dst_subreg];
                mm.read(address, dst_reg, size, dst_index);
            }

            void copy_regaddr_reg(reg_value const &src, subreg_enum src_subreg, reg_value &dst, subreg_enum dst_subreg) {
                auto src_offset = subreg_offset_map[src_subreg];
                auto src_mask = subreg_mask_map[src_subreg];

                auto dst_offset = subreg_offset_map[dst_subreg];
                auto dst_mask = subreg_mask_map[dst_subreg];

                word src_address = (static_cast<word>(src_mask) & src.w0) >> src_offset;
                reg src_data;
                mm.read(src_address, src_data, subreg_enum::w0);
                dst.w0 = (~static_cast<word>(dst_mask) & dst.w0) | (src_data.w0 << dst_offset) & static_cast<word>(dst_mask);
            }

            void copy_memaddr_reg(word address, size_t size, reg_value &dst_reg, subreg_enum dst_subreg) {
                auto dst_offset = subreg_offset_map[dst_subreg];

                reg src_address;
                mm.read(address, src_address, subreg_enum::w0);

                mm.read(src_address.h0, dst_reg, size, dst_offset);
            }

            void copy_regval_regaddr(reg_value const &src, subreg_enum src_subreg, reg_value const &dst, subreg_enum dst_subreg) {
                auto src_offset = subreg_offset_map[src_subreg];
                auto src_mask = subreg_mask_map[src_subreg];
                auto size = subreg_size_map[src_subreg];

                auto dst_offset = subreg_offset_map[dst_subreg];
                auto dst_mask = subreg_mask_map[dst_subreg];

                reg_value src_value = (src.w0 & static_cast<word>(src_mask)) >> src_offset;;
                word dst_address = (dst.w0 & static_cast<word>(dst_mask)) >> dst_offset;

                switch (size) {
                    case 1: {
                        mm.write_byte(dst_address, src_value.b0);
                        break;
                    }

                    case 2: {
                        mm.write_qword(dst_address, src_value.q0);
                        break;
                    }

                    case 4: {
                        mm.write_hword(dst_address, src_value.h0);
                        break;
                    }

                    case 8: {
                        mm.write_word(dst_address, src_value.w0);
                        break;
                    }

                    default:
                        break;
                }
            }

            void copy_memval_regaddr(word address, size_t size, reg_value const &dst, subreg_enum dst_subreg) {
                auto dst_offset = subreg_offset_map[dst_subreg];
                auto dst_mask = subreg_mask_map[dst_subreg];

                reg_value src_value;
                mm.read(address, src_value, size, 0);
                word dst_address = (dst.w0 & static_cast<word>(dst_mask)) >> dst_offset;

                switch (size) {
                    case 1: {
                        mm.write_byte(dst_address, src_value.b0);
                        break;
                    }

                    case 2: {
                        mm.write_qword(dst_address, src_value.q0);
                        break;
                    }

                    case 4: {
                        mm.write_hword(dst_address, src_value.h0);
                        break;
                    }

                    case 8: {
                        mm.write_word(dst_address, src_value.w0);
                        break;
                    }

                    default:
                        break;
                }
            }

            byte step {0};

            enum class run_states {
                decode,
                execute,
                syscall,
                arithmetic_logic_unit
            };

            run_states run_state = run_states::decode;

            void instr_execute() {
                run_state = run_states::execute;
                step = 0;
            }

            void instr_syscall(qword id) {
                run_state = run_states::syscall;
            }

            void instr_jmp_alu() {
                run_state = run_states::arithmetic_logic_unit;
            }

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

        void add_device(qword id, device& new_device) {
            devices[id] = &new_device;
        }

        void tick();

        void run() {
            {
                std::lock_guard<std::mutex> lk(int_mutex);
                is_power_on = true;
            }

            int_event.notify_all();

            while (is_power_on) {
                tick();

                {
                    std::unique_lock<std::mutex> lk(int_mutex);

                    if (is_power_on) {
                        int_event.wait(lk);
                    }
                }
            }
        }

        void run_alu() {
            byte op_size = alu.b2; // Destination size

            switch (alu.b0 & arithmetic_logic_unit::opflag_code) {
                case arithmetic_logic_unit::op_add: {
                    switch (op_size) {
                        case 1: {
                            byte result = alu.dst_reg.b0 + alu.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result < alu.dst_reg.b0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = alu.dst_reg.q0 + alu.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result < alu.dst_reg.q0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = alu.dst_reg.h0 + alu.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result < alu.dst_reg.h0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = alu.dst_reg.w0 + alu.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result < alu.dst_reg.w0;
                            alu.dst_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case arithmetic_logic_unit::op_cmp:
                case arithmetic_logic_unit::op_cmpind:
                case arithmetic_logic_unit::op_sub: {
                    switch (op_size) {
                        case 1: {
                            byte result = alu.dst_reg.b0 - alu.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result > alu.dst_reg.b0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = alu.dst_reg.q0 - alu.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result > alu.dst_reg.q0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = alu.dst_reg.h0 - alu.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result > alu.dst_reg.h0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = alu.dst_reg.w0 - alu.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result > alu.dst_reg.w0;
                            alu.dst_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case arithmetic_logic_unit::op_mul: {
                    switch (op_size) {
                        case 1: {
                            byte result = alu.dst_reg.b0 - alu.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = is_mul_overflow(alu.src_reg.b0, alu.dst_reg.b0) || is_mul_underflow(alu.src_reg.b0, alu.dst_reg.b0);
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = alu.dst_reg.q0 - alu.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = is_mul_overflow(alu.src_reg.q0, alu.dst_reg.q0) || is_mul_underflow(alu.src_reg.q0, alu.dst_reg.q0);
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = alu.dst_reg.h0 - alu.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = is_mul_overflow(alu.src_reg.h0, alu.dst_reg.h0) || is_mul_underflow(alu.src_reg.h0, alu.dst_reg.h0);
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = alu.dst_reg.w0 - alu.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = is_mul_overflow(alu.src_reg.h0, alu.dst_reg.h0) || is_mul_underflow(alu.src_reg.h0, alu.dst_reg.h0);
                            alu.dst_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case arithmetic_logic_unit::op_div: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            byte result = alu.dst_reg.b0 / alu.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = alu.dst_reg.q0 / alu.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = alu.dst_reg.h0 / alu.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = alu.dst_reg.w0 / alu.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            alu.dst_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case arithmetic_logic_unit::op_mod: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            byte result = alu.dst_reg.b0 % alu.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = alu.dst_reg.q0 % alu.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = alu.dst_reg.h0 % alu.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = alu.dst_reg.w0 % alu.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            alu.dst_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case arithmetic_logic_unit::op_test:
                case arithmetic_logic_unit::op_testind:
                case arithmetic_logic_unit::op_and: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            byte result = alu.dst_reg.b0 & alu.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = alu.dst_reg.q0 & alu.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = alu.dst_reg.h0 & alu.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = alu.dst_reg.w0 & alu.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            alu.dst_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case arithmetic_logic_unit::op_or: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            byte result = alu.dst_reg.b0 | alu.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = alu.dst_reg.q0 | alu.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = alu.dst_reg.h0 | alu.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = alu.dst_reg.w0 | alu.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            alu.dst_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case arithmetic_logic_unit::op_nor: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            byte result = ~(alu.dst_reg.b0 | alu.src_reg.b0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = ~(alu.dst_reg.q0 | alu.src_reg.q0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = ~(alu.dst_reg.h0 | alu.src_reg.h0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = ~(alu.dst_reg.w0 | alu.src_reg.w0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            alu.dst_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case arithmetic_logic_unit::op_nand: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            byte result = ~(alu.dst_reg.b0 & alu.src_reg.b0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = ~(alu.dst_reg.q0 & alu.src_reg.q0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = ~(alu.dst_reg.h0 & alu.src_reg.h0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = ~(alu.dst_reg.w0 & alu.src_reg.w0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            alu.dst_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case arithmetic_logic_unit::op_xor: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            byte result = alu.dst_reg.b0 ^ alu.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = alu.dst_reg.q0 ^ alu.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = alu.dst_reg.h0 ^ alu.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = alu.dst_reg.w0 ^ alu.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            alu.dst_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case arithmetic_logic_unit::op_shl: {
                    switch (op_size) {
                        case 1: {
                            byte result = alu.dst_reg.b0 << alu.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result < alu.dst_reg.b0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = alu.dst_reg.q0 << alu.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result < alu.dst_reg.q0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = alu.dst_reg.h0 << alu.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result < alu.dst_reg.h0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = alu.dst_reg.w0 << alu.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result < alu.dst_reg.w0;
                            alu.dst_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case arithmetic_logic_unit::op_shr: {
                    switch (op_size) {
                        case 1: {
                            byte result = alu.dst_reg.b0 >> alu.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result < alu.dst_reg.b0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = alu.dst_reg.q0 >> alu.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result < alu.dst_reg.q0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = alu.dst_reg.h0 >> alu.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result < alu.dst_reg.h0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = alu.dst_reg.w0 >> alu.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result < alu.dst_reg.w0;
                            alu.dst_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case arithmetic_logic_unit::op_inc: {
                    switch (op_size) {
                        case 1: {
                            byte result = alu.dst_reg.b0 + byte(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result < alu.dst_reg.b0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = alu.dst_reg.q0 + qword(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result < alu.dst_reg.q0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = alu.dst_reg.h0 + hword(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result < alu.dst_reg.h0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = alu.dst_reg.w0 + word(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result < alu.dst_reg.w0;
                            alu.dst_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case arithmetic_logic_unit::op_dec: {
                    switch (op_size) {
                        case 1: {
                            byte result = alu.dst_reg.b0 - byte(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result > alu.dst_reg.b0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = alu.dst_reg.q0 - qword(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result > alu.dst_reg.q0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = alu.dst_reg.h0 - hword(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result > alu.dst_reg.h0;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = alu.dst_reg.w0 - word(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result > alu.dst_reg.w0;
                            alu.dst_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case arithmetic_logic_unit::op_not: {
                    overflow_flag = 0;

                    switch (op_size) {
                        case 1: {
                            byte result = ~alu.dst_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = ~alu.dst_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = ~alu.dst_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            alu.dst_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = ~alu.dst_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            alu.dst_reg.w0 = result;
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
                step = 0;

                /* Execute instruction */
                switch (regs::in.b0) {
                    case instr::halt_opcode: {
                        running_flag = false;
                        is_power_on = false; // just temporary until I get "device" interaction working
                        break;
                    }

                    case instr::clr_regVal: {
                        regs::p.h0 += 1;
                        clr_reg(src_reg(), src_subreg_flag());
                        break;
                    }

                    case instr::ld_regVal_reg: {
                        regs::p.h0 += 2;
                        copy_regval_reg(src_reg(), src_subreg_flag(), dst_reg(), dst_subreg_flag());
                        break;
                    }

                    case instr::ld_immVal_reg: {
                        regs::p.h0 += 2;
                        hword imm_size = src_imm_size();
                        copy_memval_reg(regs::p.h0, imm_size, dst_reg(), dst_subreg_flag());
                        regs::p.h0 += imm_size;
                        break;
                    }

                    case instr::ld_regAddr_reg: {
                        regs::p.h0 += 2;
                        copy_regaddr_reg(src_reg(), src_subreg_flag(), dst_reg(), dst_subreg_flag());
                        break;
                    }

                    case instr::ld_immAddr_reg: {
                        regs::p.h0 += 2;
                        hword imm_size = src_imm_size();
                        hword dst_size = dst_subreg_size();
                        copy_memaddr_reg(regs::p.h0, dst_size, dst_reg(), dst_subreg_flag());
                        regs::p.h0 += imm_size;
                        break;
                    }

                    case instr::st_regVal_regAddr: {
                        regs::p.h0 += 2;
                        copy_regval_regaddr(src_reg(), src_subreg_flag(), dst_reg(), dst_subreg_flag());
                        break;
                    }

                    case instr::st_immVal_regAddr: {
                        regs::p.h0 += 2;
                        hword imm_size = src_imm_size();
                        copy_memval_regaddr(regs::p.h0, imm_size, dst_reg(), dst_subreg_flag());
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
                        copy_regval_reg(src_reg(), src_subreg_flag(), alu.src_reg, subreg_enum::w0);
                        copy_regval_reg(dst_reg(), dst_subreg_flag(), alu.dst_reg, subreg_enum::w0);
                        alu.b0 = regs::in.b0;
                        alu.b1 = src_subreg_size();
                        alu.b2 = dst_subreg_size();
                        run_alu();
                        copy_regval_reg(alu.dst_reg, subreg_enum::w0, dst_reg(), dst_subreg_flag());
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
                        byte src_size = src_imm_size();
                        copy_memval_reg(regs::p.h0, src_size, alu.src_reg, subreg_enum::w0);
                        copy_regval_reg(dst_reg(), dst_subreg_flag(), alu.dst_reg, subreg_enum::w0);
                        alu.b0 = regs::in.b0;
                        alu.b1 = src_size;
                        alu.b2 = dst_subreg_size();
                        run_alu();
                        regs::p.h0 += src_size;
                        copy_regval_reg(alu.dst_reg, subreg_enum::w0, dst_reg(), dst_subreg_flag());
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
                        copy_regaddr_reg(src_reg(), src_subreg_flag(), alu.src_reg, subreg_enum::w0);
                        copy_regval_reg(dst_reg(), dst_subreg_flag(), alu.dst_reg, subreg_enum::w0);
                        alu.b0 = regs::in.b0;
                        alu.b1 = src_subreg_size();
                        alu.b2 = dst_subreg_size();
                        run_alu();
                        copy_regval_reg(alu.dst_reg, subreg_enum::w0, dst_reg(), dst_subreg_flag());
                        break;

#if false
                            case 0: {
                                al.b0 = regs::in.b0;
                                regs::p.increment(2);
                                src_reg().enable_to_bus(address_bus, src_subreg_flag());
                                mm.set_address_from_bus(address_bus);
                                break;
                            }

                            case 1: {
                                mm.enable_memory_to_bus(data_bus_0, subreg_enum::w0);
                                al.set_src_from_bus(data_bus_0);
                                al.b1 = src_subreg_size();
                                dst_reg().enable_to_bus(data_bus_1, dst_subreg_flag());
                                al.set_dst_from_bus(data_bus_1);
                                al.b2 = dst_subreg_size();
                                // byte op_size = subreg_size_map[dst_subreg_index()];
                                instr_jmp_alu();
                                break;
                            }

                            case 2: {
                                al.enable_dst_to_bus(data_bus_0);
                                dst_reg().set_from_bus(data_bus_0, dst_subreg_flag());
                                instr_complete();
                                break;
                            }
#endif
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
                        byte src_size = src_imm_size();
                        copy_memaddr_reg(regs::p.h0, src_size, alu.src_reg, subreg_enum::w0);
                        copy_regval_reg(dst_reg(), dst_subreg_flag(), alu.dst_reg, subreg_enum::w0);
                        alu.b0 = regs::in.b0;
                        alu.b1 = src_size;
                        alu.b2 = dst_subreg_size();
                        run_alu();
                        regs::p.h0 += src_size;
                        copy_regval_reg(alu.dst_reg, subreg_enum::w0, dst_reg(), dst_subreg_flag());
                        break;
#if false
                        switch (step) {
                            case 0: {
                                al.b0 = regs::in.b0;
                                regs::p.increment(2);
                                regs::p.enable_to_bus(address_bus, subreg_enum::w0);
                                mm.set_address_from_bus(address_bus);
                                break;
                            }

                            case 1: {
                                byte size = src_imm_size();
                                regs::p.increment(size);
                                al.b1 = size;
                                mm.enable_memory_to_bus(address_bus, subreg_enum::w0);
                                mm.set_address_from_bus(address_bus);
                                break;
                            }

                            case 2: {
                                mm.enable_memory_to_bus(data_bus_0, subreg_enum::w0);
                                dst_reg().enable_to_bus(data_bus_1, dst_subreg_flag());
                                al.b2 = dst_subreg_size();
                                al.set_src_from_bus(data_bus_0);
                                al.set_dst_from_bus(data_bus_1);
                                instr_jmp_alu();
                                break;
                            }

                            case 3: {
                                al.enable_dst_to_bus(data_bus_0);
                                dst_reg().set_from_bus(data_bus_0, dst_subreg_flag());
                                instr_complete();
                                break;
                            }
                        }

                        break;
#endif
                    }

                    case instr::inc_regVal:
                    case instr::dec_regVal:
                    case instr::not_regVal: {
                        regs::p.h0 += 1;
                        copy_regval_reg(src_reg(), src_subreg_flag(), alu.dst_reg, subreg_enum::w0);
                        alu.b0 = regs::in.b0;
                        alu.b1 = src_subreg_size();
                        alu.b2 = src_subreg_size();
                        run_alu();
                        copy_regval_reg(alu.dst_reg, subreg_enum::w0, src_reg(), src_subreg_flag());
                        break;
                    }

                    case instr::cmpind_immVal_regAddr:
                    case instr::testind_immVal_regAddr:
                    {
                        regs::p.h0 += 2;
                        byte src_size = src_imm_size();
                        copy_memval_reg(regs::p.h0, src_size, alu.src_reg, subreg_enum::w0);
                        copy_regaddr_reg(dst_reg(), dst_subreg_flag(), alu.dst_reg, dst_subreg_flag());
                        regs::p.h0 += src_size;
                        alu.b0 = regs::in.b0;
                        alu.b1 = src_size;
                        alu.b2 = src_subreg_size();
                        run_alu();
                        break;
#if false
                        switch (step) {
                            case 0: {
                                al.b0 = regs::in.b0;
                                regs::p.increment(2);
                                regs::p.enable_to_bus(address_bus, subreg_enum::w0);
                                mm.set_address_from_bus(address_bus);
                                break;
                            }

                            case 1: {
                                byte src_size = src_imm_size();
                                al.b1 = src_size;
                                al.b2 = src_size;
                                regs::p.increment(src_size);
                                mm.enable_memory_to_bus(data_bus_0, src_size);
                                al.set_src_from_bus(data_bus_0);
                                break;
                            }

                            case 2: {
                                dst_reg().enable_to_bus(address_bus, dst_subreg_flag());
                                mm.set_address_from_bus(address_bus);
                                break;
                            }

                            case 3: {
                                mm.enable_memory_to_bus(data_bus_0, dst_subreg_flag());
                                al.set_dst_from_bus(data_bus_0);
                                instr_jmp_alu();
                                break;
                            }

                            case 4: {
                                instr_complete();
                                break;
                            }
                        }

                        break;
#endif
                    }

                    case instr::cmpind_regVal_regAddr:
                    case instr::testind_regVal_regAddr:
                    {
                        regs::p.h0 += 2;
                        byte src_size = src_subreg_size();
                        copy_regval_reg(src_reg(), src_subreg_flag(), alu.src_reg, subreg_enum::w0);
                        copy_regaddr_reg(dst_reg(), dst_subreg_flag(), alu.dst_reg, dst_subreg_flag());
                        alu.b0 = regs::in.b0;
                        alu.b1 = src_size;
                        alu.b2 = src_subreg_size();
                        run_alu();
                        break;
                    }

                    case instr::out_regVal_imm: {
                        regs::p.h0 += 2;
                        byte dst_size = dst_imm_size();
                        copy_memval_reg(regs::p.h0, dst_size, operand2, subreg_enum::w0);
                        device *pdst_dev = devices[operand2.q0];
                        device &dst_dev = *(pdst_dev);
                        copy_regval_reg(src_reg(), src_subreg_flag(), dst_dev, subreg_enum::w0);
                        regs::p.h0 += dst_size;
                        break;
                    }

                    case instr::out_immVal_imm: {
                        regs::p.h0 += 2;
                        byte src_size = src_imm_size();
                        copy_memval_reg(regs::p.h0, src_size, operand1, subreg_enum::w0);
                        regs::p.h0 += src_size;
                        byte dst_size = dst_imm_size();
                        copy_memval_reg(regs::p.h0, dst_size, operand2, subreg_enum::w0);
                        device *pdst_dev = devices[operand2.q0];
                        device &dst_dev = *(pdst_dev);
                        copy_regval_reg(operand1, subreg_enum::w0, dst_dev, subreg_enum::w0);
                        regs::p.h0 += dst_size;
                        break;
                    }

                    case instr::out_regAddr_imm: {
                        regs::p.h0 += 2;
                        copy_regaddr_reg(src_reg(), src_subreg_flag(), operand1, subreg_enum::w0);
                        byte dst_size = dst_imm_size();
                        copy_memval_reg(regs::p.h0, dst_size, operand2, subreg_enum::w0);
                        device *pdst_dev = devices[operand2.q0];
                        device &dst_dev = *(pdst_dev);
                        copy_regval_reg(src_reg(), src_subreg_flag(), dst_dev, subreg_enum::w0);
                        regs::p.h0 += dst_size;
                        break;
                    }

                    case instr::out_immAddr_imm: {
                        regs::p.h0 += 2;
                        byte src_size = src_imm_size();
                        copy_memaddr_reg(regs::p.h0, src_size, operand1, subreg_enum::w0);
                        regs::p.h0 += src_size;
                        byte dst_size = dst_imm_size();
                        copy_memval_reg(regs::p.h0, dst_size, operand2, subreg_enum::w0);
                        device *pdst_dev = devices[operand2.q0];
                        device &dst_dev = *(pdst_dev);
                        copy_regval_reg(operand1, subreg_enum::w0, dst_dev, subreg_enum::w0);
                        regs::p.h0 += dst_size;
                        break;
                    }

                    case instr::sys_immVal: {
                        regs::p.h0 += 1;
                        byte src_size = src_imm_size();
                        copy_memval_reg(regs::p.h0, src_size, operand1, subreg_enum::w0);
                        regs::p.h0 += src_size;
                        regs::a.w0 = sys::call(operand1.b0);
                        break;
                    }

                    case instr::sys_regVal: {
                        regs::p.h0 += 1;
                        regs::a.w0 = sys::call(src_reg().b0);
                        break;
                    }

                    case instr::pop_regVal: {
                        regs::p.h0 += 1;
                        auto src_size = src_subreg_size();
                        copy_memval_reg(regs::s.h0, src_size, src_reg(), src_subreg_flag());
                        regs::s.h0 += src_size;
                        break;
                    }

                    case instr::push_immVal: {
                        regs::p.h0 += 1;
                        byte src_size = src_imm_size();
                        regs::s.h0 -= src_size;
                        copy_memval_regaddr(regs::p.h0, src_size, regs::s, subreg_enum::h0);
                        regs::p.h0 += src_size;
                        break;
                    }

                    case instr::push_regVal: {
                        regs::p.h0 += 1;
                        byte src_size = src_subreg_size();
                        regs::s.h0 -= src_size;
                        copy_regval_regaddr(src_reg(), src_subreg_flag(), regs::s, subreg_enum::h0);
                        break;
                    }

                    case instr::call_immVal: {
                        regs::p.h0 += 1;
                        byte src_size = src_imm_size();
                        copy_memval_reg(regs::p.h0, src_size, operand1, subreg_enum::h0);
                        regs::s.h0 -= subreg_size_map[subreg_enum::h0];
                        regs::p.h0 += src_size;
                        copy_regval_regaddr(regs::p, subreg_enum::h0, regs::s, subreg_enum::h0);
                        regs::p.h0 = operand1.h0;
                        break;
                    }

                    case instr::ret_opcode: {
                        byte src_size = subreg_size_map[subreg_enum::h0];
                        copy_memval_reg(regs::s.h0, src_size, regs::p, subreg_enum::h0);
                        regs::s.h0 += src_size;
                        break;
                    }

                    case instr::jmp_immVal: {
                        regs::p.h0 += 1;
                        copy_memval_reg(regs::p.h0, subreg_size_map[subreg_enum::h0], regs::p, subreg_enum::h0);
                        break;
                    }

                    case instr::jz_immVal: {
                        regs::p.h0 += 1;

                        if (cpu::zero_flag) {
                            copy_memval_reg(regs::p.h0, subreg_size_map[subreg_enum::h0], regs::p, subreg_enum::h0);
                        }
                        else {
                            byte src_size = src_imm_size();
                            regs::p.h0 += src_size;
                        }

                        break;
                    }

                    default: {
                        std::stringstream err {};
                        err << "unknown opcode: " << std::hex << regs::in.b0;
                        throw std::logic_error(err.str());
                        // throw std::exception(err.str().c_str());
                        break;
                    }
                }

            }
        }

    } // namespace cpu; 

} // namespace maize
