/* Right now, I have everything crammed into this file. I'm going to break it up into headers and
various cpp files eventually. I find it a little easier to work in one file for the moment. I'm
still playing with the structure a bit. */

#include <iostream>
#include <vector>
#include <map>
#include <array>
#include <thread>
#include <chrono>
#include <semaphore>
#include <condition_variable>

#ifdef __linux__ 
// Linux-specific code
#elif _WIN32
// Windows-specific code
#define NOMINMAX
#include <Windows.h>
#else
// Oops....
#endif

/* The main tick loop is farther below, in the "tick" function. That's where you'll find the
state machine that implements the machine-code instructions. */

/* Right now, the code is still a bit scattered and not up to my usual standards, but I'll
clean it up soon. This is not intended to be pure OO code; my priorities are speed and
super-tight generated assembly. */

/* This program doesn't really "do" anything yet, at least not as visible output. So far it's
just a platform for testing instructions as I implement them. There's some code implemented as
machine language in the vector in the main function (at the bottom of this file) that I load
into memory before starting the main loop. You'll need to run this in the debugger to watch
the data move around as it executes. */

namespace maize {

	namespace cpu {
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

		const uint8_t opflag_reg = 0b11110000;
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

		const uint8_t opflag_subreg = 0b00001111;
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

		size_t size_map[] = {
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

		struct reg_value {
			reg_value() {};
			explicit reg_value(uint64_t init) : w {init} {}
			explicit reg_value(uint32_t init) : h {init} {}
			explicit reg_value(uint16_t init) : q {init} {}
			explicit reg_value(uint8_t init) : b {init} {}

			template<typename T = uint8_t> T operator[](size_t index) {
				return byte_array[index];
			}

			template<typename T = uint8_t> const T operator[](size_t index) const {
				return byte_array[index];
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

		private:
			union {
				reg_word w {0};
				reg_hword h;
				reg_qword q;
				reg_byte b;
				uint8_t byte_array[8];
			};

		};

		class reg;
		class bus;
		
		std::vector<std::pair<reg*, int64_t>> increment_array;

		struct reg_op_info {
			reg_op_info() = default;
			reg_op_info(bus* pbus, reg* preg, subreg_mask_enum mask, uint8_t offset) :
				pbus(pbus), preg(preg), mask(mask), offset(offset) {}
			reg_op_info(const reg_op_info&) = default;
			reg_op_info(reg_op_info&&) = default;
			reg_op_info& operator=(const reg_op_info&) = default;

			bus* pbus {nullptr};
			reg* preg {nullptr};
			subreg_mask_enum mask {0};
			uint8_t offset {0};
		};

		class bus : public reg_value {
		public:
			bus() = default;
		};

		std::vector<reg_op_info> bus_enable_array;
		std::vector<reg_op_info> bus_set_array;

		class reg : public reg_value {
		public:
			reg() : reg_value() {}
			reg(uint64_t init) : reg_value(init) {}

			operator uint64_t() { return w0; }
			explicit operator uint32_t() { return h0; }
			explicit operator uint16_t() { return q0; }
			explicit operator uint8_t() { return b0; }

			uint64_t operator=(uint64_t value) { return w0 = value; }

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
				increment_array.push_back(std::make_pair(this, value));
			}

			void decrement(int64_t value = 1) {
				increment_array.push_back(std::make_pair(this, -value));
			}

			void enable_to_bus(bus& en_bus, subreg_enum subreg) {
				bus_enable_array.push_back(
					reg_op_info(&en_bus, this, subreg_mask_map[static_cast<size_t>(subreg)], offset_map[static_cast<size_t>(subreg)])
				);
			}

			void set_from_bus(bus& set_bus, subreg_enum subreg) {
				bus_set_array.push_back(
					reg_op_info(&set_bus, this, subreg_mask_map[static_cast<size_t>(subreg)], offset_map[static_cast<size_t>(subreg)])
				);
			}

			virtual void on_enable() {
			}

			virtual void on_set() {
			}

		protected:
			uint64_t privilege_flags {0};
			uint64_t privilege_mask {0};
		};

		class device : public reg {
		public:
			device() = default;

		protected:
			reg address_reg;

		public:
			void enable_address_to_bus(bus& enable_bus) {
				address_reg.enable_to_bus(enable_bus, subreg_enum::w0);
			}

			void set_address_from_bus(bus& set_bus) {
				address_reg.set_from_bus(set_bus, subreg_enum::w0);
			}

			void enable_io_to_bus(bus& io_bus) {
				enable_to_bus(io_bus, subreg_enum::w0);
			}

			void set_io_from_bus(bus& source_bus) {
				set_from_bus(source_bus, subreg_enum::w0);
			}
		};

		std::map<uint16_t, device*> devices;

		bus address_bus;
		bus data_bus_0;
		bus data_bus_1;
		bus io_bus;

		struct memory_module : public reg {
			void set_address_from_bus(bus& source_bus) {
				address_reg.set_from_bus(source_bus, subreg_enum::h0);
			}

			void enable_memory_to_bus(bus& load_bus, size_t size) {
				pload_bus = &load_bus;
				load_size = size;
			}

			void enable_memory_to_bus(bus& load_bus, subreg_enum subreg) {
				pload_bus = &load_bus;
				load_size = size_map[static_cast<size_t>(subreg)];
			}

			bool enable_memory_scheduled() {
				return (pload_bus);
			}

			void on_enable_memory() {
				if (pload_bus) {
					w0 = 0;
					uint64_t address = address_reg.w0;
					size_t rem = set_cache_address(address);
					size_t idx = cache_address.b0;

					switch (load_size) {
						case sizeof(uint8_t) :
							b0 = cache[idx];
							this->enable_to_bus(*pload_bus, subreg_enum::b0);
							break;

							case sizeof(uint16_t) :
								if (rem >= sizeof(uint16_t)) {
									q0 = *((uint16_t*)(cache + idx));
								}
								else {
									b0 = cache[cache_address.b0];
									set_cache_address(address);
									b1 = cache[cache_address.b0];
								}

							this->enable_to_bus(*pload_bus, subreg_enum::q0);
							break;

							case sizeof(uint32_t) :
								if (rem >= sizeof(uint32_t)) {
									h0 = *((uint32_t*)(cache + idx));
								}
								else {
									b0 = cache[cache_address.b0];
									set_cache_address(address);
									b1 = cache[cache_address.b0];
									set_cache_address(address);
									b2 = cache[cache_address.b0];
									set_cache_address(address);
									b3 = cache[cache_address.b0];
								}

							this->enable_to_bus(*pload_bus, subreg_enum::h0);
							break;

							case sizeof(uint64_t) :
								if (rem >= sizeof(uint64_t)) {
									w0 = *((uint64_t*)(cache + idx));
								}
								else {
									b0 = cache[cache_address.b0];
									set_cache_address(address);
									b1 = cache[cache_address.b0];
									set_cache_address(address);
									b2 = cache[cache_address.b0];
									set_cache_address(address);
									b3 = cache[cache_address.b0];
									set_cache_address(address);
									b4 = cache[cache_address.b0];
									set_cache_address(address);
									b5 = cache[cache_address.b0];
									set_cache_address(address);
									b6 = cache[cache_address.b0];
									set_cache_address(address);
									b7 = cache[cache_address.b0];
								}

							this->enable_to_bus(*pload_bus, subreg_enum::w0);
							break;

					}

					pload_bus = nullptr;
				}
			}

			void write(uint64_t address, uint8_t value) {
				set_cache_address(address);
				cache[cache_address.b0] = value;
			}

			void set_memory_from_bus(bus& store_bus, subreg_enum subreg) {
				pstore_bus = &store_bus;
				store_mask = subreg_mask_map[static_cast<size_t>(subreg)];
				this->set_from_bus(store_bus, subreg);
			}

			bool set_memory_scheduled() {
				return pstore_bus;
			}

			void on_set_memory() {
				if (pstore_bus) {
					pstore_bus = nullptr;
					uint64_t address = address_reg.w0;
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
								*((uint16_t*)(cache + idx)) = q0;
							}
							else {
								cache[cache_address.b0] = b0;
								set_cache_address(address);
								cache[cache_address.b0] = b1;
							}

							break;

						case subreg_mask_enum::q1:
							if (rem >= 2) {
								*((uint16_t*)(cache + idx)) = q1;
							}
							else {
								cache[cache_address.b0] = b2;
								set_cache_address(address);
								cache[cache_address.b0] = b3;
							}

							break;

						case subreg_mask_enum::q2:
							if (rem >= 2) {
								*((uint16_t*)(cache + idx)) = q2;
							}
							else {
								cache[cache_address.b0] = b4;
								set_cache_address(address);
								cache[cache_address.b0] = b5;
							}

							break;

						case subreg_mask_enum::q3:
							if (rem >= 2) {
								*((uint16_t*)(cache + idx)) = q3;
							}
							else {
								cache[cache_address.b0] = b6;
								set_cache_address(address);
								cache[cache_address.b0] = b7;
							}

							break;

						case subreg_mask_enum::h0:
							if (rem >= 4) {
								*((uint32_t*)(cache + idx)) = h0;
							}
							else {
								cache[cache_address.b0] = b0;
								set_cache_address(address);
								cache[cache_address.b0] = b1;
								set_cache_address(address);
								cache[cache_address.b0] = b2;
								set_cache_address(address);
								cache[cache_address.b0] = b3;
							}

							break;

						case subreg_mask_enum::h1:
							if (rem >= 4) {
								*((uint32_t*)(cache + idx)) = h1;
							}
							else {
								cache[cache_address.b0] = b4;
								set_cache_address(address);
								cache[cache_address.b0] = b5;
								set_cache_address(address);
								cache[cache_address.b0] = b6;
								set_cache_address(address);
								cache[cache_address.b0] = b7;
							}

							break;

						case subreg_mask_enum::w0:
							if (rem >= 8) {
								*((uint64_t*)(cache + idx)) = w0;
							}
							else {
								cache[cache_address.b0] = b0;
								set_cache_address(address);
								cache[cache_address.b0] = b1;
								set_cache_address(address);
								cache[cache_address.b0] = b2;
								set_cache_address(address);
								cache[cache_address.b0] = b3;
								set_cache_address(address);
								cache[cache_address.b0] = b4;
								set_cache_address(address);
								cache[cache_address.b0] = b5;
								set_cache_address(address);
								cache[cache_address.b0] = b6;
								set_cache_address(address);
								cache[cache_address.b0] = b7;
							}

							break;
					}
				}
			}

		protected:
			reg address_reg {0};
			uint64_t address_mask {0xFFFFFFFFFFFFFF00};
			uint64_t cache_base {0xFFFFFFFFFFFFFFFF};
			reg_value cache_address;

			size_t load_size {3};
			subreg_mask_enum store_mask {subreg_mask_enum::w0};
			bus* pload_bus {nullptr};
			bus* pstore_bus {nullptr};

			std::map<uint64_t, uint8_t*> memory_map;
			uint8_t* cache {nullptr};

			size_t set_cache_address(uint64_t address) {
				if (cache_base != (address & address_mask)) {
					cache_base = address & address_mask;

					if (!memory_map.contains(cache_base)) {
						memory_map[cache_base] = new uint8_t[0x100] {0};
					}

					cache = memory_map[cache_base];
				}

				cache_address.w0 = address;
				return 0x100 - cache_address.b0;
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

		template <uint64_t flag_bit> class flag {
		public:
			flag(reg& reg_init, bool value = false) : flag_reg {reg_init} {
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

		//      6         5         4         3         2         1         0
		//   3210987654321098765432109876543210987654321098765432109876543210
		const uint64_t bit_carryout = 0b0000000000000000000000000000000000000000000000000000000000000001;
		const uint64_t bit_negative = 0b0000000000000000000000000000000000000000000000000000000000000010;
		const uint64_t bit_overflow = 0b0000000000000000000000000000000000000000000000000000000000000100;
		const uint64_t bit_parity = 0b0000000000000000000000000000000000000000000000000000000000001000;
		const uint64_t bit_zero = 0b0000000000000000000000000000000000000000000000000000000000010000;
		const uint64_t bit_sign = 0b0000000000000000000000000000000000000000000000000000000000100000;
		const uint64_t bit_reserved = 0b0000000000000000000000000000000000000000000000000000000001000000;
		const uint64_t bit_privilege = 0b0000000000000000000000000000000100000000000000000000000000000000;
		const uint64_t bit_interrupt_enabled = 0b0000000000000000000000000000001000000000000000000000000000000000;
		const uint64_t bit_interrupt_set = 0b0000000000000000000000000000010000000000000000000000000000000000;
		const uint64_t bit_running = 0b0000000000000000000000000000100000000000000000000000000000000000;

		flag<bit_carryout> carryout_flag {fl};
		flag<bit_negative> negative_flag {fl};
		flag<bit_overflow> overflow_flag {fl};
		flag<bit_parity> parity_flag {fl};
		flag<bit_zero> zero_flag {fl};
		flag<bit_sign> sign_flag {fl};
		flag<bit_privilege> privilege_flag {fl};
		flag<bit_interrupt_enabled> interrupt_enabled_flag {fl};
		flag<bit_interrupt_set> interrupt_set_flag {fl};
		flag<bit_running> running_flag {fl};

		std::array<reg*, 16> reg_map {
			&a, &b, &c, &d, &e, &g, &h, &j, &k, &l, &m, &z, &fl, &in, &pc, &sp
		};

		memory_module mm;

		size_t src_imm_size_flag() {
			return in.b1 & opflag_imm_size;
		}

		size_t src_imm_size() {
			return size_t(1) << (in.b1 & opflag_imm_size);
		}

		uint8_t src_reg_flag() {
			return in.b1 & opflag_reg;
		}

		uint8_t src_reg_index() {
			return (in.b1 & opflag_reg) >> 4;
		}

		uint8_t src_subreg_index() {
			return in.b1 & opflag_subreg;
		}

		subreg_enum src_subreg_flag() {
			return static_cast<subreg_enum>(in.b1 & opflag_subreg);
		}

		reg& src_reg() {
			return *reg_map[(in.b1 & opflag_reg) >> 4];
		}

		uint8_t dest_imm_size_flag() {
			return in.b2 & opflag_imm_size;
		}

		uint8_t dest_imm_size() {
			return 1 << (in.b2 & opflag_imm_size);
		}

		uint8_t dest_reg_flag() {
			return in.b2 & opflag_reg;
		}

		uint8_t dest_reg_index() {
			return (in.b2 & opflag_reg) >> 4;
		}

		uint8_t dest_subreg_index() {
			return in.b2 & opflag_subreg;
		}

		subreg_enum dest_subreg_flag() {
			return static_cast<subreg_enum>(in.b2 & opflag_subreg);
		}

		reg& dest_reg() {
			return *reg_map[(in.b2 & opflag_reg) >> 4];
		}

		subreg_enum pc_src_imm_subreg_flag() {
			return imm_size_subreg_map[src_imm_size_flag()];
		}

		subreg_enum pc_dest_imm_subreg_flag() {
			return imm_size_subreg_map[dest_imm_size_flag()];
		}

		namespace instr {
			const opcode halt {0x00};

			const opcode ld_regVal_reg {0x01};
			const opcode ld_immVal_reg {0x41};
			const opcode ld_regAddr_reg {0x81};
			const opcode ld_immAddr_reg {0xC1};

			const opcode st_regVal_regAddr {0x02};

			const opcode add_regVal_reg {0x03};
			const opcode add_immVal_reg {0x43};
			const opcode add_regAddr_reg {0x83};
			const opcode add_immAddr_reg {0xC3};

			const opcode sub_regVal_reg {0x04};
			const opcode sub_immVal_reg {0x44};
			const opcode sub_regAddr_reg {0x84};
			const opcode sub_immAddr_reg {0xC4};

			const opcode mul_regVal_reg {0x05};
			const opcode mul_immVal_reg {0x45};
			const opcode mul_regAddr_reg {0x85};
			const opcode mul_immAddr_reg {0xC5};

			const opcode div_regVal_reg {0x06};
			const opcode div_immVal_reg {0x46};
			const opcode div_regAddr_reg {0x86};
			const opcode div_immAddr_reg {0xC6};

			const opcode mod_regVal_reg {0x07};
			const opcode mod_immVal_reg {0x47};
			const opcode mod_regAddr_reg {0x87};
			const opcode mod_immAddr_reg {0xC7};

			const opcode and_regVal_reg {0x08};
			const opcode and_immVal_reg {0x48};
			const opcode and_regAddr_reg {0x88};
			const opcode and_immAddr_reg {0xC8};

			const opcode or_regVal_reg {0x09};
			const opcode or_immVal_reg {0x49};
			const opcode or_regAddr_reg {0x89};
			const opcode or_immAddr_reg {0xC9};

			const opcode nor_regVal_reg {0x0A};
			const opcode nor_immVal_reg {0x4A};
			const opcode nor_regAddr_reg {0x8A};
			const opcode nor_immAddr_reg {0xCA};

			const opcode nand_regVal_reg {0x0B};
			const opcode nand_immVal_reg {0x4B};
			const opcode nand_regAddr_reg {0x8B};
			const opcode nand_immAddr_reg {0xCB};

			const opcode xor_regVal_reg {0x0C};
			const opcode xor_immVal_reg {0x4C};
			const opcode xor_regAddr_reg {0x8C};
			const opcode xor_immAddr_reg {0xCC};

			const opcode shl_regVal_reg {0x0D};
			const opcode shl_immVal_reg {0x4D};
			const opcode shl_regAddr_reg {0x8D};
			const opcode shl_immAddr_reg {0xCD};

			const opcode shr_regVal_reg {0x0E};
			const opcode shr_immVal_reg {0x4E};
			const opcode shr_regAddr_reg {0x8E};
			const opcode shr_immAddr_reg {0xCE};

			const opcode cmp_regVal_reg {0x0F};
			const opcode cmp_immVal_reg {0x4F};
			const opcode cmp_regAddr_reg {0x8F};
			const opcode cmp_immAddr_reg {0xCF};

			const opcode test_regVal_reg {0x10};
			const opcode test_immVal_reg {0x50};
			const opcode test_regAddr_reg {0x90};
			const opcode test_immAddr_reg {0xD0};

			const opcode inc_regVal {0x11};
			const opcode dec_regVal {0x12};
			const opcode not_regVal {0x13};

			const opcode out_regVal_imm {0x14};
			const opcode out_immVal_imm {0x54};
			const opcode out_regAddr_imm {0x94};
			const opcode out_immAddr_imm {0xD4};

			const opcode lngjmp_regVal_reg {0x15};
			const opcode lngjmp_immVal_reg {0x55};
			const opcode lngjmp_regAddr_reg {0x95};
			const opcode lngjmp_immAddr_reg {0xD5};

			const opcode jmp_regVal_imm {0x16};
			const opcode jmp_immVal_imm {0x56};
			const opcode jmp_regAddr_imm {0x96};
			const opcode jmp_immAddr_imm {0xD6};

			const opcode jz_regVal_imm {0x17};
			const opcode jz_immVal_imm {0x57};
			const opcode jz_regAddr_imm {0x97};
			const opcode jz_immAddr_imm {0xD7};

			const opcode jnz_regVal_imm {0x18};
			const opcode jnz_immVal_imm {0x58};
			const opcode jnz_regAddr_imm {0x98};
			const opcode jnz_immAddr_imm {0xD8};

			const opcode jlt_regVal_imm {0x19};
			const opcode jlt_immVal_imm {0x59};
			const opcode jlt_regAddr_imm {0x99};
			const opcode jlt_immAddr_imm {0xD9};

			const opcode jb_regVal_imm {0x1A};
			const opcode jb_immVal_imm {0x5A};
			const opcode jb_regAddr_imm {0x9A};
			const opcode jb_immAddr_imm {0xDA};

			const opcode jgt_regVal_imm {0x1B};
			const opcode jgt_immVal_imm {0x5B};
			const opcode jgt_regAddr_imm {0x9B};
			const opcode jgt_immAddr_imm {0xDB};

			const opcode ja_regVal_imm {0x1C};
			const opcode ja_immVal_imm {0x5C};
			const opcode ja_regAddr_imm {0x9C};
			const opcode ja_immAddr_imm {0xDC};

			const opcode call_regVal_reg {0x1D};
			const opcode call_immVal_imm {0x5D};
			const opcode call_regAddr_imm {0x9D};
			const opcode call_immAddr_imm {0xDD};

			const opcode outr_regVal_reg {0x1E};
			const opcode outr_immVal_imm {0x5E};
			const opcode outr_regAddr_imm {0x9E};
			const opcode outr_immAddr_imm {0xDE};

			const opcode in_regVal_imm {0x1F};
			const opcode in_immVal_imm {0x5F};
			const opcode in_regAddr_imm {0x9F};
			const opcode in_immAddr_imm {0xDF};

			const opcode push_regVal {0x20};
			const opcode push_immVal {0x60};

			const opcode clr_regVal {0x20};

			const opcode cmpind_regVal_regAddr {0x23};
			const opcode cmpind_immVal_regAddr {0x63};

			const opcode int_regVal {0x24};
			const opcode int_immVal {0x64};

			const opcode tstind_regVal_regAddr {0x25};
			const opcode tstind_immVal_regAddr {0x65};

			const opcode pop_regVal {0x26};

			const opcode ret {0x27};

			const opcode iret {0x28};

			const opcode setint {0x29};

			const opcode clrint {0x30};

			const opcode setcry {0x31};

			const opcode clrcry {0x32};

			const opcode nop {0xAA};

			const opcode brk {0xFF};

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
			static const opcode op_cmdind {0x23};
			static const opcode op_test {0x10};
			static const opcode op_tstind {0x25};
			static const opcode op_inc {0x11};
			static const opcode op_dec {0x12};
			static const opcode op_not {0x13};

			static const opcode opctrl_carryin {0x80};

			static const opcode opflag_code {0x1F};
			static const opcode opflag_size {0x30};
			static const opcode opflag_ctrl {0xC0};

			template <typename T>
			static constexpr bool is_mul_overflow(const T& a, const T& b) {
				return ((b >= 0) && (a >= 0) && (a > std::numeric_limits<T>::max() / b))
					|| ((b < 0) && (a < 0) && (a < std::numeric_limits<T>::max() / b));
			}

			template <typename T>
			static constexpr bool is_mul_underflow(const T& a, const T& b) {
				return ((b >= 0) && (a < 0) && (a < std::numeric_limits<T>::min() / b))
					|| ((b < 0) && (a >= 0) && (a > std::numeric_limits<T>::min() / b));
			}

			void set_src_from_bus(bus& source_bus, subreg_enum subreg = subreg_enum::w0) {
				src_reg.set_from_bus(source_bus, subreg);
			}

			void set_dest_from_bus(bus& source_bus, subreg_enum subreg = subreg_enum::w0) {
				set_from_bus(source_bus, subreg);
			}

			void enable_dest_to_bus(bus& dest_bus, subreg_enum subreg = subreg_enum::w0) {
				enable_to_bus(dest_bus, subreg);
			}

			reg src_reg;
		};

		alu al;

		uint8_t cycle {0};
		uint8_t step {0};

		enum class run_states {
			decode,
			execute,
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

		void tick() {
			running_flag = true;

			while (running_flag) {
				switch (run_state) {
					case run_states::decode: {
						switch (cycle) {
							case 0:
								pc.enable_to_bus(address_bus, subreg_enum::h0);
								mm.set_address_from_bus(address_bus);
								break;

							case 1:
								pc.increment(1);
								mm.enable_memory_to_bus(data_bus_0, 8);
								in.set_from_bus(data_bus_0, subreg_enum::w0);
								run_state = run_states::execute;
								step = 0;
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
										is_power_on = false; // just temporary
										instr_complete();
										break;
								}

								break;
							}

							case instr::ld_regVal_reg: {
								switch (step) {
									case 0: {
										pc.increment(2);
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
										pc.increment(2);
										pc.enable_to_bus(address_bus, subreg_enum::w0);
										mm.set_address_from_bus(address_bus);
										break;
									}

									case 1: {
										size_t src_size = src_imm_size();
										pc.increment(src_size);
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
										pc.increment(2);
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
										pc.increment(2);
										pc.enable_to_bus(address_bus, subreg_enum::w0);
										mm.set_address_from_bus(address_bus);
										break;
									}

									case 1: {
										pc.increment(src_imm_size());
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
										pc.increment(2);
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
										pc.increment(2);
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
										pc.increment(2);
										pc.enable_to_bus(address_bus, subreg_enum::w0);
										mm.set_address_from_bus(address_bus);
										break;
									}

									case 1: {
										size_t src_size = src_imm_size();
										pc.increment(src_size);
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
										pc.increment(2);
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
										pc.increment(2);
										pc.enable_to_bus(address_bus, subreg_enum::w0);
										mm.set_address_from_bus(address_bus);
										break;
									}

									case 1: {
										pc.increment(src_imm_size());
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
										pc.increment(1);
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
										pc.increment(2);
										pc.enable_to_bus(address_bus, subreg_enum::h0);
										mm.set_address_from_bus(address_bus);
										src_reg().enable_to_bus(data_bus_0, src_subreg_flag());
										break;
									}

									case 1: {
										size_t dest_size = dest_imm_size();
										pc.increment(dest_size);
										mm.enable_memory_to_bus(data_bus_1, dest_size);
										operand1.set_from_bus(data_bus_1, src_subreg_flag());
										break;
									}

									case 2: {
										device* pdevice = devices[operand1.q0];
										pdevice->set_io_from_bus(data_bus_0);
										pdevice->set_address_from_bus(data_bus_1);
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
						uint8_t op_size = size_map[dest_subreg_index()];

						switch (in.b0 & alu::opflag_code) {
							case alu::op_add: {
								switch (op_size) {
									case 1: {
										uint8_t result = al.b0 + al.src_reg.b0;
										zero_flag = result == 0;
										negative_flag = result & 0x80;
										overflow_flag = result < al.b0;
										al.w0 = result;
										break;
									}

									case 2: {
										uint16_t result = al.q0 + al.src_reg.q0;
										zero_flag = result == 0;
										negative_flag = result & 0x8000;
										overflow_flag = result < al.q0;
										al.w0 = result;
										break;
									}

									case 4: {
										uint32_t result = al.h0 + al.src_reg.h0;
										zero_flag = result == 0;
										negative_flag = result & 0x80000000;
										overflow_flag = result < al.h0;
										al.w0 = result;
										break;
									}

									case 8: {
										uint64_t result = al.w0 + al.src_reg.w0;
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
										uint8_t result = al.b0 - al.src_reg.b0;
										zero_flag = result == 0;
										negative_flag = result & 0x80;
										overflow_flag = result > al.b0;
										al.w0 = result;
										break;
									}

									case 2: {
										uint16_t result = al.q0 - al.src_reg.q0;
										zero_flag = result == 0;
										negative_flag = result & 0x8000;
										overflow_flag = result > al.q0;
										al.w0 = result;
										break;
									}

									case 4: {
										uint32_t result = al.h0 - al.src_reg.h0;
										zero_flag = result == 0;
										negative_flag = result & 0x80000000;
										overflow_flag = result > al.h0;
										al.w0 = result;
										break;
									}

									case 8: {
										uint64_t result = al.w0 - al.src_reg.w0;
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
										uint8_t result = al.b0 - al.src_reg.b0;
										zero_flag = result == 0;
										negative_flag = result & 0x80;
										overflow_flag = alu::is_mul_overflow(al.src_reg.b0, al.b0) || alu::is_mul_underflow(al.src_reg.b0, al.b0);
										al.w0 = result;
										break;
									}

									case 2: {
										uint16_t result = al.q0 - al.src_reg.q0;
										zero_flag = result == 0;
										negative_flag = result & 0x8000;
										overflow_flag = alu::is_mul_overflow(al.src_reg.q0, al.q0) || alu::is_mul_underflow(al.src_reg.q0, al.q0);
										al.w0 = result;
										break;
									}

									case 4: {
										uint32_t result = al.h0 - al.src_reg.h0;
										zero_flag = result == 0;
										negative_flag = result & 0x80000000;
										overflow_flag = alu::is_mul_overflow(al.src_reg.h0, al.h0) || alu::is_mul_underflow(al.src_reg.h0, al.h0);
										al.w0 = result;
										break;
									}

									case 8: {
										uint64_t result = al.w0 - al.src_reg.w0;
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
										uint8_t result = al.b0 / al.src_reg.b0;
										zero_flag = result == 0;
										negative_flag = result & 0x80;
										al.w0 = result;
										break;
									}

									case 2: {
										uint16_t result = al.q0 / al.src_reg.q0;
										zero_flag = result == 0;
										negative_flag = result & 0x8000;
										al.w0 = result;
										break;
									}

									case 4: {
										uint32_t result = al.h0 / al.src_reg.h0;
										zero_flag = result == 0;
										negative_flag = result & 0x80000000;
										al.w0 = result;
										break;
									}

									case 8: {
										uint64_t result = al.w0 / al.src_reg.w0;
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
										uint8_t result = al.b0 % al.src_reg.b0;
										zero_flag = result == 0;
										negative_flag = result & 0x80;
										al.w0 = result;
										break;
									}

									case 2: {
										uint16_t result = al.q0 % al.src_reg.q0;
										zero_flag = result == 0;
										negative_flag = result & 0x8000;
										al.w0 = result;
										break;
									}

									case 4: {
										uint32_t result = al.h0 % al.src_reg.h0;
										zero_flag = result == 0;
										negative_flag = result & 0x80000000;
										al.w0 = result;
										break;
									}

									case 8: {
										uint64_t result = al.w0 % al.src_reg.w0;
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
										uint8_t result = al.b0 % al.src_reg.b0;
										zero_flag = result == 0;
										negative_flag = result & 0x80;
										al.w0 = result;
										break;
									}

									case 2: {
										uint16_t result = al.q0 % al.src_reg.q0;
										zero_flag = result == 0;
										negative_flag = result & 0x8000;
										al.w0 = result;
										break;
									}

									case 4: {
										uint32_t result = al.h0 % al.src_reg.h0;
										zero_flag = result == 0;
										negative_flag = result & 0x80000000;
										al.w0 = result;
										break;
									}

									case 8: {
										uint64_t result = al.w0 % al.src_reg.w0;
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
										uint8_t result = al.b0 + uint8_t(1);
										zero_flag = result == 0;
										negative_flag = result & 0x80;
										overflow_flag = result < al.b0;
										al.w0 = result;
										break;
									}

									case 2: {
										uint16_t result = al.q0 + uint16_t(1);
										zero_flag = result == 0;
										negative_flag = result & 0x8000;
										overflow_flag = result < al.q0;
										al.w0 = result;
										break;
									}

									case 4: {
										uint32_t result = al.h0 + uint32_t(1);
										zero_flag = result == 0;
										negative_flag = result & 0x80000000;
										overflow_flag = result < al.h0;
										al.w0 = result;
										break;
									}

									case 8: {
										uint64_t result = al.w0 + uint64_t(1);
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
										uint8_t result = al.b0 - uint8_t(1);
										zero_flag = result == 0;
										negative_flag = result & 0x80;
										overflow_flag = result > al.b0;
										al.w0 = result;
										break;
									}

									case 2: {
										uint16_t result = al.q0 - uint16_t(1);
										zero_flag = result == 0;
										negative_flag = result & 0x8000;
										overflow_flag = result > al.q0;
										al.w0 = result;
										break;
									}

									case 4: {
										uint32_t result = al.h0 - uint32_t(1);
										zero_flag = result == 0;
										negative_flag = result & 0x80000000;
										overflow_flag = result > al.h0;
										al.w0 = result;
										break;
									}

									case 8: {
										uint64_t result = al.w0 - uint64_t(1);
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
										uint8_t result = ~al.b0;
										zero_flag = result == 0;
										negative_flag = result & 0x80;
										al.w0 = result;
										break;
									}

									case 2: {
										uint16_t result = ~al.q0;
										zero_flag = result == 0;
										negative_flag = result & 0x8000;
										al.w0 = result;
										break;
									}

									case 4: {
										uint32_t result = ~al.h0;
										zero_flag = result == 0;
										negative_flag = result & 0x80000000;
										al.w0 = result;
										break;
									}

									case 8: {
										uint64_t result = ~al.w0;
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
					for (auto & info : increment_array) {
						info.first->w0 = info.first->w0 + info.second;
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
						info.pbus->w0 = (info.preg->w0 & (uint64_t)info.mask) >> info.offset;
					}

					bus_enable_array.clear();
				}

				if (bus_set_array.size()) {
					for (auto& info : bus_set_array) {
						info.preg->w0 = (~(uint64_t)info.mask & info.preg->w0) | (info.pbus->w0 << info.offset) & (uint64_t)info.mask;
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

		void add_device(uint16_t id, device& new_device) {
			devices[id] = &new_device;
		}

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

	} // namespace cpu; 

	namespace sys {
		class win_console : public cpu::device {
		protected:
			std::mutex ctrl_mutex;
			std::mutex io_mutex;
			std::counting_semaphore<16> io_set {0};
			std::condition_variable io_run_event;
			std::condition_variable io_close_event;
			bool is_open {false};

		public:
			virtual void on_set() {
				cpu::reg::on_set();
				set(w0);
			}

			void set(uint64_t new_bus_value) {
				{
					std::lock_guard<std::mutex> lk(io_mutex);
					w0 = new_bus_value;
				}

				io_set.release();
			}

			uint64_t enable() {
				return 0;
			}

			void open() {
				// std::cout << "opening" << std::endl;
				std::thread run_thread {&win_console::run, this};

				{
					std::unique_lock<std::mutex> lk(io_mutex);
					address_reg.h0 = 0xFFFE;
				}

				io_set.release();

				{
					std::unique_lock<std::mutex> lk(io_mutex);
					io_run_event.wait(lk);
					is_open = true;
				}

				// std::cout << "opened" << std::endl;

				if (run_thread.joinable()) {
					run_thread.detach();
				}
			}

			void close() {
				// std::cout << "closing" << std::endl;

				{
					std::unique_lock<std::mutex> lk(io_mutex);
					address_reg.h0 = 0xFFFF;
				}

				io_set.release();

				{
					std::unique_lock<std::mutex> lk(io_mutex);
					io_close_event.wait(lk);
					is_open = false;
				}

				// std::cout << "closed" << std::endl;
			}

			void run() {
				bool running = true;
				HANDLE hStdin {INVALID_HANDLE_VALUE};
				HANDLE hStdout {INVALID_HANDLE_VALUE};
				HANDLE hStderr {INVALID_HANDLE_VALUE};
				CONSOLE_SCREEN_BUFFER_INFO csbiInfo {0};

				while (running) {
					io_set.acquire();
					uint64_t local_bus_value {0};

					{
						std::unique_lock<std::mutex> lk(io_mutex);
						local_bus_value = w0;
					}

					cpu::reg_value cmd {w0};

					switch (address_reg.w0) {
						case 0x7F: {
							auto opcode = cmd.b1;

							switch (opcode) {
								case 0x0A: {
									WCHAR c = static_cast<WCHAR>(b0);
									WCHAR buf[1] {c};
									WriteConsole(hStdout, buf, 1, nullptr, nullptr);
									break;
								}
							}

							break;
						}

						case 0xFFFE: {
							hStdin = GetStdHandle(STD_INPUT_HANDLE);
							// TODO: error handling
							hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
							// TODO: error handling
							hStderr = GetStdHandle(STD_ERROR_HANDLE);
							// TODO: error handling

							DWORD dwMode {0};

							if (!GetConsoleMode(hStdout, &dwMode)) {
								// TODO: error handling
							}

							dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;

							if (!SetConsoleMode(hStdout, dwMode)) {
								// TODO: error handling
							}

							io_run_event.notify_all();
							break;
						}

						case 0xFFFF: {
							running = false;
							break;
						}
					}
				}

				io_close_event.notify_all();
			}
		};

	} // namespace sys

} // namespace maize

using namespace maize;

int main() {
	/* This is a sample program that I load into memory before starting the main loop. Eventually,
	I'll load a binary file with BIOS, OS image, etc. */

	/* This is the address where the CPU starts executing code. */
	uint64_t address {0x0000000000001000};

	std::vector<uint8_t> mem {
		/* LD B.B0 0x41         */	cpu::instr::ld_immVal_reg, 0x00, 0x10, 0x41,
		/* LD B.B1 0x0A         */	cpu::instr::ld_immVal_reg, 0x00, 0x11, 0x0A,
		/* OUT B 0x7F           */	cpu::instr::out_regVal_imm, 0x1E, 0x00, 0x7F,
		/* ADD B.B0 B.B1        */	cpu::instr::add_regVal_reg, 0x10, 0x11,
		/* LD B.B1 C.Q0         */	cpu::instr::ld_regVal_reg, 0x11, 0x28,
		/* LD 0x00002000 A.H0   */	cpu::instr::ld_immVal_reg, 0x02, 0x0C, 0x00, 0x20, 0x00, 0x00,
		/* ST B @A.H0           */	cpu::instr::st_regVal_regAddr, 0x1E, 0x0C,
		/* LD @A.H0 D */			cpu::instr::ld_regAddr_reg, 0x0C, 0x3E,
		/* INC B.B1             */	cpu::instr::inc_regVal, 0x11,
		/* LD @0x00002000 E.H0  */	cpu::instr::ld_immAddr_reg, 0x02, 0x4C, 0x00, 0x10, 0x00, 0x00,
		/* HALT                 */	cpu::instr::halt
	};

	/* Load the program above into memory. */
	for (auto& b : mem) {
		cpu::mm.write(address, b);
		++address;
	}

	sys::win_console console;
	console.open();
	cpu::add_device(0x7F, console);
	cpu::is_power_on = true;
	cpu::run();
	console.close();
}

