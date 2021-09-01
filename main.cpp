#include <iostream>
#include <vector>
#include <map>
#include <array>

namespace maize {
	namespace cpu {
		typedef uint8_t byte;
		typedef uint8_t opcode;

		struct reg_byte {
			uint8_t b0;
			uint8_t b1;
			uint8_t b2;
			uint8_t b3;
			uint8_t b4;
			uint8_t b5;
			uint8_t b6;
			uint8_t b7;
		};

		struct reg_qword {
			uint16_t q0;
			uint16_t q1;
			uint16_t q2;
			uint16_t q3;
		};

		struct reg_hword {
			uint32_t h0;
			uint32_t h1;
		};

		struct reg_word {
			uint64_t w0;
		};

		struct reg_value {
			reg_value() = default;
			explicit reg_value(uint64_t init) : w{init} {}
			explicit reg_value(uint32_t init) : h{init} {}
			explicit reg_value(uint16_t init) : q{init} {}
			explicit reg_value(uint8_t init) : b{init} {}

			uint8_t operator[](size_t index) {
				int shift = index * 8;
				return static_cast<uint8_t>(w.w0 >> shift);
			}

			const uint8_t operator[](size_t index) const {
				int shift = index * 8;
				return static_cast<uint8_t>(w.w0 >> shift);
			}

			uint64_t& w0 {w.w0};
			uint32_t& h0 {h.h0};
			uint32_t& h1 {h.h1};
			uint16_t& q0 {q.q0};
			uint16_t& q1 {q.q1};
			uint16_t& q2 {q.q2};
			uint16_t& q3 {q.q3};
			uint8_t& b0 {b.b0};
			uint8_t& b1 {b.b1};
			uint8_t& b2 {b.b2};
			uint8_t& b3 {b.b3};
			uint8_t& b4 {b.b4};
			uint8_t& b5 {b.b5};
			uint8_t& b6 {b.b6};
			uint8_t& b7 {b.b7};

		protected:
			union {
				reg_byte b;
				reg_qword q;
				reg_hword h;
				reg_word w {0};
			};

		};

		enum class reg_enum {
			a = 0x00,
			b = 0x01,
			c = 0x02,
			d = 0x03,
			e = 0x04,
			g = 0x05,
			h = 0x06,
			j = 0x07,
			k = 0x08,
			l = 0x09,
			m = 0x0A,
			z = 0x0B,
			fl = 0x0C,
			in = 0x0D,
			pc = 0x0E,
			sp = 0x0F
		};

		enum class subreg_enum {
			b0 = 0x00,
			b1 = 0x01,
			b2 = 0x02,
			b3 = 0x03,
			b4 = 0x04,
			b5 = 0x05,
			b6 = 0x06,
			b7 = 0x07,
			q0 = 0x08,
			q1 = 0x09,
			q2 = 0x0A,
			q3 = 0x0B,
			h0 = 0x0C,
			h1 = 0x0D,
			w0 = 0x0E
		};

		enum class subreg_mask_enum : uint64_t {
			b0 = 0b0000000000000000000000000000000000000000000000000000000011111111,
			b1 = 0b0000000000000000000000000000000000000000000000001111111100000000,
			b2 = 0b0000000000000000000000000000000000000000111111110000000000000000,
			b3 = 0b0000000000000000000000000000000011111111000000000000000000000000,
			b4 = 0b0000000000000000000000001111111100000000000000000000000000000000,
			b5 = 0b0000000000000000111111110000000000000000000000000000000000000000,
			b6 = 0b0000000011111111000000000000000000000000000000000000000000000000,
			b7 = 0b1111111100000000000000000000000000000000000000000000000000000000,
			q0 = 0b0000000000000000000000000000000000000000000000001111111111111111,
			q1 = 0b0000000000000000000000000000000011111111111111110000000000000000,
			q2 = 0b0000000000000000111111111111111100000000000000000000000000000000,
			q3 = 0b1111111111111111000000000000000000000000000000000000000000000000,
			h0 = 0b0000000000000000000000000000000011111111111111111111111111111111,
			h1 = 0b1111111111111111111111111111111100000000000000000000000000000000,
			w0 = 0b1111111111111111111111111111111111111111111111111111111111111111,
		};

		const uint8_t opcode_flag = 0b11000000;
		const uint8_t opcode_flag_srcImm = 0b01000000;
		const uint8_t opcode_flag_srcAddr = 0b10000000;

		const uint8_t opflag_reg  = 0b11110000;
		const uint8_t opflag_reg_a = 0b00000000;
		const uint8_t opflag_reg_b = 0b00010000;
		const uint8_t opflag_reg_c = 0b00100000;
		const uint8_t opflag_reg_d = 0b00110000;
		const uint8_t opflag_reg_e = 0b01000000;
		const uint8_t opflag_reg_g = 0b01010000;
		const uint8_t opflag_reg_h = 0b01100000;
		const uint8_t opflag_reg_j = 0b01110000;
		const uint8_t opflag_reg_k = 0b10000000;
		const uint8_t opflag_reg_l = 0b10010000;
		const uint8_t opflag_reg_m = 0b10100000;
		const uint8_t opflag_reg_z = 0b10110000;
		const uint8_t opflag_reg_f = 0b11000000;
		const uint8_t opflag_reg_in = 0b11010000;
		const uint8_t opflag_reg_p = 0b11100000;
		const uint8_t opflag_reg_s = 0b11110000;

		const uint8_t opflag_reg_sp = 0b11111100; // S.H0 = stack pointer
		const uint8_t opflag_reg_bp = 0b11111101; // S.H1 = base pointer
		const uint8_t opflag_reg_pc = 0b11101100; // P.H0 = program counter
		const uint8_t opflag_reg_cs = 0b11101101; // P.H1 = program segment
		const uint8_t opflag_reg_fl = 0b11001100; // F.H0 = flags

		const uint8_t opflag_subreg =	 0b00001111;
		const uint8_t opflag_subreg_b0 = 0b00000000;
		const uint8_t opflag_subreg_b1 = 0b00000001;
		const uint8_t opflag_subreg_b2 = 0b00000010;
		const uint8_t opflag_subreg_b3 = 0b00000011;
		const uint8_t opflag_subreg_b4 = 0b00000100;
		const uint8_t opflag_subreg_b5 = 0b00000101;
		const uint8_t opflag_subreg_b6 = 0b00000110;
		const uint8_t opflag_subreg_b7 = 0b00000111;
		const uint8_t opflag_subreg_q0 = 0b00001000;
		const uint8_t opflag_subreg_q1 = 0b00001001;
		const uint8_t opflag_subreg_q2 = 0b00001010;
		const uint8_t opflag_subreg_q3 = 0b00001011;
		const uint8_t opflag_subreg_h0 = 0b00001100;
		const uint8_t opflag_subreg_h1 = 0b00001101;
		const uint8_t opflag_subreg_w0 = 0b00001110;

		const uint8_t opflag_imm_size = 0b00000111;
		const uint8_t opflag_imm_size_08b = 0b00000000;
		const uint8_t opflag_imm_size_16b = 0b00000001;
		const uint8_t opflag_imm_size_32b = 0b00000010;
		const uint8_t opflag_imm_size_64b = 0b00000011;

		const uint8_t opflag_imm_reserved_01 = 0b01000000;
		const uint8_t opflag_imm_reserved_02 = 0b01010000;
		const uint8_t opflag_imm_reserved_03 = 0b01100000;
		const uint8_t opflag_imm_reserved_04 = 0b01110000;

		uint8_t offset_map[] = {
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

		const subreg_enum imm_size_subreg_map[] = {
			subreg_enum::b0,
			subreg_enum::q0,
			subreg_enum::h0,
			subreg_enum::w0
		};

		subreg_mask_enum subreg_mask_map[] = {
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

		int size_map[] = {
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
			8
		};

		struct reg;
		static const size_t max_increment_count = 32;
		size_t increment_count{0};
		std::pair<reg*, int64_t> increment_array[max_increment_count];

		struct reg_op_info {
			reg_op_info() = default;

			reg* op_reg{nullptr};
			subreg_mask_enum mask{0};
			uint8_t offset{0};
		};

		static const size_t max_store_count = 32;
		size_t store_count{0};
		reg_op_info store_array[max_store_count];

		static const size_t max_load_count = 32;
		size_t load_count{0};
		reg_op_info load_array[max_load_count];

		struct reg : public reg_value {
			reg() : reg_value() {}
			reg(uint64_t init) : reg_value(init) {}

			operator uint64_t() { return w.w0; }
			explicit operator uint32_t() { return h.h0; }
			explicit operator uint16_t() { return q.q0; }
			explicit operator uint8_t() { return b.b0; }

			uint64_t operator=(uint64_t value) { return w.w0 = value; }

			uint64_t& operator++() {
				increment();
				return w0;
			}

			uint64_t operator++(int) {
				increment();
				return w0;
			}

			uint64_t operator--() {
				decrement();
				return w0;
			}

			uint64_t operator--(int) {
				decrement();
				return w0;
			}

			void increment(int64_t value = 1) {
				if (increment_count < max_increment_count) {
					auto& info = increment_array[increment_count];
					info.first = this;
					info.second = value;
					++increment_count;
				}
			}

			void decrement(int64_t value = 1) {
				if (increment_count < max_increment_count) {
					auto& info = increment_array[increment_count];
					info.first = this;
					info.second = -value;
					++increment_count;
				}
			}

		protected:
			uint64_t privilege_flags {0};
			uint64_t privilege_mask {0};
		};

		class bus : public reg {
		public:
			bus() = default;

		protected:
			reg* enabled_reg{nullptr};
			subreg_mask_enum enable_subreg_mask{0};
			uint8_t enable_offset{0};

			static const size_t max_bus_set = 16;
			size_t set_count{0};
			reg_op_info bus_set_array[max_bus_set];

		public:
			void enable_reg(reg* pen_reg, subreg_enum subreg) {
				enable_reg(*pen_reg, subreg);
			}

			void enable_reg(reg& en_reg, subreg_enum subreg) {
				enable_subreg_mask = subreg_mask_map[static_cast<size_t>(subreg)];
				enable_offset = offset_map[static_cast<size_t>(subreg)];
				enabled_reg = &en_reg;
			}

			void set_reg(reg* pset_reg, subreg_enum subreg) {
				set_reg(*pset_reg, subreg);
			}

			void set_reg(reg& set_reg, subreg_enum subreg) {
				if (set_count < max_bus_set) {
					bus_set_array[set_count].op_reg = &set_reg;
					bus_set_array[set_count].mask = subreg_mask_map[static_cast<size_t>(subreg)];
					bus_set_array[set_count].offset = offset_map[static_cast<size_t>(subreg)];
					++set_count;
				}
			}

			void on_enable() {
				if (enabled_reg) {
					w0 = (enabled_reg->w0 & (uint64_t)enable_subreg_mask) >> enable_offset;
					enabled_reg = nullptr;
				}
			}

			void on_set() {
				// privilege_check();
				for (size_t idx = 0; idx < set_count; ++idx) {
					auto & info = bus_set_array[idx];
					info.op_reg->w0 = (~(uint64_t)info.mask & info.op_reg->w0) | (w0 << info.offset) & (uint64_t)info.mask;
				}

				set_count = 0;
			}
		};

		bus address_bus;
		bus data_bus;
		bus io_bus;

		struct memory_module : public reg {
			reg address_reg{0};
			reg data_reg{0};
			std::map<uint64_t, uint8_t*> memory_map;
			uint8_t* cache{nullptr};
			uint64_t cache_base{0xFFFFFFFFFFFFFFFF};
			reg_value cache_address;
			// reg_value tmp;

			void write_byte(uint64_t address, uint8_t value) {
				b0 = value;
				write(address, b0);
			}

			uint8_t read_byte(uint64_t address) {
				b0 = read(address);
				return b0;
			}

			void write_quarter_word(uint64_t address, uint16_t value) {
				q0 = value;
				write(address, b0);
				++address;
				write(address, b1);
			}

			uint16_t read_qword(uint64_t address) {
				b0 = read(address);
				++address;
				b1 = read(address);
				return q0;
			}

			void write_half_word(uint64_t address, uint32_t value) {
				h0 = value;
				write(address, b0);
				++address;
				write(address, b1);
				++address;
				write(address, b2);
				++address;
				write(address, b3);
			}

			uint32_t read_hword(uint64_t address) {
				b0 = read(address);
				++address;
				b1 = read(address);
				++address;
				b2 = read(address);
				++address;
				b3 = read(address);
				return h0;
			}

			void write_word(uint64_t address, uint64_t value) {
				w0 = value;
				write(address, b0);
				++address;
				write(address, b1);
				++address;
				write(address, b2);
				++address;
				write(address, b3);
				++address;
				write(address, b4);
				++address;
				write(address, b5);
				++address;
				write(address, b6);
				++address;
				write(address, b7);
			}

			uint64_t read_word(uint64_t address) {
				b0 = read(address);
				++address;
				b1 = read(address);
				++address;
				b2 = read(address);
				++address;
				b3 = read(address);
				++address;
				b4 = read(address);
				++address;
				b5 = read(address);
				++address;
				b6 = read(address);
				++address;
				b7 = read(address);
				return w0;
			}

			size_t load_size{ 3 };
			subreg_mask_enum store_mask{ subreg_mask_enum::w0 };
			bus* pload_bus {nullptr};
			bus* pstore_bus {nullptr};

			void set_address(bus& source_bus) {
				// MemoryModule.AddressRegister.SetFromAddressBus(SubRegister.H0);
				source_bus.set_reg(address_reg, subreg_enum::h0);
			}

			void load(bus& load_bus, size_t size) {
				// MemoryModule.DataRegister.EnableToDataBus(SubRegister.W0);
				pload_bus = &load_bus;
				load_size = size;
			}

			void on_load() {
				if (pload_bus) {
					data_reg.w0 = 0;

					switch (load_size) {
						case 1:
							data_reg.b0 = read_byte(address_reg.w0);
							pload_bus->enable_reg(data_reg, subreg_enum::b0);
							break;

						case 2:
							data_reg.q0 = read_qword(address_reg.w0);
							pload_bus->enable_reg(data_reg, subreg_enum::q0);
							break;

						case 4:
							data_reg.h0 = read_hword(address_reg.w0);
							pload_bus->enable_reg(data_reg, subreg_enum::h0);
							break;

						case 8:
							data_reg.w0 = read_word(address_reg.w0);
							pload_bus->enable_reg(data_reg, subreg_enum::w0);
							break;

					}

					pload_bus = nullptr;
				}
			}

			void store(bus& store_bus, subreg_enum subreg) {
				// MemoryModule.DataRegister.SetFromDataBus((SubRegister)src_subreg_flag);
				pstore_bus = &store_bus;
				store_mask = subreg_mask_map[static_cast<size_t>(subreg)];
				store_bus.set_reg(data_reg, subreg);
			}

			void on_store() {
				if (pstore_bus) {
					pstore_bus = nullptr;

					switch (store_mask) {
					case subreg_mask_enum::b0:
							write_byte(address_reg.w0, data_reg.b0);
							break;

						case subreg_mask_enum::b1:
							write_byte(address_reg.w0, data_reg.b1);
							break;

						case subreg_mask_enum::b2:
							write_byte(address_reg.w0, data_reg.b2);
							break;

						case subreg_mask_enum::b3:
							write_byte(address_reg.w0, data_reg.b3);
							break;

						case subreg_mask_enum::b4:
							write_byte(address_reg.w0, data_reg.b4);
							break;

						case subreg_mask_enum::b5:
							write_byte(address_reg.w0, data_reg.b5);
							break;

						case subreg_mask_enum::b6:
							write_byte(address_reg.w0, data_reg.b6);
							break;

						case subreg_mask_enum::b7:
							write_byte(address_reg.w0, data_reg.b7);
							break;

						case subreg_mask_enum::q0:
							write_quarter_word(address_reg.w0, data_reg.q0);
							break;

						case subreg_mask_enum::q1:
							write_quarter_word(address_reg.w0, data_reg.q1);
							break;

						case subreg_mask_enum::q2:
							write_quarter_word(address_reg.w0, data_reg.q2);
							break;

						case subreg_mask_enum::q3:
							write_quarter_word(address_reg.w0, data_reg.q3);
							break;

						case subreg_mask_enum::h0:
							write_half_word(address_reg.w0, data_reg.h0);
							break;

						case subreg_mask_enum::h1:
							write_half_word(address_reg.w0, data_reg.h1);
							break;

						case subreg_mask_enum::w0:
							write_word(address_reg.w0, data_reg.w0);
							break;

					}
				}
			}

		protected:
			void set_cache_address(uint64_t address) {
				uint64_t address_base = address >> 8;

				if (address_base != cache_base) {
					if (!memory_map.contains(address_base)) {
						memory_map[address_base] = new uint8_t[0x100]{ 0 };
					}

					cache = memory_map[address_base];
					cache_base = address_base;
				}

				cache_address.w0 = address;
			}

			void write(uint64_t address, uint8_t value) {
				set_cache_address(address);
				cache[cache_address.b0] = value;
			}

			uint8_t read(uint64_t address) {
				set_cache_address(address);
				return cache[cache_address.b0];
			}

		};

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

		template <uint64_t flag_bit> class flag{
		public:
			flag(reg& reg_init, bool value = false) : flag_reg{reg_init} {
				set(value);
			}

			bool get() {
				return (flag_reg.w0 & flag_bit);
			}

			void set(bool value) {
				flag_reg.w0 = ((flag_reg.w0 & ~flag_bit) | (value ? flag_bit : 0));
			}

			flag& operator=(const flag& that) {
				if (this == that) {
					return *this;
				}

				set(that.value);
				return *this;
			}

			flag& operator=(bool value) {
				set(value);
				return *this;
			}

			bool operator==(bool value) {
				return get() == value;
			}

			operator bool() {
				return get();
			}

		private:
			reg& flag_reg;
		};

											//       6         5         4         3         2         1         0
											//    3210987654321098765432109876543210987654321098765432109876543210
		const uint64_t flag_carryout =          0b0000000000000000000000000000000000000000000000000000000000000001;
		const uint64_t flag_negative =          0b0000000000000000000000000000000000000000000000000000000000000010;
		const uint64_t flag_overflow =          0b0000000000000000000000000000000000000000000000000000000000000100;
		const uint64_t flag_parity =            0b0000000000000000000000000000000000000000000000000000000000001000;
		const uint64_t flag_zero =              0b0000000000000000000000000000000000000000000000000000000000010000;
		const uint64_t flag_sign =              0b0000000000000000000000000000000000000000000000000000000000100000;
		const uint64_t flag_reserved =          0b0000000000000000000000000000000000000000000000000000000001000000;
		const uint64_t flag_privilege =         0b0000000000000000000000000000000100000000000000000000000000000000;
		const uint64_t flag_interrupt_enabled = 0b0000000000000000000000000000001000000000000000000000000000000000;
		const uint64_t flag_interrupt_set =     0b0000000000000000000000000000010000000000000000000000000000000000;
		const uint64_t flag_running =           0b0000000000000000000000000000100000000000000000000000000000000000;

		flag<flag_carryout> carryout_flag {fl};
		flag<flag_negative> negative_flag {fl};
		flag<flag_overflow> overflow_flag {fl};
		flag<flag_parity> parity_flag {fl};
		flag<flag_zero> zero_flag {fl};
		flag<flag_sign> sign_flag {fl};
		flag<flag_privilege> privilege_flag {fl};
		flag<flag_interrupt_enabled> interrupt_enabled_flag {fl};
		flag<flag_interrupt_set> interrupt_set_flag {fl};
		flag<flag_running> running_flag {fl};

		std::array<reg*, 16> reg_map {
			&a, &b, &c, &d, &e, &g, &h, &j, &k, &l, &m, &z, &fl, &in, &pc, &sp
		};

		memory_module mm;

		size_t src_imm_size_flag() {
			return in.b1 & opflag_imm_size;
		}

		size_t src_imm_size() {
			return size_t(1) << src_imm_size_flag();
		}

		uint8_t src_reg_flag() {
			return in.b1 & opflag_reg;
		}

		uint8_t src_reg_index() {
			return src_reg_flag() >> 4;
		}

		uint8_t src_subreg_index() {
			return in.b1 & opflag_subreg;
		}

		subreg_enum src_subreg_flag()  {
			return static_cast<subreg_enum>(src_subreg_index());
		}

		uint8_t dest_imm_size_flag() {
			return in.b2 & opflag_imm_size;
		}

		uint8_t dest_imm_size() {
			return 1 << dest_imm_size_flag();
		}

		uint8_t dest_reg_flag() {
			return in.b2 & opflag_reg;
		}

		uint8_t dest_reg_index() {
			return dest_reg_flag() >> 4;
		}

		uint8_t dest_subreg_index() {
			return in.b2 & opflag_subreg;
		}

		subreg_enum dest_subreg_flag() {
			return static_cast<subreg_enum>(dest_subreg_index());
		}

		namespace instr {
			const opcode halt               {0x00};

			const opcode ld_regVal_reg      {0x01};
			const opcode ld_immVal_reg		{0x41};

			const opcode st_regVal_regAddr  {0x02};

			const opcode add_regVal_reg     {0x03};

			const opcode sub_regVal_reg		{0x04};

			const opcode mul_regVal_reg		{0x05};

			const opcode div_regVal_reg		{0x06};

			const opcode inc_reg			{0x11};
			const opcode dec_reg			{0x12};
			const opcode not_reg			{0x13};

		}

		class alu : public reg {
		public:
			static const opcode op_add {0x03};
			static const opcode op_sub {0x04};
			static const opcode op_mul {0x05};
			static const opcode op_div {0x06};
			static const opcode op_mod {0x07};
			static const opcode op_and {0x08};
			static const opcode op_or {0x09};
			static const opcode op_nor {0x0A};
			static const opcode op_nand {0x0B};
			static const opcode op_xor {0x0C};
			static const opcode op_shl {0x0D};
			static const opcode op_shr {0x0E};
			static const opcode op_cmp {0x0F};
			static const opcode op_test {0x10};
			static const opcode op_inc {0x11};
			static const opcode op_dec {0x12};
			static const opcode op_not {0x13};

			static const opcode opsize_byte {0x00};
			static const opcode opsize_qword {0x10};
			static const opcode opsize_hword {0x20};
			static const opcode opsize_word {0x30};

			static const opcode opctrl_carryin {0x80};

			static const opcode opflag_code {0x1F};
			static const opcode opflag_size {0x30};
			static const opcode opflag_ctrl {0xC0};

			template <typename T>
			static constexpr bool test_mul_overflow(const T& a, const T& b) {
				return ((b >= 0) && (a >= 0) && (a > std::numeric_limits<T>::max() / b))
					|| ((b < 0) && (a < 0) && (a < std::numeric_limits<T>::max() / b));
			}

			template <typename T>
			static constexpr bool test_mul_underflow(const T& a, const T& b) {
				return ((b >= 0) && (a < 0) && (a < std::numeric_limits<T>::min() / b))
					|| ((b < 0) && (a >= 0) && (a > std::numeric_limits<T>::min() / b));
			}

			opcode op {0};
			reg src_reg;
		};

		alu al;

		uint8_t alu_op_size_map[] {
			alu::opsize_byte,
			alu::opsize_byte,
			alu::opsize_byte,
			alu::opsize_byte,
			alu::opsize_byte,
			alu::opsize_byte,
			alu::opsize_byte,
			alu::opsize_byte,
			alu::opsize_qword,
			alu::opsize_qword,
			alu::opsize_qword,
			alu::opsize_qword,
			alu::opsize_hword,
			alu::opsize_hword,
			alu::opsize_word
		};

		uint8_t& cycle {fl.b6};
		uint8_t& step {fl.b7};

		enum class run_states {
			decode,
			execute,
			alu
		};

		run_states run_state = run_states::decode;

		void ins_complete() {
			run_state = run_states::decode;
			cycle = 0;
			step = 0;
		}

		void ins_substep_complete() {
			run_state = run_states::decode;
		}

		void ins_continue() {
			run_state = run_states::execute;
			step = 0;
		}

		void init() {

		}

		void run() {
			reg* psrc_reg {nullptr};
			reg* pdest_reg {nullptr};
			running_flag = true;

			while (running_flag) {
				switch (run_state) {
					case run_states::decode: {
						switch (cycle) {
							case 0:
								// PC.EnableToAddressBus(SubRegister.H0);
								address_bus.enable_reg(pc, subreg_enum::h0);
								// MemoryModule.AddressRegister.SetFromAddressBus(SubRegister.H0);
								mm.set_address(address_bus);
								break;

							case 1:
								// MemoryModule.DataRegister.EnableToDataBus(SubRegister.W0);
								mm.load(data_bus, 8);
								// Decoder.SetFromDataBus(SubRegister.W0);
								data_bus.set_reg(in, subreg_enum::w0);
								++pc;
								ins_continue();
								break;
						}

						++cycle;
						break;
					}

					case run_states::execute: {
						++cycle;

						switch (in.b0) {
							case instr::halt: {
								switch (step) {
									case 0:
										running_flag = false;
										cycle = 0;
										break;
								}

								break;
							}

							case instr::ld_regVal_reg: {
								switch (step) {
									case 0: {
										// P.Increment(2);
										pc.increment(2);
										// SrcReg = Decoder.RegisterMap[src_reg_flag >> 4];
										psrc_reg = reg_map[src_reg_index()];
										// SrcReg.EnableToDataBus((SubRegister)src_subreg_flag);
										data_bus.enable_reg(psrc_reg, src_subreg_flag());
										// DestReg = Decoder.RegisterMap[DestRegisterFlag >> 4];
										pdest_reg = reg_map[dest_reg_index()];
										// DestReg.SetFromDataBus((SubRegister)DestSubRegisterFlag);
										data_bus.set_reg(pdest_reg, dest_subreg_flag());
										ins_complete();
										break;
									}
								}

								break;
							}

							case instr::ld_immVal_reg: {
								switch (step) {
									case 0: {
										// P.Increment(2);
										pc.increment(2);
										// Decoder.ReadAndEnableImmediate(src_imm_size);
										address_bus.enable_reg(pc, subreg_enum::h0);
										mm.set_address(address_bus);
										break;
									}

									case 1: {
										size_t src_size = src_imm_size();
										pc.increment(src_size);
										mm.load(data_bus, src_size);
										break;
									}

									case 2: {
										// DestReg = Decoder.RegisterMap[DestRegisterFlag >> 4];
										pdest_reg = reg_map[dest_reg_index()];
										// DestReg.SetFromDataBus((SubRegister)DestSubRegisterFlag);
										data_bus.set_reg(*pdest_reg, dest_subreg_flag());
										ins_complete();
										break;
									}
								}

								break;
							}

							case instr::st_regVal_regAddr: {
								switch (step) {
									case 0: {
										// P.Increment(2);
										pc.increment(2);
										// SrcReg = Decoder.RegisterMap[src_reg_flag >> 4];
										psrc_reg = reg_map[src_reg_index()];
										// DestReg = Decoder.RegisterMap[DestRegisterFlag >> 4];
										pdest_reg = reg_map[dest_reg_index()];
										// DestReg.EnableToAddressBus((SubRegister)DestSubRegisterFlag);
										address_bus.enable_reg(pdest_reg, dest_subreg_flag());
										// MemoryModule.AddressRegister.SetFromAddressBus(SubRegister.W0);
										mm.set_address(address_bus);
										break;
									}

									case 1: {
										// SrcReg.EnableToDataBus((SubRegister)src_subreg_flag);
										data_bus.enable_reg(psrc_reg, src_subreg_flag());
										// MemoryModule.DataRegister.SetFromDataBus((SubRegister)src_subreg_flag);
										mm.store(data_bus, src_subreg_flag());
										ins_complete();
										break;
									}
								}

								break;
							}

							case instr::add_regVal_reg: 
							case instr::sub_regVal_reg:
							case instr::mul_regVal_reg:
							case instr::div_regVal_reg:
							{
								switch (step) {
									case 0: {
										pc.increment(2);
										psrc_reg = reg_map[src_reg_index()];
										data_bus.enable_reg(psrc_reg, src_subreg_flag());
										data_bus.set_reg(al.src_reg, subreg_enum::w0);
										break;
									}

									case 1: {
										pdest_reg = reg_map[dest_reg_index()];
										data_bus.enable_reg(pdest_reg, dest_subreg_flag());
										data_bus.set_reg(al, subreg_enum::w0);
										run_state = run_states::alu;
										break;
									}

									case 2: {
										data_bus.enable_reg(al, subreg_enum::w0);
										data_bus.set_reg(pdest_reg, dest_subreg_flag());
										ins_complete();
										break;
									}
								}

								break;
							}

							case instr::inc_reg:
							case instr::dec_reg:
							case instr::not_reg:
							{
								switch (step) {
									case 0: {
										pc.increment(1);
										psrc_reg = reg_map[src_reg_index()];
										data_bus.enable_reg(psrc_reg, src_subreg_flag());
										data_bus.set_reg(al.src_reg, subreg_enum::w0);
										run_state = run_states::alu;
										break;
									}

									case 1: {
										data_bus.enable_reg(al, subreg_enum::w0);
										data_bus.set_reg(psrc_reg, src_subreg_flag());
										ins_complete();
										break;
									}
								}
							}
						}

						++step;
						break;
					}

					case run_states::alu: {
						uint8_t op_size = alu_op_size_map[dest_subreg_index()];

						switch (in.b0 & alu::opflag_code) {
							case alu::op_add: {
								switch (op_size) {
									case alu::opsize_byte: {
										uint8_t result = static_cast<uint8_t>(al.b0) + static_cast<uint8_t>(al.src_reg.b0);
										zero_flag = result == 0;
										negative_flag = result & 0x80;
										overflow_flag = result < al.b0;
										al.w0 = result;
										break;
									}

									case alu::opsize_qword: {
										uint16_t result = static_cast<uint16_t>(al.q0) + static_cast<uint16_t>(al.src_reg.q0);
										zero_flag = result == 0;
										negative_flag = result & 0x8000;
										overflow_flag = result < al.q0;
										al.w0 = result;
										break;
									}

									case alu::opsize_hword: {
										uint32_t result = static_cast<uint32_t>(al.h0) + static_cast<uint32_t>(al.src_reg.h0);
										zero_flag = result == 0;
										negative_flag = result & 0x80000000;
										overflow_flag = result < al.h0;
										al.w0 = result;
										break;
									}

									case alu::opsize_word: {
										uint64_t result = static_cast<uint64_t>(al.w0) + static_cast<uint64_t>(al.src_reg.w0);
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
									case alu::opsize_byte: {
										uint8_t result = static_cast<uint8_t>(al.b0) - static_cast<uint8_t>(al.src_reg.b0);
										zero_flag = result == 0;
										negative_flag = result & 0x80;
										overflow_flag = result > al.b0;
										al.w0 = result;
										break;
									}

									case alu::opsize_qword: {
										uint16_t result = static_cast<uint16_t>(al.q0) - static_cast<uint16_t>(al.src_reg.q0);
										zero_flag = result == 0;
										negative_flag = result & 0x8000;
										overflow_flag = result > al.q0;
										al.w0 = result;
										break;
									}

									case alu::opsize_hword: {
										uint32_t result = static_cast<uint32_t>(al.h0) - static_cast<uint32_t>(al.src_reg.h0);
										zero_flag = result == 0;
										negative_flag = result & 0x80000000;
										overflow_flag = result > al.h0;
										al.w0 = result;
										break;
									}

									case alu::opsize_word: {
										uint64_t result = static_cast<uint64_t>(al.w0) - static_cast<uint64_t>(al.src_reg.w0);
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
									case alu::opsize_byte: {
										uint8_t result = static_cast<uint8_t>(al.b0) - static_cast<uint8_t>(al.src_reg.b0);
										zero_flag = result == 0;
										negative_flag = result & 0x80;
										overflow_flag = alu::test_mul_overflow(al.src_reg.b0, al.b0) || alu::test_mul_underflow(al.src_reg.b0, al.b0);
										al.w0 = result;
										break;
									}

									case alu::opsize_qword: {
										uint16_t result = static_cast<uint16_t>(al.q0) - static_cast<uint16_t>(al.src_reg.q0);
										zero_flag = result == 0;
										negative_flag = result & 0x8000;
										overflow_flag = alu::test_mul_overflow(al.src_reg.q0, al.q0) || alu::test_mul_underflow(al.src_reg.q0, al.q0);
										al.w0 = result;
										break;
									}

									case alu::opsize_hword: {
										uint32_t result = static_cast<uint32_t>(al.h0) - static_cast<uint32_t>(al.src_reg.h0);
										zero_flag = result == 0;
										negative_flag = result & 0x80000000;
										overflow_flag = alu::test_mul_overflow(al.src_reg.h0, al.h0) || alu::test_mul_underflow(al.src_reg.h0, al.h0);
										al.w0 = result;
										break;
									}

									case alu::opsize_word: {
										uint64_t result = static_cast<uint64_t>(al.w0) - static_cast<uint64_t>(al.src_reg.w0);
										zero_flag = result == 0;
										negative_flag = result & 0x8000000000000000;
										overflow_flag = alu::test_mul_overflow(al.src_reg.h0, al.h0) || alu::test_mul_underflow(al.src_reg.h0, al.h0);
										al.w0 = result;
										break;
									}
								}

								break;
							}

							case alu::op_div: {
								overflow_flag = false;

								switch (op_size) {
									case alu::opsize_byte: {
										uint8_t result = static_cast<uint8_t>(al.b0) / static_cast<uint8_t>(al.src_reg.b0);
										zero_flag = result == 0;
										negative_flag = result & 0x80;
										al.w0 = result;
										break;
									}

									case alu::opsize_qword: {
										uint16_t result = static_cast<uint16_t>(al.q0) / static_cast<uint16_t>(al.src_reg.q0);
										zero_flag = result == 0;
										negative_flag = result & 0x8000;
										al.w0 = result;
										break;
									}

									case alu::opsize_hword: {
										uint32_t result = static_cast<uint32_t>(al.h0) / static_cast<uint32_t>(al.src_reg.h0);
										zero_flag = result == 0;
										negative_flag = result & 0x80000000;
										al.w0 = result;
										break;
									}

									case alu::opsize_word: {
										uint64_t result = static_cast<uint64_t>(al.w0) / static_cast<uint64_t>(al.src_reg.w0);
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
									case alu::opsize_byte: {
										uint8_t result = static_cast<uint8_t>(al.b0) + uint8_t(1);
										zero_flag = result == 0;
										negative_flag = result & 0x80;
										overflow_flag = result < al.b0;
										al.w0 = result;
										break;
									}

									case alu::opsize_qword: {
										uint16_t result = static_cast<uint16_t>(al.q0) + uint16_t(1);
										zero_flag = result == 0;
										negative_flag = result & 0x8000;
										overflow_flag = result < al.q0;
										al.w0 = result;
										break;
									}

									case alu::opsize_hword: {
										uint32_t result = static_cast<uint32_t>(al.h0) + uint32_t(1);
										zero_flag = result == 0;
										negative_flag = result & 0x80000000;
										overflow_flag = result < al.h0;
										al.w0 = result;
										break;
									}

									case alu::opsize_word: {
										uint64_t result = static_cast<uint64_t>(al.w0) + uint64_t(1);
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
									case alu::opsize_byte: {
										uint8_t result = static_cast<uint8_t>(al.b0) - uint8_t(1);
										zero_flag = result == 0;
										negative_flag = result & 0x80;
										overflow_flag = result > al.b0;
										al.w0 = result;
										break;
									}

									case alu::opsize_qword: {
										uint16_t result = static_cast<uint16_t>(al.q0) - uint16_t(1);
										zero_flag = result == 0;
										negative_flag = result & 0x8000;
										overflow_flag = result > al.q0;
										al.w0 = result;
										break;
									}

									case alu::opsize_hword: {
										uint32_t result = static_cast<uint32_t>(al.h0) - uint32_t(1);
										zero_flag = result == 0;
										negative_flag = result & 0x80000000;
										overflow_flag = result > al.h0;
										al.w0 = result;
										break;
									}

									case alu::opsize_word: {
										uint64_t result = static_cast<uint64_t>(al.w0) - uint64_t(1);
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
									case alu::opsize_byte: {
										uint8_t result = ~static_cast<uint8_t>(al.b0);
										zero_flag = result == 0;
										negative_flag = result & 0x80;
										al.w0 = result;
										break;
									}

									case alu::opsize_qword: {
										uint16_t result = ~static_cast<uint16_t>(al.q0);
										zero_flag = result == 0;
										negative_flag = result & 0x8000;
										al.w0 = result;
										break;
									}

									case alu::opsize_hword: {
										uint32_t result = ~static_cast<uint32_t>(al.h0);
										zero_flag = result == 0;
										negative_flag = result & 0x80000000;
										al.w0 = result;
										break;
									}

									case alu::opsize_word: {
										uint64_t result = ~static_cast<uint64_t>(al.w0);
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

						/* 
						Skip the update / load / enable / set / store steps below. That means that the ALU 
						operations CANNOT do anything that would require these steps to execute.
						*/
						continue;
					}
				}

				/* update */
				for (size_t idx = 0; idx < increment_count; ++idx) {
					auto & info = increment_array[idx];
					info.first->w0 = info.first->w0 + info.second;
					increment_count = 0;
				}

				/* load */
				mm.on_load();

				/* enable */
				address_bus.on_enable();
				data_bus.on_enable();
				io_bus.on_enable();

				/* set */
				address_bus.on_set();
				data_bus.on_set();
				io_bus.on_set();

				/* store */
				mm.on_store();
			}
		}
	} /* namespace cpu; */
} /* namespace maize */

using namespace maize;

std::vector<cpu::byte> mem {
	cpu::instr::ld_immVal_reg, 0x00, 0x10, 0x88,
	cpu::instr::ld_immVal_reg, 0x00, 0x11, 0x99,
	cpu::instr::add_regVal_reg, 0x10, 0x11,
	cpu::instr::ld_immVal_reg, 0x02, 0x0C, 0x00, 0x20, 0x00, 0x00,
	cpu::instr::st_regVal_regAddr, 0x11, 0x0C,
	cpu::instr::halt
};

int main() {
	uint64_t address = 0x0000000000001000;

	for (auto & b : mem) {
		cpu::mm.write_byte(address, b);
		++address;
	}

	cpu::run();
}

