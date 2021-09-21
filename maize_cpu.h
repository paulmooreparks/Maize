#include <cstdint>
#include <vector>
#include <map>
#include <array>
#include <thread>
#include <chrono>
#include <semaphore>
#include <condition_variable>

namespace maize {
	typedef uint64_t word;
	typedef uint32_t hword;
	typedef uint16_t qword;
	typedef uint8_t byte;

	typedef byte opcode;

	namespace cpu {
		class reg;
		class bus;
		class device;

		namespace {
			struct reg_byte {
				byte b0;
				byte b1;
				byte b2;
				byte b3;
				byte b4;
				byte b5;
				byte b6;
				byte b7;
			};

			struct reg_qword {
				qword q0;
				qword q1;
				qword q2;
				qword q3;
			};

			struct reg_hword {
				hword h0;
				hword h1;
			};

			struct reg_word {
				word w0;
			};

			template <word flag_bit> class flag {
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

		}

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

		/* Defines a bit mask for each subregister in a register. */
		enum class subreg_mask_enum : word {
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

		struct reg_value {
			reg_value() {};
			reg_value(word init) : w {init} {}

			operator word() { return w0; }

			explicit operator hword() {
				return h0;
			}

			explicit operator qword() {
				return q0;
			}

			explicit operator byte() {
				return b0;
			}

			word operator=(word value) {
				return w0 = value;
			}

			byte operator[](size_t index) {
				return byte_index(index);
			}

			byte operator[](size_t index) const {
				return byte_index(index);
			}

			byte byte_index(size_t index) const {
				// TODO: range error handling
				return byte_array[index];
			}

			qword qword_index(size_t index) const {
				// TODO: range error handling
				return qword_array[index];
			}

			hword hword_index(size_t index) const {
				// TODO: range error handling
				return hword_array[index];
			}

			word& w0 {w.w0};
			hword& h0 {h.h0};
			hword& h1 {h.h1};
			qword& q0 {q.q0};
			qword& q1 {q.q1};
			qword& q2 {q.q2};
			qword& q3 {q.q3};
			byte& b0 {b.b0};
			byte& b1 {b.b1};
			byte& b2 {b.b2};
			byte& b3 {b.b3};
			byte& b4 {b.b4};
			byte& b5 {b.b5};
			byte& b6 {b.b6};
			byte& b7 {b.b7};

		private:
			union {
				reg_word w {0};
				reg_hword h;
				reg_qword q;
				reg_byte b;
				hword hword_array[2];
				qword qword_array[4];
				byte byte_array[8];
			};

		};

		class bus : public reg_value {
		public:
			bus() = default;
		};

		class reg : public reg_value {
		public:
			reg() : reg_value() {}
			reg(word init) : reg_value(init) {}

			word& operator++() {
				increment(1);
				return w0;
			}

			word operator++(int) {
				increment(1);
				return w0;
			}

			word operator--() {
				decrement(1);
				return w0;
			}

			word operator--(int) {
				decrement(1);
				return w0;
			}

			void increment(int8_t value, subreg_enum subreg = subreg_enum::w0);
			void decrement(int8_t value, subreg_enum subreg = subreg_enum::w0);

			void enable_to_bus(bus& en_bus, subreg_enum subreg);
			void set_from_bus(bus& set_bus, subreg_enum subreg);
			virtual void on_enable();
			virtual void on_set();

		protected:
			// word privilege_flags {0};
			// word privilege_mask {0};
		};

		class device : public reg {
		public:
			device() = default;

		protected:
			reg address_reg;

		public:
			void enable_address_to_bus(bus& enable_bus);
			void set_address_from_bus(bus& set_bus);
			void enable_io_to_bus(bus& io_bus);
			void set_io_from_bus(bus& source_bus);
		};

		struct memory_module : public reg {
			void set_segment_from_bus(bus& source_bus);
			void set_address_from_bus(bus& source_bus);
			void enable_memory_to_bus(bus& load_bus, size_t size);
			void enable_memory_to_bus(bus& load_bus, subreg_enum subreg);
			bool enable_memory_scheduled();
			void on_enable_memory();

			void write(reg_value address, byte value);
			std::vector<byte> read(reg_value address, hword count);
			void set_memory_from_bus(bus& store_bus, subreg_enum subreg);
			bool set_memory_scheduled();
			void on_set_memory();

		protected:
			reg address_reg {0};
			word address_mask {0xFFFFFFFFFFFFFF00};
			word cache_base {0xFFFFFFFFFFFFFFFF};
			reg_value cache_address;

			size_t load_size {3};
			subreg_mask_enum store_mask {subreg_mask_enum::w0};
			bus* pload_bus {nullptr};
			bus* pstore_bus {nullptr};

			std::map<word, byte*> memory_map;
			byte* cache {nullptr};

			size_t set_cache_address(reg_value address);
		};

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

			void set_src_from_bus(bus& source_bus, subreg_enum subreg = subreg_enum::w0);
			void set_dest_from_bus(bus& source_bus, subreg_enum subreg = subreg_enum::w0);
			void enable_dest_to_bus(bus& dest_bus, subreg_enum subreg = subreg_enum::w0);

			reg src_reg;
		};


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

			const opcode sys_regVal {0x34};
			const opcode sys_immVal {0x74};

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

		namespace regs {
			// The CPU's general registers are defined here
			extern reg a;
			extern reg b;
			extern reg c;
			extern reg d;
			extern reg e;
			extern reg g;
			extern reg h;
			extern reg j;
			extern reg k;
			extern reg l;
			extern reg m;
			extern reg z;
			extern reg fl; // flags register
			extern reg in; // instruction register
			extern reg pc; // program execution register
			extern reg sp; // stack register
		}

		extern bus address_bus;
		extern bus data_bus_0;
		extern bus data_bus_1;
		extern bus io_bus;

		extern memory_module mm;
		extern alu al;

		void add_device(qword id, device& new_device);
		void run();


	} // namespace cpu; 

} // namespace maize
