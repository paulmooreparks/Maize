#pragma once 
#include <cstdint>
#include <cstddef>
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
		const byte opcode_flag = 0b11000000;
		const byte opcode_flag_srcReg =  0b00000000;
		const byte opcode_flag_srcImm =  0b01000000;
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

#if false
		const byte opflag_reg_sp = 0b11111100; // S.H0 = stack pointer
		const byte opflag_reg_bp = 0b11111101; // S.H1 = base pointer
		const byte opflag_reg_pc = 0b11101100; // P.H0 = program counter
		const byte opflag_reg_cs = 0b11101101; // P.H1 = program segment
		const byte opflag_reg_fl = 0b11001100; // F.H0 = flags
#endif

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

			void increment(byte value, subreg_enum subreg = subreg_enum::w0);
			void decrement(byte value, subreg_enum subreg = subreg_enum::w0);

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

			template <typename T> hword write(reg_value address, T value);
			template <> hword write<byte>(reg_value address, byte value);
			template <> hword write<qword>(reg_value address, qword value);
			template <> hword write<hword>(reg_value address, hword value);
			template <> hword write<word>(reg_value address, word value);

			std::vector<byte> read(reg_value address, hword count);
			byte read_byte(reg_value address);
			void set_memory_from_bus(bus& store_bus, subreg_enum subreg);
			bool set_memory_scheduled();
			void on_set_memory();
			word last_block() const;
			
			const hword block_size {0x100};

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
			static const opcode op_test {0x10};
			static const opcode op_inc {0x11};
			static const opcode op_dec {0x12};
			static const opcode op_not {0x13};
			static const opcode op_cmpind {0x2F};
			static const opcode op_tstind {0x30};

			static const opcode opctrl_carryin {0x80};

			static const opcode opflag_code {0x3F}; // 0001`1111
			static const opcode opflag_size {0x30}; // 1100`0000

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
			reg dest_reg;
		};


		namespace instr {
			const opcode halt_opcode			{0x00};

			const opcode ld_opcode				{0x01};
			const opcode ld_regVal_reg			{ld_opcode | opcode_flag_srcReg};
			const opcode ld_immVal_reg			{ld_opcode | opcode_flag_srcImm};
			const opcode ld_regAddr_reg			{ld_opcode | opcode_flag_srcAddr};
			const opcode ld_immAddr_reg			{ld_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode st_opcode				{0x02};
			const opcode st_regVal_regAddr		{st_opcode | opcode_flag_srcReg};

			const opcode add_opcode				{0x03};
			const opcode add_regVal_reg			{add_opcode | opcode_flag_srcReg};
			const opcode add_immVal_reg			{add_opcode | opcode_flag_srcImm};
			const opcode add_regAddr_reg		{add_opcode | opcode_flag_srcAddr};
			const opcode add_immAddr_reg		{add_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode sub_opcode				{0x04};
			const opcode sub_regVal_reg			{sub_opcode | opcode_flag_srcReg};
			const opcode sub_immVal_reg			{sub_opcode | opcode_flag_srcImm};
			const opcode sub_regAddr_reg		{sub_opcode | opcode_flag_srcAddr};
			const opcode sub_immAddr_reg		{sub_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode mul_opcode				{0x05};
			const opcode mul_regVal_reg			{mul_opcode | opcode_flag_srcReg};
			const opcode mul_immVal_reg			{mul_opcode | opcode_flag_srcImm};
			const opcode mul_regAddr_reg		{mul_opcode | opcode_flag_srcAddr};
			const opcode mul_immAddr_reg		{mul_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode div_opcode				{0x06};
			const opcode div_regVal_reg			{div_opcode | opcode_flag_srcReg};
			const opcode div_immVal_reg			{div_opcode | opcode_flag_srcImm};
			const opcode div_regAddr_reg		{div_opcode | opcode_flag_srcAddr};
			const opcode div_immAddr_reg		{div_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode mod_opcode				{0x07};
			const opcode mod_regVal_reg			{mod_opcode | opcode_flag_srcReg};
			const opcode mod_immVal_reg			{mod_opcode | opcode_flag_srcImm};
			const opcode mod_regAddr_reg		{mod_opcode | opcode_flag_srcAddr};
			const opcode mod_immAddr_reg		{mod_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode and_opcode				{0x08};
			const opcode and_regVal_reg			{and_opcode | opcode_flag_srcReg};
			const opcode and_immVal_reg			{and_opcode | opcode_flag_srcImm};
			const opcode and_regAddr_reg		{and_opcode | opcode_flag_srcAddr};
			const opcode and_immAddr_reg		{and_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};
   
			const opcode or_opcode				{0x09}; 
			const opcode or_regVal_reg			{or_opcode | opcode_flag_srcReg};
			const opcode or_immVal_reg			{or_opcode | opcode_flag_srcImm};
			const opcode or_regAddr_reg			{or_opcode | opcode_flag_srcAddr};
			const opcode or_immAddr_reg			{or_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode nor_opcode				{0x0A};
			const opcode nor_regVal_reg			{nor_opcode | opcode_flag_srcReg};
			const opcode nor_immVal_reg			{nor_opcode | opcode_flag_srcImm};
			const opcode nor_regAddr_reg		{nor_opcode | opcode_flag_srcAddr};
			const opcode nor_immAddr_reg		{nor_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode nand_opcode			{0x0B};
			const opcode nand_regVal_reg		{nand_opcode | opcode_flag_srcReg};
			const opcode nand_immVal_reg		{nand_opcode | opcode_flag_srcImm};
			const opcode nand_regAddr_reg		{nand_opcode | opcode_flag_srcAddr};
			const opcode nand_immAddr_reg		{nand_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode xor_opcode				{0x0C};
			const opcode xor_regVal_reg			{xor_opcode | opcode_flag_srcReg};
			const opcode xor_immVal_reg			{xor_opcode | opcode_flag_srcImm};
			const opcode xor_regAddr_reg		{xor_opcode | opcode_flag_srcAddr};
			const opcode xor_immAddr_reg		{xor_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode shl_opcode				{0x0D};
			const opcode shl_regVal_reg			{shl_opcode | opcode_flag_srcReg};
			const opcode shl_immVal_reg			{shl_opcode | opcode_flag_srcImm};
			const opcode shl_regAddr_reg		{shl_opcode | opcode_flag_srcAddr};
			const opcode shl_immAddr_reg		{shl_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode shr_opcode				{0x0E};
			const opcode shr_regVal_reg			{shr_opcode | opcode_flag_srcReg};
			const opcode shr_immVal_reg			{shr_opcode | opcode_flag_srcImm};
			const opcode shr_regAddr_reg		{shr_opcode | opcode_flag_srcAddr};
			const opcode shr_immAddr_reg		{shr_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode cmp_opcode				{0x0F};
			const opcode cmp_regVal_reg			{cmp_opcode | opcode_flag_srcReg};
			const opcode cmp_immVal_reg			{cmp_opcode | opcode_flag_srcImm};
			const opcode cmp_regAddr_reg		{cmp_opcode | opcode_flag_srcAddr};
			const opcode cmp_immAddr_reg		{cmp_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode test_opcode			{0x10};
			const opcode test_regVal_reg		{test_opcode | opcode_flag_srcReg};
			const opcode test_immVal_reg		{test_opcode | opcode_flag_srcImm};
			const opcode test_regAddr_reg		{test_opcode | opcode_flag_srcAddr};
			const opcode test_immAddr_reg		{test_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode inc_opcode				{0x11};
			const opcode inc_regVal				{inc_opcode | opcode_flag_srcReg};
			
			const opcode dec_opcode				{0x12};
			const opcode dec_regVal				{dec_opcode | opcode_flag_srcReg};
			
			const opcode not_opcode				{0x13};
			const opcode not_regVal				{not_opcode | opcode_flag_srcReg};

			const opcode out_opcode				{0x14};
			const opcode out_regVal_imm			{out_opcode | opcode_flag_srcReg};
			const opcode out_immVal_imm			{out_opcode | opcode_flag_srcImm};
			const opcode out_regAddr_imm		{out_opcode | opcode_flag_srcAddr};
			const opcode out_immAddr_imm		{out_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode lngjmp_opcode			{0x15};
			const opcode lngjmp_regVal			{lngjmp_opcode | opcode_flag_srcReg};
			const opcode lngjmp_immVal			{lngjmp_opcode | opcode_flag_srcImm};
			const opcode lngjmp_regAddr			{lngjmp_opcode | opcode_flag_srcAddr};
			const opcode lngjmp_immAddr			{lngjmp_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode jmp_opcode				{0x16};
			const opcode jmp_regVal				{jmp_opcode | opcode_flag_srcReg};
			const opcode jmp_immVal				{jmp_opcode | opcode_flag_srcImm};
			const opcode jmp_regAddr			{jmp_opcode | opcode_flag_srcAddr};
			const opcode jmp_immAddr			{jmp_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode jz_opcode				{0x17};
			const opcode jz_regVal				{jz_opcode | opcode_flag_srcReg};
			const opcode jz_immVal				{jz_opcode | opcode_flag_srcImm};
			const opcode jz_regAddr				{jz_opcode | opcode_flag_srcAddr};
			const opcode jz_immAddr				{jz_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode jnz_opcode				{0x18};
			const opcode jnz_regVal				{jnz_opcode | opcode_flag_srcReg};
			const opcode jnz_immVal				{jnz_opcode | opcode_flag_srcImm};
			const opcode jnz_regAddr			{jnz_opcode | opcode_flag_srcAddr};
			const opcode jnz_immAddr			{jnz_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode jlt_opcode				{0x19};
			const opcode jlt_regVal				{jlt_opcode | opcode_flag_srcReg};
			const opcode jlt_immVal				{jlt_opcode | opcode_flag_srcImm};
			const opcode jlt_regAddr			{jlt_opcode | opcode_flag_srcAddr};
			const opcode jlt_immAddr			{jlt_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode jb_opcode				{0x1A};
			const opcode jb_regVal				{jb_opcode | opcode_flag_srcReg};
			const opcode jb_immVal				{jb_opcode | opcode_flag_srcImm};
			const opcode jb_regAddr				{jb_opcode | opcode_flag_srcAddr};
			const opcode jb_immAddr				{jb_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode jgt_opcode				{0x1B};
			const opcode jgt_regVal				{jgt_opcode | opcode_flag_srcReg};
			const opcode jgt_immVal				{jgt_opcode | opcode_flag_srcImm};
			const opcode jgt_regAddr			{jgt_opcode | opcode_flag_srcAddr};
			const opcode jgt_immAddr			{jgt_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode ja_opcode				{0x1C};
			const opcode ja_regVal				{ja_opcode | opcode_flag_srcReg};
			const opcode ja_immVal				{ja_opcode | opcode_flag_srcImm};
			const opcode ja_regAddr				{ja_opcode | opcode_flag_srcAddr};
			const opcode ja_immAddr				{ja_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode call_opcode			{0x1D};
			const opcode call_regVal			{call_opcode | opcode_flag_srcReg};
			const opcode call_immVal			{call_opcode | opcode_flag_srcImm};
			const opcode call_regAddr			{call_opcode | opcode_flag_srcAddr};
			const opcode call_immAddr			{call_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode outr_opcode			{0x1E};
			const opcode outr_regVal_reg		{outr_opcode | opcode_flag_srcReg};
			const opcode outr_immVal_imm		{outr_opcode | opcode_flag_srcImm};
			const opcode outr_regAddr_imm		{outr_opcode | opcode_flag_srcAddr};
			const opcode outr_immAddr_imm		{outr_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode in_opcode				{0x1F};
			const opcode in_regVal_imm			{in_opcode | opcode_flag_srcReg};
			const opcode in_immVal_imm			{in_opcode | opcode_flag_srcImm};
			const opcode in_regAddr_imm			{in_opcode | opcode_flag_srcAddr};
			const opcode in_immAddr_imm			{in_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode push_opcode			{0x20};
			const opcode push_regVal			{push_opcode | opcode_flag_srcReg};
			const opcode push_immVal			{push_opcode | opcode_flag_srcImm};

			const opcode clr_opcode				{0x22};
			const opcode clr_regVal				{clr_opcode | opcode_flag_srcReg};

			const opcode int_opcode				{0x24};
			const opcode int_regVal				{int_opcode | opcode_flag_srcReg};
			const opcode int_immVal				{int_opcode | opcode_flag_srcImm};

			const opcode pop_opcode				{0x26};
			const opcode pop_regVal				{pop_opcode | opcode_flag_srcReg};

			const opcode ret_opcode				{0x27};

			const opcode iret_opcode			{0x28};

			const opcode setint_opcode			{0x29};

			const opcode cmpind_opcode			{0x2F};
			const opcode cmpind_regVal_regAddr	{cmpind_opcode | opcode_flag_srcReg};
			const opcode cmpind_immVal_regAddr	{cmpind_opcode | opcode_flag_srcImm};

			const opcode tstind_opcode			{0x30};
			const opcode tstind_regVal_regAddr	{tstind_opcode | opcode_flag_srcReg};
			const opcode tstind_immVal_regAddr	{tstind_opcode | opcode_flag_srcImm};

			const opcode setcry_opcode			{0x31};

			const opcode clrcry_opcode			{0x32};

			const opcode clrint_opcode			{0x33};

			const opcode sys_opcode				{0x34};
			const opcode sys_regVal				{sys_opcode | opcode_flag_srcReg};
			const opcode sys_immVal				{sys_opcode | opcode_flag_srcImm};

			const opcode nop_opcode				{0xAA};

			const opcode brk_opcode				{0xFF};

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
