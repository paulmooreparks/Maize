#include "maize_cpu.h"
#include "maize_sys.h"
#include <unordered_map>

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
            const byte subreg_size_map[] = {
                1,
                1,
                1,
                1,
                1,
                1,
                1,
                1,
                2,
                2,
                2,
                2,
                4,
                4,
                8,
                8
            };

            /* Maps the value of a subreg_enum to the offset of the corresponding subregister in the register's 64-bit value. */
            const maize::byte subreg_offset_map[] {
                0,  // b0
                8,  // b1
                16, // b2
                24, // b3
                32, // b4
                40, // b5
                48, // b6
                56, // b7
                0,  // q0
                16, // q1
                32, // q2
                48, // q3
                0,  // h0
                32, // h1
                0  // w0
            };

            const maize::byte subreg_index_map[] {
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
            const subreg_mask_enum subreg_mask_map[] = {
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

        void reg::increment(byte value, subreg_enum subreg) {
            increment_array.push_back(std::make_pair(std::make_pair(this, subreg), value));
        }

        void reg::decrement(byte value, subreg_enum subreg) {
            increment_array.push_back(std::make_pair(std::make_pair(this, subreg), -value));
        }

        void reg::enable_to_bus(bus& en_bus, subreg_enum subreg) {
            bus_enable_array.push_back(
                reg_op_info(&en_bus, this, subreg_mask_map[static_cast<size_t>(subreg)], subreg_offset_map[static_cast<size_t>(subreg)])
            );
        }

        void reg::set_from_bus(bus& set_bus, subreg_enum subreg) {
            bus_set_array.push_back(
                reg_op_info(&set_bus, this, subreg_mask_map[static_cast<size_t>(subreg)], subreg_offset_map[static_cast<size_t>(subreg)])
            );
        }

        void reg::on_enable() {
        }

        void reg::on_set() {
        }

        void memory_module::set_address_from_bus(bus& source_bus) {
            address_reg.set_from_bus(source_bus, subreg_enum::h0);
        }

        void memory_module::set_segment_from_bus(bus& source_bus) {
            address_reg.set_from_bus(source_bus, subreg_enum::h1);
        }


        void memory_module::enable_memory_to_bus(bus& load_bus, size_t size) {
            pload_bus = &load_bus;
            load_size = size;
        }

        void memory_module::enable_memory_to_bus(bus& load_bus, subreg_enum subreg) {
            pload_bus = &load_bus;
            load_size = subreg_size_map[static_cast<size_t>(subreg)];
        }

        bool memory_module::enable_memory_scheduled() {
            return (pload_bus);
        }

        void memory_module::on_enable_memory() {
            /* The "right" thing to do here is check if pload_bus is non-null,
            but I'm making that check in enable_memory_scheduled() to avoid a
            function call anyway, so I'll assume it's non-null here. */
            w0 = 0;
            reg_value address = address_reg.w0;
            size_t rem = set_cache_address(address);
            size_t idx = cache_address.b0;

            switch (load_size) {
                case sizeof(byte) :
                    b0 = cache[idx];
                    this->enable_to_bus(*pload_bus, subreg_enum::b0);
                    break;

                case sizeof(qword) :
                    if (rem >= sizeof(qword)) {
                        q0 = *((qword*)(cache + idx));
                    }
                    else {
                        b0 = cache[cache_address.b0];
                        set_cache_address(++address.h0);
                        b1 = cache[cache_address.b0];
                    }

                    this->enable_to_bus(*pload_bus, subreg_enum::q0);
                    break;

                case sizeof(hword) :
                    if (rem >= sizeof(hword)) {
                        h0 = *((hword*)(cache + idx));
                    }
                    else {
                        b0 = cache[cache_address.b0];
                        set_cache_address(++address.h0);
                        b1 = cache[cache_address.b0];
                        set_cache_address(++address.h0);
                        b2 = cache[cache_address.b0];
                        set_cache_address(++address.h0);
                        b3 = cache[cache_address.b0];
                    }

                    this->enable_to_bus(*pload_bus, subreg_enum::h0);
                    break;

                case sizeof(word) :
                    if (rem >= sizeof(word)) {
                        w0 = *((word*)(cache + idx));
                    }
                    else {
                        b0 = cache[cache_address.b0];
                        set_cache_address(++address.h0);
                        b1 = cache[cache_address.b0];
                        set_cache_address(++address.h0);
                        b2 = cache[cache_address.b0];
                        set_cache_address(++address.h0);
                        b3 = cache[cache_address.b0];
                        set_cache_address(++address.h0);
                        b4 = cache[cache_address.b0];
                        set_cache_address(++address.h0);
                        b5 = cache[cache_address.b0];
                        set_cache_address(++address.h0);
                        b6 = cache[cache_address.b0];
                        set_cache_address(++address.h0);
                        b7 = cache[cache_address.b0];
                    }

                    this->enable_to_bus(*pload_bus, subreg_enum::w0);
                    break;

                default:
                    throw std::logic_error("Should not get here!");
                    break;
            }

            pload_bus = nullptr;
        }

        void memory_module::set_memory_from_bus(bus& store_bus, subreg_enum subreg) {
            pstore_bus = &store_bus;
            store_mask = subreg_mask_map[static_cast<size_t>(subreg)];
            this->set_from_bus(store_bus, subreg);
        }

        bool memory_module::set_memory_scheduled() {
            return pstore_bus;
        }

        void memory_module::on_set_memory() {
            if (pstore_bus) {
                pstore_bus = nullptr;
                reg_value address = address_reg.w0;
                set_memory(address);
            }
        }

        void memory_module::set_memory(maize::cpu::reg_value &address) {
            size_t rem = set_cache_address(address);
            size_t idx = cache_address.b0;
            size_t subreg_index = static_cast<size_t>(store_mask);

            switch (store_mask) {
                case subreg_mask_enum::b0:
                    cache[idx] = b0;
                    break;

                case subreg_mask_enum::b1:
                    cache[idx] = b1;
                    break;

                case subreg_mask_enum::b2:
                    cache[idx] = b2;
                    break;

                case subreg_mask_enum::b3:
                    cache[idx] = b3;
                    break;

                case subreg_mask_enum::b4:
                    cache[idx] = b4;
                    break;

                case subreg_mask_enum::b5:
                    cache[idx] = b5;
                    break;

                case subreg_mask_enum::b6:
                    cache[idx] = b6;
                    break;

                case subreg_mask_enum::b7:
                    cache[idx] = b7;
                    break;

                case subreg_mask_enum::q0:
                    if (rem >= 2) {
                        *((qword *)(cache + idx)) = q0;
                    }
                    else {
                        cache[cache_address.b0] = b0;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b1;
                    }

                    break;

                case subreg_mask_enum::q1:
                    if (rem >= 2) {
                        *((qword *)(cache + idx)) = q1;
                    }
                    else {
                        cache[cache_address.b0] = b2;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b3;
                    }

                    break;

                case subreg_mask_enum::q2:
                    if (rem >= 2) {
                        *((qword *)(cache + idx)) = q2;
                    }
                    else {
                        cache[cache_address.b0] = b4;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b5;
                    }

                    break;

                case subreg_mask_enum::q3:
                    if (rem >= 2) {
                        *((qword *)(cache + idx)) = q3;
                    }
                    else {
                        cache[cache_address.b0] = b6;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b7;
                    }

                    break;

                case subreg_mask_enum::h0:
                    if (rem >= 4) {
                        *((hword *)(cache + idx)) = h0;
                    }
                    else {
                        cache[cache_address.b0] = b0;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b1;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b2;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b3;
                    }

                    break;

                case subreg_mask_enum::h1:
                    if (rem >= 4) {
                        *((hword *)(cache + idx)) = h1;
                    }
                    else {
                        cache[cache_address.b0] = b4;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b5;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b6;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b7;
                    }

                    break;

                case subreg_mask_enum::w0:
                    if (rem >= 8) {
                        *((word *)(cache + idx)) = w0;
                    }
                    else {
                        cache[cache_address.b0] = b0;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b1;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b2;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b3;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b4;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b5;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b6;
                        set_cache_address(++address.h0);
                        cache[cache_address.b0] = b7;
                    }

                    break;
            }
        }

        template <> hword memory_module::write<byte>(reg_value address, byte value) {
            set_cache_address(address);
            cache[cache_address.b0] = value;
            return sizeof(byte);
        }

        template<> hword memory_module::write<qword>(reg_value address, qword value) {
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

        template<> hword memory_module::write<hword>(reg_value address, hword value) {
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

        template<> hword memory_module::write<word>(reg_value address, word value) {
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
            auto count = subreg_size_map[static_cast<size_t>(subreg)];
            size_t dest_idx {subreg_index_map[static_cast<size_t>(subreg)]};
            return read(address, reg, count, dest_idx);
        }

        size_t memory_module::read(reg_value const &address, reg_value &reg, subreg_enum subreg) {
            auto count = subreg_size_map[static_cast<size_t>(subreg)];
            size_t dest_idx {subreg_index_map[static_cast<size_t>(subreg)]};
            return read(address.w0, reg, count, dest_idx);
        }

        size_t memory_module::read(reg_value const &address, reg_value &reg, size_t count, size_t dest_idx) {
            return read(address.w0, reg, count, dest_idx);
        }

        size_t memory_module::read(word address, reg_value &reg, size_t count, size_t dest_idx) {
            size_t read_count {0};

            do {
                size_t rem {set_cache_address(address)};
                size_t idx {cache_address.b0};

                if (rem >= count) {
                    while (count && idx <= 0xFF) {
                        reg[dest_idx] = cache[idx];
                        ++idx;
                        ++dest_idx;
                        --count;
                        ++read_count;
                    }
                }
                else {
                    while (count) {
                        reg[dest_idx] = cache[cache_address.b0];
                        set_cache_address(++address);
                        ++dest_idx;
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



        void device::enable_address_to_bus(bus& enable_bus) {
            address_reg.enable_to_bus(enable_bus, subreg_enum::w0);
        }

        void device::set_address_from_bus(bus& set_bus) {
            address_reg.set_from_bus(set_bus, subreg_enum::w0);
        }

        void device::enable_io_to_bus(bus& io_bus) {
            enable_to_bus(io_bus, subreg_enum::w0);
        }

        void device::set_io_from_bus(bus& source_bus) {
            set_from_bus(source_bus, subreg_enum::w0);
        }


        void alu::set_src_from_bus(bus& source_bus, subreg_enum subreg) {
            src_reg.set_from_bus(source_bus, subreg);
        }

        void alu::set_dest_from_bus(bus& source_bus, subreg_enum subreg) {
            dest_reg.set_from_bus(source_bus, subreg);
        }

        void alu::enable_dest_to_bus(bus& dest_bus, subreg_enum subreg) {
            dest_reg.enable_to_bus(dest_bus, subreg);
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
                return subreg_size_map[regs::in.b1 & opflag_subreg];
            }

            reg& src_reg() {
                return *reg_map[(regs::in.b1 & opflag_reg) >> 4];
            }

            byte dest_imm_size_flag() {
                return regs::in.b2 & opflag_imm_size;
            }

            byte dest_imm_size() {
                return 1 << (regs::in.b2 & opflag_imm_size);
            }

            byte dest_reg_flag() {
                return regs::in.b2 & opflag_reg;
            }

            byte dest_reg_index() {
                return (regs::in.b2 & opflag_reg) >> 4;
            }

            byte dest_subreg_index() {
                return regs::in.b2 & opflag_subreg;
            }

            subreg_enum dest_subreg_flag() {
                return static_cast<subreg_enum>(regs::in.b2 & opflag_subreg);
            }

            byte dest_subreg_size() {
                return subreg_size_map[regs::in.b2 & opflag_subreg];
            }

            reg& dest_reg() {
                return *reg_map[(regs::in.b2 & opflag_reg) >> 4];
            }

            subreg_enum pc_src_imm_subreg_flag() {
                return imm_size_subreg_map[src_imm_size_flag()];
            }

            subreg_enum pc_dest_imm_subreg_flag() {
                return imm_size_subreg_map[dest_imm_size_flag()];
            }

            void clr_reg(reg_value &dest, subreg_enum dst_subreg) {
                auto dst_subreg_idx = static_cast<size_t>(dst_subreg);
                auto dst_offset = subreg_offset_map[dst_subreg_idx];
                auto dst_mask = subreg_mask_map[dst_subreg_idx];

                word src_value = 0;
                dest.w0 = (~static_cast<word>(dst_mask) & dest.w0) | (src_value << dst_offset) & static_cast<word>(dst_mask);
            }

            void copy_regval_reg(reg_value const &src, subreg_enum src_subreg, reg_value &dest, subreg_enum dst_subreg) {
                auto src_subreg_idx = static_cast<size_t>(src_subreg);
                auto src_offset = subreg_offset_map[src_subreg_idx];
                auto src_mask = subreg_mask_map[src_subreg_idx];
                
                auto dst_subreg_idx = static_cast<size_t>(dst_subreg);
                auto dst_offset = subreg_offset_map[dst_subreg_idx];
                auto dst_mask = subreg_mask_map[dst_subreg_idx];

                word src_value = (src.w0 & static_cast<word>(src_mask)) >> src_offset;
                dest.w0 = (~static_cast<word>(dst_mask) & dest.w0) | (src_value << dst_offset) & static_cast<word>(dst_mask);
            }

            void copy_immval_reg(word address, size_t size, reg_value &dst_reg, subreg_enum dst_subreg) {
                auto dst_subreg_idx = static_cast<size_t>(dst_subreg);
                auto dst_index = subreg_index_map[dst_subreg_idx];
                mm.read(address, dst_reg, size, dst_index);
            }

            void copy_regaddr_reg(reg_value const &src, subreg_enum src_subreg, reg_value &dest, subreg_enum dst_subreg) {
                auto src_subreg_idx = static_cast<size_t>(src_subreg);
                auto src_offset = subreg_offset_map[src_subreg_idx];
                auto src_mask = subreg_mask_map[src_subreg_idx];

                auto dst_subreg_idx = static_cast<size_t>(dst_subreg);
                auto dst_offset = subreg_offset_map[dst_subreg_idx];
                auto dst_mask = subreg_mask_map[dst_subreg_idx];

                word src_address = (static_cast<word>(src_mask) & src.w0) >> src_offset;
                reg src_data;
                mm.read(src_address, src_data, subreg_enum::w0);
                dest.w0 = (~static_cast<word>(dst_mask) & dest.w0) | (src_data.w0 << dst_offset) & static_cast<word>(dst_mask);
            }

            void copy_immaddr_reg(word address, size_t size, reg_value &dst_reg, subreg_enum dst_subreg) {
                auto dst_subreg_idx = static_cast<size_t>(dst_subreg);
                auto dst_offset = subreg_offset_map[dst_subreg_idx];

                reg src_address;
                mm.read(address, src_address, subreg_enum::w0);

                mm.read(src_address.h0, dst_reg, size, dst_offset);
            }

            void copy_regval_regaddr(reg_value const &src, subreg_enum src_subreg, reg_value const &dest, subreg_enum dst_subreg) {
                auto src_subreg_idx = static_cast<size_t>(src_subreg);
                auto src_offset = subreg_offset_map[src_subreg_idx];
                auto src_mask = subreg_mask_map[src_subreg_idx];
                auto size = subreg_size_map[src_subreg_idx];

                auto dst_subreg_idx = static_cast<size_t>(dst_subreg);
                auto dst_offset = subreg_offset_map[dst_subreg_idx];
                auto dst_mask = subreg_mask_map[dst_subreg_idx];

                reg_value src_value = (src.w0 & static_cast<word>(src_mask)) >> src_offset;;
                word dest_address = (dest.w0 & static_cast<word>(dst_mask)) >> dst_offset;

                switch (size) {
                    case 1: {
                        mm.write(dest_address, src_value.b0);
                        break;
                    }

                    case 2: {
                        mm.write(dest_address, src_value.q0);
                        break;
                    }

                    case 4: {
                        mm.write(dest_address, src_value.h0);
                        break;
                    }

                    case 8: {
                        mm.write(dest_address, src_value.w0);
                        break;
                    }

                    default:
                        break;
                }
            }

            void copy_immval_regaddr(word address, size_t size, reg_value const &dest, subreg_enum dst_subreg) {
                auto dst_subreg_idx = static_cast<size_t>(dst_subreg);
                auto dst_offset = subreg_offset_map[dst_subreg_idx];
                auto dst_mask = subreg_mask_map[dst_subreg_idx];

                reg_value src_value;
                mm.read(address, src_value, size, 0);
                word dest_address = (dest.w0 & static_cast<word>(dst_mask)) >> dst_offset;

                switch (size) {
                    case 1: {
                        mm.write(dest_address, src_value.b0);
                        break;
                    }

                    case 2: {
                        mm.write(dest_address, src_value.q0);
                        break;
                    }

                    case 4: {
                        mm.write(dest_address, src_value.h0);
                        break;
                    }

                    case 8: {
                        mm.write(dest_address, src_value.w0);
                        break;
                    }

                    default:
                        break;
                }
            }

            byte cycle {0};
            byte step {0};

            enum class run_states {
                decode,
                execute,
                syscall,
                alu
            };

            run_states run_state = run_states::decode;

            void instr_execute() {
                run_state = run_states::execute;
                step = 0;
            }

            void instr_complete() {
                run_state = run_states::decode;
                cycle = 0;
                step = 0;
            }

            void instr_syscall(qword id) {
                run_state = run_states::syscall;
            }

            void instr_jmp_alu() {
                run_state = run_states::alu;
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
        alu al;

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
            byte op_size = al.b2; // Destination size

            switch (al.b0 & alu::opflag_code) {
                case alu::op_add: {
                    switch (op_size) {
                        case 1: {
                            byte result = al.dest_reg.b0 + al.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result < al.dest_reg.b0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = al.dest_reg.q0 + al.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result < al.dest_reg.q0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = al.dest_reg.h0 + al.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result < al.dest_reg.h0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = al.dest_reg.w0 + al.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result < al.dest_reg.w0;
                            al.dest_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu::op_cmp:
                case alu::op_cmpind:
                case alu::op_sub: {
                    switch (op_size) {
                        case 1: {
                            byte result = al.dest_reg.b0 - al.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result > al.dest_reg.b0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = al.dest_reg.q0 - al.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result > al.dest_reg.q0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = al.dest_reg.h0 - al.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result > al.dest_reg.h0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = al.dest_reg.w0 - al.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result > al.dest_reg.w0;
                            al.dest_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu::op_mul: {
                    switch (op_size) {
                        case 1: {
                            byte result = al.dest_reg.b0 - al.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = is_mul_overflow(al.src_reg.b0, al.dest_reg.b0) || is_mul_underflow(al.src_reg.b0, al.dest_reg.b0);
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = al.dest_reg.q0 - al.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = is_mul_overflow(al.src_reg.q0, al.dest_reg.q0) || is_mul_underflow(al.src_reg.q0, al.dest_reg.q0);
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = al.dest_reg.h0 - al.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = is_mul_overflow(al.src_reg.h0, al.dest_reg.h0) || is_mul_underflow(al.src_reg.h0, al.dest_reg.h0);
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = al.dest_reg.w0 - al.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = is_mul_overflow(al.src_reg.h0, al.dest_reg.h0) || is_mul_underflow(al.src_reg.h0, al.dest_reg.h0);
                            al.dest_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu::op_div: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            byte result = al.dest_reg.b0 / al.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = al.dest_reg.q0 / al.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = al.dest_reg.h0 / al.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = al.dest_reg.w0 / al.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            al.dest_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu::op_mod: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            byte result = al.dest_reg.b0 % al.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = al.dest_reg.q0 % al.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = al.dest_reg.h0 % al.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = al.dest_reg.w0 % al.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            al.dest_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu::op_test:
                case alu::op_testind:
                case alu::op_and: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            byte result = al.dest_reg.b0 & al.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = al.dest_reg.q0 & al.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = al.dest_reg.h0 & al.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = al.dest_reg.w0 & al.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            al.dest_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu::op_or: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            byte result = al.dest_reg.b0 | al.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = al.dest_reg.q0 | al.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = al.dest_reg.h0 | al.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = al.dest_reg.w0 | al.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            al.dest_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu::op_nor: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            byte result = ~(al.dest_reg.b0 | al.src_reg.b0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = ~(al.dest_reg.q0 | al.src_reg.q0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = ~(al.dest_reg.h0 | al.src_reg.h0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = ~(al.dest_reg.w0 | al.src_reg.w0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            al.dest_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu::op_nand: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            byte result = ~(al.dest_reg.b0 & al.src_reg.b0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = ~(al.dest_reg.q0 & al.src_reg.q0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = ~(al.dest_reg.h0 & al.src_reg.h0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = ~(al.dest_reg.w0 & al.src_reg.w0);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            al.dest_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu::op_xor: {
                    overflow_flag = false;

                    switch (op_size) {
                        case 1: {
                            byte result = al.dest_reg.b0 ^ al.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = al.dest_reg.q0 ^ al.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = al.dest_reg.h0 ^ al.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = al.dest_reg.w0 ^ al.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            al.dest_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu::op_shl: {
                    switch (op_size) {
                        case 1: {
                            byte result = al.dest_reg.b0 << al.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result < al.dest_reg.b0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = al.dest_reg.q0 << al.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result < al.dest_reg.q0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = al.dest_reg.h0 << al.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result < al.dest_reg.h0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = al.dest_reg.w0 << al.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result < al.dest_reg.w0;
                            al.dest_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu::op_shr: {
                    switch (op_size) {
                        case 1: {
                            byte result = al.dest_reg.b0 >> al.src_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result < al.dest_reg.b0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = al.dest_reg.q0 >> al.src_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result < al.dest_reg.q0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = al.dest_reg.h0 >> al.src_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result < al.dest_reg.h0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = al.dest_reg.w0 >> al.src_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result < al.dest_reg.w0;
                            al.dest_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu::op_inc: {
                    switch (op_size) {
                        case 1: {
                            byte result = al.dest_reg.b0 + byte(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result < al.dest_reg.b0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = al.dest_reg.q0 + qword(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result < al.dest_reg.q0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = al.dest_reg.h0 + hword(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result < al.dest_reg.h0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = al.dest_reg.w0 + word(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result < al.dest_reg.w0;
                            al.dest_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu::op_dec: {
                    switch (op_size) {
                        case 1: {
                            byte result = al.dest_reg.b0 - byte(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            overflow_flag = result > al.dest_reg.b0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = al.dest_reg.q0 - qword(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            overflow_flag = result > al.dest_reg.q0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = al.dest_reg.h0 - hword(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            overflow_flag = result > al.dest_reg.h0;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = al.dest_reg.w0 - word(1);
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            overflow_flag = result > al.dest_reg.w0;
                            al.dest_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }

                case alu::op_not: {
                    overflow_flag = 0;

                    switch (op_size) {
                        case 1: {
                            byte result = ~al.dest_reg.b0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 2: {
                            qword result = ~al.dest_reg.q0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 4: {
                            hword result = ~al.dest_reg.h0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x80000000;
                            al.dest_reg.w0 = result;
                            break;
                        }

                        case 8: {
                            word result = ~al.dest_reg.w0;
                            zero_flag = result == 0;
                            negative_flag = result & 0x8000000000000000;
                            al.dest_reg.w0 = result;
                            break;
                        }
                    }

                    break;
                }
            }
        }

        /* This is the state machine that implements the machine-code instructions. */
        void tick() {
            std::vector<byte> instr_data;
            running_flag = true;

            while (running_flag) {
                switch (run_state) {
                    case run_states::decode: {
                        switch (cycle) {
                            case 0: {
                                mm.read(regs::p.h0, regs::in, subreg_enum::w0);
                                ++regs::p.h0;
                                run_state = run_states::execute;
                                step = 0;
                                break;
                            }
                        }

                        ++cycle;
                        break;
                    }

                    case run_states::execute: {
                        ++cycle;

                        switch (regs::in.b0) {
                            case instr::halt_opcode: {
                                switch (step) {
                                    case 0: {
                                        running_flag = false;
                                        is_power_on = false; // just temporary
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::clr_regVal: {
                                switch (step) {
                                    case 0:
                                        regs::p.increment(1);
                                        clr_reg(src_reg(), src_subreg_flag());
                                        instr_complete();
                                        break;
                                    }

                                break;
                            }

                            case instr::ld_regVal_reg: {
                                switch (step) {
                                    case 0: {
                                        regs::p.h0 += 2;
                                        copy_regval_reg(src_reg(), src_subreg_flag(), dest_reg(), dest_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::ld_immVal_reg: {
                                switch (step) {
                                    case 0: {
                                        regs::p.h0 += 2;
                                        hword imm_size = src_imm_size();
                                        copy_immval_reg(regs::p.h0, imm_size, dest_reg(), dest_subreg_flag());
                                        regs::p.h0 += imm_size;
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::ld_regAddr_reg: {
                                switch (step) {
                                    case 0: {
                                        regs::p.h0 += 2;
                                        copy_regaddr_reg(src_reg(), src_subreg_flag(), dest_reg(), dest_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::ld_immAddr_reg: {
                                switch (step) {
                                    case 0: {
                                        regs::p.h0 += 2;
                                        hword imm_size = src_imm_size();
                                        hword dest_size = dest_subreg_size();
                                        copy_immaddr_reg(regs::p.h0, dest_size, dest_reg(), dest_subreg_flag());
                                        regs::p.h0 += imm_size;

                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::st_regVal_regAddr: {
                                switch (step) {
                                    case 0: {
                                        regs::p.h0 += 2;
                                        copy_regval_regaddr(src_reg(), src_subreg_flag(), dest_reg(), dest_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::st_immVal_regAddr: {
                                switch (step) {
                                    case 0: {
                                        regs::p.h0 += 2;
                                        hword imm_size = src_imm_size();
                                        copy_immval_regaddr(regs::p.h0, imm_size, dest_reg(), dest_subreg_flag());
                                        regs::p.h0 += imm_size;
                                        instr_complete();
                                        break;
                                    }
                                }

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
                            case instr::test_regVal_reg:
                            {
                                switch (step) {
                                    case 0: {
                                        regs::p.h0 += 2;
                                        al.b0 = regs::in.b0;
                                        copy_regval_reg(src_reg(), src_subreg_flag(), al.src_reg, subreg_enum::w0);
                                        copy_regval_reg(dest_reg(), dest_subreg_flag(), al.dest_reg, subreg_enum::w0);
                                        al.b1 = src_subreg_size();
                                        al.b2 = dest_subreg_size();
                                        run_alu();
                                        copy_regval_reg(al.dest_reg, subreg_enum::w0, dest_reg(), dest_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
                                }

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
                            case instr::test_immVal_reg:
                            {
                                switch (step) {
                                    case 0: {
                                        regs::p.h0 += 2;
                                        al.b0 = regs::in.b0;
                                        byte src_size = src_imm_size();
                                        copy_immval_reg(regs::p.h0, src_size, al.src_reg, subreg_enum::w0);
                                        copy_regval_reg(dest_reg(), dest_subreg_flag(), al.dest_reg, subreg_enum::w0);
                                        al.b1 = src_size;
                                        al.b2 = dest_subreg_size();
                                        run_alu();
                                        regs::p.h0 += src_size;
                                        copy_regval_reg(al.dest_reg, subreg_enum::w0, dest_reg(), dest_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
                                }

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
                            case instr::test_regAddr_reg:
                            {
                                switch (step) {
                                    case 0: {
                                        regs::p.h0 += 2;
                                        al.b0 = regs::in.b0;
                                        copy_regaddr_reg(src_reg(), src_subreg_flag(), al.src_reg, subreg_enum::w0);
                                        copy_regaddr_reg(dest_reg(), dest_subreg_flag(), al.dest_reg, subreg_enum::w0);
                                        al.b1 = src_subreg_size();
                                        al.b2 = dest_subreg_size();
                                        run_alu();
                                        copy_regval_reg(al.dest_reg, subreg_enum::w0, dest_reg(), dest_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
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
                                        dest_reg().enable_to_bus(data_bus_1, dest_subreg_flag());
                                        al.set_dest_from_bus(data_bus_1);
                                        al.b2 = dest_subreg_size();
                                        // byte op_size = subreg_size_map[dest_subreg_index()];
                                        instr_jmp_alu();
                                        break;
                                    }

                                    case 2: {
                                        al.enable_dest_to_bus(data_bus_0);
                                        dest_reg().set_from_bus(data_bus_0, dest_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
#endif
                                }

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
                            case instr::test_immAddr_reg:
                            {
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
                                        dest_reg().enable_to_bus(data_bus_1, dest_subreg_flag());
                                        al.b2 = dest_subreg_size();
                                        al.set_src_from_bus(data_bus_0);
                                        al.set_dest_from_bus(data_bus_1);
                                        instr_jmp_alu();
                                        break;
                                    }

                                    case 3: {
                                        al.enable_dest_to_bus(data_bus_0);
                                        dest_reg().set_from_bus(data_bus_0, dest_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::inc_regVal:
                            case instr::dec_regVal:
                            case instr::not_regVal:
                            {
                                switch (step) {
                                    case 0: {
                                        al.b0 = regs::in.b0;
                                        regs::p.increment(1);
                                        src_reg().enable_to_bus(data_bus_0, src_subreg_flag());
                                        al.set_dest_from_bus(data_bus_0);
                                        al.b1 = src_subreg_size();
                                        instr_jmp_alu();
                                        break;
                                    }

                                    case 1: {
                                        al.enable_dest_to_bus(data_bus_0);
                                        src_reg().set_from_bus(data_bus_0, src_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::cmpind_immVal_regAddr:
                            case instr::testind_immVal_regAddr:
                            {
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
                                        dest_reg().enable_to_bus(address_bus, dest_subreg_flag());
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 3: {
                                        mm.enable_memory_to_bus(data_bus_0, dest_subreg_flag());
                                        al.set_dest_from_bus(data_bus_0);
                                        instr_jmp_alu();
                                        break;
                                    }

                                    case 4: {
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::cmpind_regVal_regAddr:
                            case instr::testind_regVal_regAddr:
                            {
                                switch (step) {
                                    case 0: {
                                        al.b0 = regs::in.b0;
                                        byte src_size = src_subreg_size();
                                        al.b1 = src_size;
                                        al.b2 = src_size;
                                        regs::p.increment(2);
                                        src_reg().enable_to_bus(data_bus_0, src_subreg_flag());
                                        dest_reg().enable_to_bus(address_bus, dest_subreg_flag());
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 1: {
                                        al.set_src_from_bus(data_bus_0);
                                        mm.enable_memory_to_bus(data_bus_1, subreg_enum::w0);
                                        al.set_dest_from_bus(data_bus_1);
                                        instr_jmp_alu();
                                        break;
                                    }

                                    case 2: {
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::out_regVal_imm: {
                                switch (step) {
                                    case 0: {
                                        regs::p.increment(2);
                                        regs::p.enable_to_bus(address_bus, subreg_enum::h0);
                                        mm.set_address_from_bus(address_bus);
                                        src_reg().enable_to_bus(data_bus_0, src_subreg_flag());
                                        break;
                                    }

                                    case 1: {
                                        byte dest_size = dest_imm_size();
                                        regs::p.increment(dest_size);
                                        mm.enable_memory_to_bus(data_bus_1, dest_size);
                                        operand1.set_from_bus(data_bus_1, src_subreg_flag());
                                        break;
                                    }

                                    case 2: {
                                        device* pdevice = devices[operand1.q0];
                                        pdevice->set_address_from_bus(data_bus_1);
                                        pdevice->set_io_from_bus(data_bus_0);
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::sys_immVal: {
                                switch (step) {
                                    case 0: {
                                        regs::p.increment(1);
                                        regs::p.enable_to_bus(address_bus, subreg_enum::w0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 1: {
                                        byte src_size = src_imm_size();
                                        regs::p.increment(src_size);
                                        mm.enable_memory_to_bus(data_bus_0, src_size);
                                        operand1.set_from_bus(data_bus_0, subreg_enum::w0);
                                        break;
                                    }

                                    case 2: {
                                        operand1.w0 = sys::call(operand1.b0);
                                        operand1.enable_to_bus(data_bus_0, subreg_enum::w0);
                                        regs::a.set_from_bus(data_bus_0, subreg_enum::w0);
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::sys_regVal: {
                                regs::p.increment(1);
                                operand1.w0 = sys::call(src_reg().b0);
                                operand1.enable_to_bus(data_bus_0, subreg_enum::w0);
                                regs::a.set_from_bus(data_bus_0, subreg_enum::w0);
                                instr_complete();
                                break;
                            }

                            case instr::pop_regVal: {
                                switch (step) {
                                    case 0: {
                                        regs::p.increment(1);
                                        regs::s.enable_to_bus(address_bus, subreg_enum::h0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 1: {
                                        regs::s.increment(src_subreg_size(), subreg_enum::h0);
                                        mm.enable_memory_to_bus(data_bus_0, subreg_enum::w0);
                                        src_reg().set_from_bus(data_bus_0, src_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::push_immVal: {
                                switch (step) {
                                    case 0: {
                                        regs::p.increment(1);
                                        regs::p.enable_to_bus(address_bus, subreg_enum::w0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 1: {
                                        byte src_size = src_imm_size();
                                        regs::p.increment(src_size);
                                        regs::s.decrement(src_size, subreg_enum::h0);
                                        regs::s.enable_to_bus(address_bus, subreg_enum::h0);
                                        mm.set_address_from_bus(address_bus);
                                        mm.enable_memory_to_bus(data_bus_0, src_size);
                                        operand1.set_from_bus(data_bus_0, subreg_enum::w0);
                                        break;
                                    }

                                    case 2: {
                                        operand1.enable_to_bus(data_bus_0, subreg_enum::w0);
                                        mm.set_memory_from_bus(data_bus_0, subreg_enum::w0);
                                        instr_complete();
                                        break;
                                    }
                                }
                                break;
                            }

                            case instr::push_regVal: {
                                switch (step) {
                                    case 0: {
                                        regs::p.increment(1);
                                        regs::s.decrement(src_subreg_size(), subreg_enum::h0);
                                        regs::s.enable_to_bus(address_bus, subreg_enum::h0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 1: {
                                        src_reg().enable_to_bus(data_bus_0, src_subreg_flag());
                                        mm.set_memory_from_bus(data_bus_0, src_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::call_immVal: {
                                switch (step) {
                                    case 0: {
                                        regs::p.increment(1);
                                        regs::p.enable_to_bus(address_bus, subreg_enum::w0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }
                                    case 1: {
                                        byte src_size = src_imm_size();
                                        regs::p.increment(src_size);
                                        mm.enable_memory_to_bus(data_bus_0, src_size);
                                        operand1.set_from_bus(data_bus_0, subreg_enum::w0);
                                        regs::s.decrement(sizeof(hword), subreg_enum::h0);
                                        regs::s.enable_to_bus(address_bus, subreg_enum::h0);
                                        mm.set_address_from_bus(address_bus);
                                        regs::p.enable_to_bus(data_bus_1, subreg_enum::w0);
                                        mm.set_memory_from_bus(data_bus_1, subreg_enum::w0);
                                        break;
                                    }
                                    case 2: {
                                        operand1.enable_to_bus(data_bus_0, subreg_enum::h0);
                                        regs::p.set_from_bus(data_bus_0, subreg_enum::h0);
                                        instr_complete();
                                        break;
                                    }
                                }
                                break;
                            }

                            case instr::ret_opcode: {
                                switch (step) {
                                    case 0: {
                                        regs::s.enable_to_bus(address_bus, subreg_enum::h0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }
                                    case 1: {
                                        regs::s.increment(src_subreg_size(), subreg_enum::h0);
                                        mm.enable_memory_to_bus(data_bus_0, subreg_enum::w0);
                                        regs::p.set_from_bus(data_bus_0, subreg_enum::h0);
                                        instr_complete();
                                        break;
                                    }
                                }
                                break;
                            }

                            case instr::jmp_immVal: {
                                switch (step) {
                                    case 0: {
                                        regs::p.increment(1);
                                        regs::p.enable_to_bus(address_bus, subreg_enum::w0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }
                                    case 1: {
                                        mm.enable_memory_to_bus(data_bus_0, subreg_enum::w0);
                                        regs::p.set_from_bus(data_bus_0, subreg_enum::h0);
                                        instr_complete();
                                        break;
                                    }
                                }
                                break;
                            }

                            case instr::jz_immVal: {
                                switch (step) {
                                    case 0: {
                                        regs::p.increment(1);
                                        regs::p.enable_to_bus(address_bus, subreg_enum::w0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }
                                    case 1: {
                                        if (cpu::zero_flag) {
                                            mm.enable_memory_to_bus(data_bus_0, subreg_enum::w0);
                                            regs::p.set_from_bus(data_bus_0, subreg_enum::h0);
                                        }
                                        else {
                                            byte src_size = src_imm_size();
                                            regs::p.increment(src_size);
                                        }
                                        instr_complete();
                                        break;
                                    }
                                }
                                break;
                            }

                            default: {
                                std::stringstream err;
                                err << "unknown opcode: " << std::hex << regs::in.b0;
                                throw std::exception(err.str().c_str());
                                break;
                            }
                        }

                        ++step;
                        break;
                    }

                    case run_states::alu: {
                        run_alu();
                        run_state = run_states::execute;

                        /* Go back to the top of the loop, skipping the increment/enable/set steps
                        below. That means that the ALU operations CANNOT do anything that would
                        require these steps to execute. */
                        continue;
                    }
                }

                /* increment */
                if (increment_array.size()) {
                    for (auto& info : increment_array) {
                        switch (info.first.second) {
                            case subreg_enum::b0: {
                                info.first.first->b0 = info.first.first->b0 + info.second;
                                break;
                            }

                            case subreg_enum::b1: {
                                info.first.first->b1 = info.first.first->b1 + info.second;
                                break;
                            }

                            case subreg_enum::b2: {
                                info.first.first->b2 = info.first.first->b2 + info.second;
                                break;
                            }

                            case subreg_enum::b3: {
                                info.first.first->b3 = info.first.first->b3 + info.second;
                                break;
                            }

                            case subreg_enum::b4: {
                                info.first.first->b4 = info.first.first->b4 + info.second;
                                break;
                            }

                            case subreg_enum::b5: {
                                info.first.first->b5 = info.first.first->b5 + info.second;
                                break;
                            }

                            case subreg_enum::b6: {
                                info.first.first->b6 = info.first.first->b6 + info.second;
                                break;
                            }

                            case subreg_enum::b7: {
                                info.first.first->b7 = info.first.first->b7 + info.second;
                                break;
                            }

                            case subreg_enum::q0: {
                                info.first.first->q0 = info.first.first->q0 + info.second;
                                break;
                            }

                            case subreg_enum::q1: {
                                info.first.first->q1 = info.first.first->q1 + info.second;
                                break;
                            }

                            case subreg_enum::q2: {
                                info.first.first->q2 = info.first.first->q2 + info.second;
                                break;
                            }

                            case subreg_enum::q3: {
                                info.first.first->q3 = info.first.first->q3 + info.second;
                                break;
                            }

                            case subreg_enum::h0: {
                                info.first.first->h0 = info.first.first->h0 + info.second;
                                break;
                            }

                            case subreg_enum::h1: {
                                info.first.first->h1 = info.first.first->h1 + info.second;
                                break;
                            }

                            case subreg_enum::w0: {
                                info.first.first->w0 = info.first.first->w0 + info.second;
                                break;
                            }

                        }

                    }

                    increment_array.clear();
                }

                /* enable memory to bus */
                if (mm.enable_memory_scheduled()) {
                    mm.on_enable_memory();
                }

                if (bus_enable_array.size()) {
                    for (auto& info : bus_enable_array) {
                        info.preg->on_enable();
                        info.pbus->w0 = (info.preg->w0 & (word)info.mask) >> info.offset;
                    }

                    bus_enable_array.clear();
                }

                if (bus_set_array.size()) {
                    for (auto& info : bus_set_array) {
                        info.preg->w0 = (~(word)info.mask & info.preg->w0) | (info.pbus->w0 << info.offset) & (word)info.mask;
                        info.preg->on_set();
                    }

                    bus_set_array.clear();
                }

                /* set memory from buses*/
                if (mm.set_memory_scheduled()) {
                    mm.on_set_memory();
                }
            }
        }

    } // namespace cpu; 

} // namespace maize
