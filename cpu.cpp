#include "maize_cpu.h"
#include "maize_sys.h"

namespace maize {
    namespace cpu {

        namespace {
            const byte opcode_flag = 0b11000000;
            const byte opcode_flag_srcImm = 0b01000000;
            const byte opcode_flag_srcAddr = 0b10000000;

            const byte opflag_reg = 0b11110000;
            const byte opflag_reg_a = 0b00000000;
            const byte opflag_reg_b = 0b00010000;
            const byte opflag_reg_c = 0b00100000;
            const byte opflag_reg_d = 0b00110000;
            const byte opflag_reg_e = 0b01000000;
            const byte opflag_reg_g = 0b01010000;
            const byte opflag_reg_h = 0b01100000;
            const byte opflag_reg_j = 0b01110000;
            const byte opflag_reg_k = 0b10000000;
            const byte opflag_reg_l = 0b10010000;
            const byte opflag_reg_m = 0b10100000;
            const byte opflag_reg_z = 0b10110000;
            const byte opflag_reg_f = 0b11000000;
            const byte opflag_reg_in = 0b11010000;
            const byte opflag_reg_p = 0b11100000;
            const byte opflag_reg_s = 0b11110000;

            const byte opflag_reg_sp = 0b11111100; // SP.H0 = stack pointer
            const byte opflag_reg_bp = 0b11111101; // SP.H1 = base pointer
            const byte opflag_reg_pc = 0b11101100; // PC.H0 = program counter
            const byte opflag_reg_cs = 0b11101101; // PC.H1 = program segment
            const byte opflag_reg_fl = 0b11001100; // FL.H0 = flags

            const byte opflag_subreg = 0b00001111;
            const byte opflag_subreg_b0 = 0b00000000;
            const byte opflag_subreg_b1 = 0b00000001;
            const byte opflag_subreg_b2 = 0b00000010;
            const byte opflag_subreg_b3 = 0b00000011;
            const byte opflag_subreg_b4 = 0b00000100;
            const byte opflag_subreg_b5 = 0b00000101;
            const byte opflag_subreg_b6 = 0b00000110;
            const byte opflag_subreg_b7 = 0b00000111;
            const byte opflag_subreg_q0 = 0b00001000;
            const byte opflag_subreg_q1 = 0b00001001;
            const byte opflag_subreg_q2 = 0b00001010;
            const byte opflag_subreg_q3 = 0b00001011;
            const byte opflag_subreg_h0 = 0b00001100;
            const byte opflag_subreg_h1 = 0b00001101;
            const byte opflag_subreg_w0 = 0b00001110;

            const byte opflag_imm_size = 0b00000111;
            const byte opflag_imm_size_08b = 0b00000000;
            const byte opflag_imm_size_16b = 0b00000001;
            const byte opflag_imm_size_32b = 0b00000010;
            const byte opflag_imm_size_64b = 0b00000011;

            const byte opflag_imm_reserved_01 = 0b01000000;
            const byte opflag_imm_reserved_02 = 0b01010000;
            const byte opflag_imm_reserved_03 = 0b01100000;
            const byte opflag_imm_reserved_04 = 0b01110000;

                                            //       6         5         4         3         2         1         0
                                            //    3210987654321098765432109876543210987654321098765432109876543210
            const word bit_carryout = 0b0000000000000000000000000000000000000000000000000000000000000001;
            const word bit_negative = 0b0000000000000000000000000000000000000000000000000000000000000010;
            const word bit_overflow = 0b0000000000000000000000000000000000000000000000000000000000000100;
            const word bit_parity = 0b0000000000000000000000000000000000000000000000000000000000001000;
            const word bit_zero = 0b0000000000000000000000000000000000000000000000000000000000010000;
            const word bit_sign = 0b0000000000000000000000000000000000000000000000000000000000100000;
            const word bit_reserved = 0b0000000000000000000000000000000000000000000000000000000001000000;
            const word bit_privilege = 0b0000000000000000000000000000000100000000000000000000000000000000;
            const word bit_interrupt_enabled = 0b0000000000000000000000000000001000000000000000000000000000000000;
            const word bit_interrupt_set = 0b0000000000000000000000000000010000000000000000000000000000000000;
            const word bit_running = 0b0000000000000000000000000000100000000000000000000000000000000000;

            /* Maps the value of a subreg_enum to the size, in bytes, of the corresponding subregister. */
            const size_t subreg_size_map[] = {
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
            const byte subreg_offset_map[] = {
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
                0   // w0
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
                subreg_mask_enum mask {0};
                byte offset {0};
            };

            std::vector<reg_op_info> bus_enable_array;
            std::vector<reg_op_info> bus_set_array;
            std::vector<std::pair<std::pair<reg*, subreg_enum>, int8_t>> increment_array;
            std::map<qword, device*> devices;
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
                            *((qword*)(cache + idx)) = q0;
                        }
                        else {
                            cache[cache_address.b0] = b0;
                            set_cache_address(++address.h0);
                            cache[cache_address.b0] = b1;
                        }

                        break;

                    case subreg_mask_enum::q1:
                        if (rem >= 2) {
                            *((qword*)(cache + idx)) = q1;
                        }
                        else {
                            cache[cache_address.b0] = b2;
                            set_cache_address(++address.h0);
                            cache[cache_address.b0] = b3;
                        }

                        break;

                    case subreg_mask_enum::q2:
                        if (rem >= 2) {
                            *((qword*)(cache + idx)) = q2;
                        }
                        else {
                            cache[cache_address.b0] = b4;
                            set_cache_address(++address.h0);
                            cache[cache_address.b0] = b5;
                        }

                        break;

                    case subreg_mask_enum::q3:
                        if (rem >= 2) {
                            *((qword*)(cache + idx)) = q3;
                        }
                        else {
                            cache[cache_address.b0] = b6;
                            set_cache_address(++address.h0);
                            cache[cache_address.b0] = b7;
                        }

                        break;

                    case subreg_mask_enum::h0:
                        if (rem >= 4) {
                            *((hword*)(cache + idx)) = h0;
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
                            *((hword*)(cache + idx)) = h1;
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
                            *((word*)(cache + idx)) = w0;
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
        }

        void memory_module::write(reg_value address, byte value) {
            set_cache_address(address);
            cache[cache_address.b0] = value;
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

        size_t memory_module::set_cache_address(reg_value address) {
            if (cache_base != (address & address_mask)) {
                cache_base = address & address_mask;

                if (!memory_map.contains(cache_base)) {
                    memory_map[cache_base] = new byte[0x100] {0};
                }

                cache = memory_map[cache_base];
            }

            cache_address.w0 = address;
            return 0x100ULL - cache_address.b0;
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
            set_from_bus(source_bus, subreg);
        }

        void alu::enable_dest_to_bus(bus& dest_bus, subreg_enum subreg) {
            enable_to_bus(dest_bus, subreg);
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
            reg fl; // flags register
            reg in; // instruction register
            reg pc {0x0000000000001000}; // program execution register
            reg sp; // stack register
        }

        namespace {
            flag<bit_carryout> carryout_flag {regs::fl};
            flag<bit_negative> negative_flag {regs::fl};
            flag<bit_overflow> overflow_flag {regs::fl};
            flag<bit_parity> parity_flag {regs::fl};
            flag<bit_zero> zero_flag {regs::fl};
            flag<bit_sign> sign_flag {regs::fl};
            flag<bit_privilege> privilege_flag {regs::fl};
            flag<bit_interrupt_enabled> interrupt_enabled_flag {regs::fl};
            flag<bit_interrupt_set> interrupt_set_flag {regs::fl};
            flag<bit_running> running_flag {regs::fl};

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
                &regs::fl,
                &regs::in,
                &regs::pc,
                &regs::sp
            };

            size_t src_imm_size_flag() {
                return regs::in.b1 & opflag_imm_size;
            }

            size_t src_imm_size() {
                return size_t(1) << (regs::in.b1 & opflag_imm_size);
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

            size_t src_subreg_size() {
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

            size_t dest_subreg_size() {
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

        /* This is the state machine that implements the machine-code instructions. */
        void tick() {
            running_flag = true;

            while (running_flag) {
                switch (run_state) {
                    case run_states::decode: {
                        switch (cycle) {
                            case 0: {
                                regs::pc.enable_to_bus(address_bus, subreg_enum::h0);
                                mm.set_address_from_bus(address_bus);
                                break;
                            }

                            case 1: {
                                regs::pc.increment(1);
                                mm.enable_memory_to_bus(data_bus_0, 8);
                                regs::in.set_from_bus(data_bus_0, subreg_enum::w0);
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
                            case instr::halt: {
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

                            case instr::ld_regVal_reg: {
                                switch (step) {
                                    case 0: {
                                        regs::pc.increment(2);
                                        src_reg().enable_to_bus(data_bus_0, src_subreg_flag());
                                        dest_reg().set_from_bus(data_bus_0, dest_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::ld_immVal_reg: {
                                switch (step) {
                                    case 0: {
                                        regs::pc.increment(2);
                                        regs::pc.enable_to_bus(address_bus, subreg_enum::w0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 1: {
                                        size_t src_size = src_imm_size();
                                        regs::pc.increment(src_size);
                                        mm.enable_memory_to_bus(data_bus_0, src_size);
                                        dest_reg().set_from_bus(data_bus_0, dest_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::ld_regAddr_reg: {
                                switch (step) {
                                    case 0: {
                                        regs::pc.increment(2);
                                        src_reg().enable_to_bus(address_bus, src_subreg_flag());
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 1: {
                                        mm.enable_memory_to_bus(data_bus_0, subreg_enum::w0);
                                        dest_reg().set_from_bus(data_bus_0, dest_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::ld_immAddr_reg: {
                                switch (step) {
                                    case 0: {
                                        regs::pc.increment(2);
                                        regs::pc.enable_to_bus(address_bus, subreg_enum::w0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 1: {
                                        regs::pc.increment(src_imm_size());
                                        mm.enable_memory_to_bus(address_bus, subreg_enum::w0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 2: {
                                        mm.enable_memory_to_bus(data_bus_0, subreg_enum::w0);
                                        dest_reg().set_from_bus(data_bus_0, dest_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::st_regVal_regAddr: {
                                switch (step) {
                                    case 0: {
                                        regs::pc.increment(2);
                                        src_reg().enable_to_bus(data_bus_0, src_subreg_flag());
                                        dest_reg().enable_to_bus(address_bus, dest_subreg_flag());
                                        mm.set_address_from_bus(address_bus);
                                        mm.set_memory_from_bus(data_bus_0, src_subreg_flag());
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
                            {
                                switch (step) {
                                    case 0: {
                                        regs::pc.increment(2);
                                        src_reg().enable_to_bus(data_bus_0, src_subreg_flag());
                                        dest_reg().enable_to_bus(data_bus_1, dest_subreg_flag());
                                        al.set_src_from_bus(data_bus_0);
                                        al.set_dest_from_bus(data_bus_1);
                                        instr_jmp_alu();
                                        break;
                                    }

                                    case 1: {
                                        al.enable_dest_to_bus(data_bus_0);
                                        dest_reg().set_from_bus(data_bus_0, dest_subreg_flag());
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
                            {
                                switch (step) {
                                    case 0: {
                                        regs::pc.increment(2);
                                        regs::pc.enable_to_bus(address_bus, subreg_enum::w0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 1: {
                                        size_t src_size = src_imm_size();
                                        regs::pc.increment(src_size);
                                        mm.enable_memory_to_bus(data_bus_0, src_size);
                                        dest_reg().enable_to_bus(data_bus_1, dest_subreg_flag());
                                        al.set_src_from_bus(data_bus_0);
                                        al.set_dest_from_bus(data_bus_1);
                                        instr_jmp_alu();
                                        break;
                                    }

                                    case 2: {
                                        al.enable_dest_to_bus(data_bus_0);
                                        dest_reg().set_from_bus(data_bus_0, dest_subreg_flag());
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
                            {
                                switch (step) {
                                    case 0: {
                                        regs::pc.increment(2);
                                        src_reg().enable_to_bus(address_bus, src_subreg_flag());
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 1: {
                                        mm.enable_memory_to_bus(data_bus_0, subreg_enum::w0);
                                        dest_reg().enable_to_bus(data_bus_1, dest_subreg_flag());
                                        al.set_src_from_bus(data_bus_0);
                                        al.set_dest_from_bus(data_bus_1);
                                        instr_jmp_alu();
                                        break;
                                    }

                                    case 2: {
                                        al.enable_dest_to_bus(data_bus_0);
                                        dest_reg().set_from_bus(data_bus_0, dest_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::add_immAddr_reg:
                            case instr::sub_immAddr_reg:
                            case instr::mul_immAddr_reg:
                            case instr::div_immAddr_reg:
                            case instr::mod_immAddr_reg:
                            {
                                switch (step) {
                                    case 0: {
                                        regs::pc.increment(2);
                                        regs::pc.enable_to_bus(address_bus, subreg_enum::w0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 1: {
                                        regs::pc.increment(src_imm_size());
                                        mm.enable_memory_to_bus(address_bus, subreg_enum::w0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 2: {
                                        mm.enable_memory_to_bus(data_bus_0, subreg_enum::w0);
                                        dest_reg().enable_to_bus(data_bus_1, dest_subreg_flag());
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
                                        regs::pc.increment(1);
                                        src_reg().enable_to_bus(data_bus_0, src_subreg_flag());
                                        al.set_src_from_bus(data_bus_0);
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

                            case instr::out_regVal_imm: {
                                switch (step) {
                                    case 0: {
                                        regs::pc.increment(2);
                                        regs::pc.enable_to_bus(address_bus, subreg_enum::h0);
                                        mm.set_address_from_bus(address_bus);
                                        src_reg().enable_to_bus(data_bus_0, src_subreg_flag());
                                        break;
                                    }

                                    case 1: {
                                        size_t dest_size = dest_imm_size();
                                        regs::pc.increment(dest_size);
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
                                        regs::pc.increment(1);
                                        regs::pc.enable_to_bus(address_bus, subreg_enum::w0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 1: {
                                        size_t src_size = src_imm_size();
                                        regs::pc.increment(src_size);
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
                                regs::pc.increment(1);
                                operand1.w0 = sys::call(src_reg().b0);
                                operand1.enable_to_bus(data_bus_0, subreg_enum::w0);
                                regs::a.set_from_bus(data_bus_0, subreg_enum::w0);
                                instr_complete();
                                break;
                            }

                            case instr::pop_regVal: {
                                switch (step) {
                                    case 0: {
                                        regs::pc.increment(1);
                                        regs::sp.enable_to_bus(address_bus, subreg_enum::h0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 1: {
                                        regs::sp.increment(src_subreg_size(), subreg_enum::h0);
                                        mm.enable_memory_to_bus(data_bus_0, subreg_enum::w0);
                                        src_reg().set_from_bus(data_bus_0, src_subreg_flag());
                                        instr_complete();
                                        break;
                                    }
                                }

                                break;
                            }

                            case instr::push_regVal: {
                                switch (step) {
                                    case 0: {
                                        regs::pc.increment(1);
                                        regs::sp.decrement(src_subreg_size(), subreg_enum::h0);
                                        regs::sp.enable_to_bus(address_bus, subreg_enum::h0);
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

                            case instr::push_immVal: {
                                switch (step) {
                                    case 0: {
                                        regs::pc.increment(1);
                                        regs::pc.enable_to_bus(address_bus, subreg_enum::w0);
                                        mm.set_address_from_bus(address_bus);
                                        break;
                                    }

                                    case 1: {
                                        size_t src_size = src_imm_size();
                                        regs::pc.increment(src_size);
                                        regs::sp.decrement(src_size, subreg_enum::h0);
                                        regs::sp.enable_to_bus(address_bus, subreg_enum::h0);
                                        mm.enable_memory_to_bus(data_bus_0, src_size);
                                        mm.set_address_from_bus(address_bus);
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

                        }

                        ++step;
                        break;
                    }

                    case run_states::alu: {
                        byte op_size = subreg_size_map[dest_subreg_index()];

                        switch (regs::in.b0 & alu::opflag_code) {
                            case alu::op_add: {
                                switch (op_size) {
                                    case 1: {
                                        byte result = al.b0 + al.src_reg.b0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80;
                                        overflow_flag = result < al.b0;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 2: {
                                        qword result = al.q0 + al.src_reg.q0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000;
                                        overflow_flag = result < al.q0;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 4: {
                                        hword result = al.h0 + al.src_reg.h0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80000000;
                                        overflow_flag = result < al.h0;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 8: {
                                        word result = al.w0 + al.src_reg.w0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000000000000000;
                                        overflow_flag = result < al.w0;
                                        al.w0 = result;
                                        break;
                                    }
                                }

                                break;
                            }

                            case alu::op_sub: {
                                switch (op_size) {
                                    case 1: {
                                        byte result = al.b0 - al.src_reg.b0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80;
                                        overflow_flag = result > al.b0;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 2: {
                                        qword result = al.q0 - al.src_reg.q0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000;
                                        overflow_flag = result > al.q0;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 4: {
                                        hword result = al.h0 - al.src_reg.h0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80000000;
                                        overflow_flag = result > al.h0;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 8: {
                                        word result = al.w0 - al.src_reg.w0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000000000000000;
                                        overflow_flag = result > al.w0;
                                        al.w0 = result;
                                        break;
                                    }
                                }

                                break;
                            }

                            case alu::op_mul: {
                                switch (op_size) {
                                    case 1: {
                                        byte result = al.b0 - al.src_reg.b0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80;
                                        overflow_flag = alu::is_mul_overflow(al.src_reg.b0, al.b0) || alu::is_mul_underflow(al.src_reg.b0, al.b0);
                                        al.w0 = result;
                                        break;
                                    }

                                    case 2: {
                                        qword result = al.q0 - al.src_reg.q0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000;
                                        overflow_flag = alu::is_mul_overflow(al.src_reg.q0, al.q0) || alu::is_mul_underflow(al.src_reg.q0, al.q0);
                                        al.w0 = result;
                                        break;
                                    }

                                    case 4: {
                                        hword result = al.h0 - al.src_reg.h0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80000000;
                                        overflow_flag = alu::is_mul_overflow(al.src_reg.h0, al.h0) || alu::is_mul_underflow(al.src_reg.h0, al.h0);
                                        al.w0 = result;
                                        break;
                                    }

                                    case 8: {
                                        word result = al.w0 - al.src_reg.w0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000000000000000;
                                        overflow_flag = alu::is_mul_overflow(al.src_reg.h0, al.h0) || alu::is_mul_underflow(al.src_reg.h0, al.h0);
                                        al.w0 = result;
                                        break;
                                    }
                                }

                                break;
                            }

                            case alu::op_div: {
                                overflow_flag = false;

                                switch (op_size) {
                                    case 1: {
                                        byte result = al.b0 / al.src_reg.b0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 2: {
                                        qword result = al.q0 / al.src_reg.q0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 4: {
                                        hword result = al.h0 / al.src_reg.h0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80000000;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 8: {
                                        word result = al.w0 / al.src_reg.w0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000000000000000;
                                        al.w0 = result;
                                        break;
                                    }
                                }

                                break;
                            }

                            case alu::op_mod: {
                                overflow_flag = false;

                                switch (op_size) {
                                    case 1: {
                                        byte result = al.b0 % al.src_reg.b0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 2: {
                                        qword result = al.q0 % al.src_reg.q0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 4: {
                                        hword result = al.h0 % al.src_reg.h0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80000000;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 8: {
                                        word result = al.w0 % al.src_reg.w0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000000000000000;
                                        al.w0 = result;
                                        break;
                                    }
                                }

                                break;
                            }

                            case alu::op_and: {
                                overflow_flag = false;

                                switch (op_size) {
                                    case 1: {
                                        byte result = al.b0 % al.src_reg.b0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 2: {
                                        qword result = al.q0 % al.src_reg.q0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 4: {
                                        hword result = al.h0 % al.src_reg.h0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80000000;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 8: {
                                        word result = al.w0 % al.src_reg.w0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000000000000000;
                                        al.w0 = result;
                                        break;
                                    }
                                }

                                break;
                            }

                            case alu::op_inc: {
                                switch (op_size) {
                                    case 1: {
                                        byte result = al.b0 + byte(1);
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80;
                                        overflow_flag = result < al.b0;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 2: {
                                        qword result = al.q0 + qword(1);
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000;
                                        overflow_flag = result < al.q0;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 4: {
                                        hword result = al.h0 + hword(1);
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80000000;
                                        overflow_flag = result < al.h0;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 8: {
                                        word result = al.w0 + word(1);
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000000000000000;
                                        overflow_flag = result < al.w0;
                                        al.w0 = result;
                                        break;
                                    }
                                }

                                break;
                            }

                            case alu::op_dec: {
                                switch (op_size) {
                                    case 1: {
                                        byte result = al.b0 - byte(1);
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80;
                                        overflow_flag = result > al.b0;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 2: {
                                        qword result = al.q0 - qword(1);
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000;
                                        overflow_flag = result > al.q0;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 4: {
                                        hword result = al.h0 - hword(1);
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80000000;
                                        overflow_flag = result > al.h0;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 8: {
                                        word result = al.w0 - word(1);
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000000000000000;
                                        overflow_flag = result > al.w0;
                                        al.w0 = result;
                                        break;
                                    }
                                }

                                break;
                            }

                            case alu::op_not: {
                                overflow_flag = 0;

                                switch (op_size) {
                                    case 1: {
                                        byte result = ~al.b0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 2: {
                                        qword result = ~al.q0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 4: {
                                        hword result = ~al.h0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x80000000;
                                        al.w0 = result;
                                        break;
                                    }

                                    case 8: {
                                        word result = ~al.w0;
                                        zero_flag = result == 0;
                                        negative_flag = result & 0x8000000000000000;
                                        al.w0 = result;
                                        break;
                                    }
                                }

                                break;
                            }

                        }

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
