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
	typedef int64_t s_word;
	typedef int32_t s_hword;
	typedef int16_t s_qword;
	typedef int8_t	s_byte;

	typedef uint64_t u_word;
	typedef uint32_t u_hword;
	typedef uint16_t u_qword;
	typedef uint8_t  u_byte;

	typedef u_byte opcode;

	namespace cpu {
		const u_byte opcode_flag = 0b11000000;
		const u_byte opcode_flag_srcReg =  0b00000000;
		const u_byte opcode_flag_srcImm =  0b01000000;
		const u_byte opcode_flag_srcAddr = 0b10000000;

		const u_byte opflag_reg		= 0b11110000;
		const u_byte opflag_reg_r0	= 0b00000000;
		const u_byte opflag_reg_r1	= 0b00010000;
		const u_byte opflag_reg_r2	= 0b00100000;
		const u_byte opflag_reg_r3	= 0b00110000;
		const u_byte opflag_reg_r4	= 0b01000000;
		const u_byte opflag_reg_r5	= 0b01010000;
		const u_byte opflag_reg_r6	= 0b01100000;
		const u_byte opflag_reg_r7	= 0b01110000;
		const u_byte opflag_reg_r8	= 0b10000000;
		const u_byte opflag_reg_r9	= 0b10010000;
		const u_byte opflag_reg_rt	= 0b10100000;
		const u_byte opflag_reg_rv	= 0b10110000;
		const u_byte opflag_reg_rf	= 0b11000000;
		const u_byte opflag_reg_ri	= 0b11010000;
		const u_byte opflag_reg_rp	= 0b11100000;
		const u_byte opflag_reg_rs	= 0b11110000;

#if false
		const u_byte opflag_reg_sp = 0b11111100; // RS.H0 = stack pointer
		const u_byte opflag_reg_bp = 0b11111101; // RS.H1 = base pointer
		const u_byte opflag_reg_pc = 0b11101100; // RP.H0 = program counter
		const u_byte opflag_reg_cs = 0b11101101; // RP.H1 = program segment
		const u_byte opflag_reg_fl = 0b11001100; // RF.H0 = flags
#endif

		const u_byte opflag_subreg = 0b00001111;
		const u_byte opflag_subreg_b0 = 0b00000000;
		const u_byte opflag_subreg_b1 = 0b00000001;
		const u_byte opflag_subreg_b2 = 0b00000010;
		const u_byte opflag_subreg_b3 = 0b00000011;
		const u_byte opflag_subreg_b4 = 0b00000100;
		const u_byte opflag_subreg_b5 = 0b00000101;
		const u_byte opflag_subreg_b6 = 0b00000110;
		const u_byte opflag_subreg_b7 = 0b00000111;
		const u_byte opflag_subreg_q0 = 0b00001000;
		const u_byte opflag_subreg_q1 = 0b00001001;
		const u_byte opflag_subreg_q2 = 0b00001010;
		const u_byte opflag_subreg_q3 = 0b00001011;
		const u_byte opflag_subreg_h0 = 0b00001100;
		const u_byte opflag_subreg_h1 = 0b00001101;
		const u_byte opflag_subreg_w0 = 0b00001110;

		const u_byte opflag_imm_size = 0b00000111;
		const u_byte opflag_imm_size_08b = 0b00000000;
		const u_byte opflag_imm_size_16b = 0b00000001;
		const u_byte opflag_imm_size_32b = 0b00000010;
		const u_byte opflag_imm_size_64b = 0b00000011;

		const u_byte opflag_imm_reserved_01 = 0b01000000;
		const u_byte opflag_imm_reserved_02 = 0b01010000;
		const u_byte opflag_imm_reserved_03 = 0b01100000;
		const u_byte opflag_imm_reserved_04 = 0b01110000;

		class reg;
		class bus;
		class device;

		namespace {
			struct reg_byte {
				u_byte b0;
				u_byte b1;
				u_byte b2;
				u_byte b3;
				u_byte b4;
				u_byte b5;
				u_byte b6;
				u_byte b7;
			};

			struct reg_qword {
				u_qword q0;
				u_qword q1;
				u_qword q2;
				u_qword q3;
			};

			struct reg_hword {
				u_hword h0;
				u_hword h1;
			};

			struct reg_word {
				u_word w0;
			};

			template <u_word flag_bit> class flag {
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
			r0 = 0x00,
			r1 = 0x01,
			r2 = 0x02,
			r3 = 0x03,
			r4 = 0x04,
			r5 = 0x05,
			r6 = 0x06,
			r7 = 0x07,
			r8 = 0x08,
			r9 = 0x09,
			rt = 0x0A,
			rv = 0x0B,
			fl = 0x0C,
			in = 0x0D,
			pc = 0x0E,
			sp = 0x0F
		};

		enum class subreg_enum : size_t {
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
		enum class subreg_mask_enum : u_word {
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
			reg_value(u_word init) : w {init} {}

			operator u_word() { return w0; }

			explicit operator u_hword() {
				return h0;
			}

			explicit operator u_qword() {
				return q0;
			}

			explicit operator u_byte() {
				return b0;
			}

			u_word operator=(u_word value) {
				return w0 = value;
			}

			u_byte& operator[](size_t index) {
				return byte_array[index];
			}

			u_byte const& operator[](size_t index) const {
				return byte_array[index];
			}

			u_byte byte_index(size_t index) const {
				// TODO: range error handling
				return byte_array[index];
			}

			u_qword qword_index(size_t index) const {
				// TODO: range error handling
				return qword_array[index];
			}

			u_hword hword_index(size_t index) const {
				// TODO: range error handling
				return hword_array[index];
			}

			u_word& w0 {w.w0};
			u_hword& h0 {h.h0};
			u_hword& h1 {h.h1};
			u_qword& q0 {q.q0};
			u_qword& q1 {q.q1};
			u_qword& q2 {q.q2};
			u_qword& q3 {q.q3};
			u_byte& b0 {b.b0};
			u_byte& b1 {b.b1};
			u_byte& b2 {b.b2};
			u_byte& b3 {b.b3};
			u_byte& b4 {b.b4};
			u_byte& b5 {b.b5};
			u_byte& b6 {b.b6};
			u_byte& b7 {b.b7};

		private:
			union {
				reg_word w {0};
				reg_hword h;
				reg_qword q;
				reg_byte b;
				u_hword hword_array[2];
				u_qword qword_array[4];
				u_byte byte_array[8];
			};

		};

		class bus : public reg_value {
		public:
			bus() = default;
		};

		class reg : public reg_value {
		public:
			reg() : reg_value() {}
			reg(u_word init) : reg_value(init) {}

			u_word& operator++() {
				increment(1);
				return w0;
			}

			u_word operator++(int) {
				increment(1);
				return w0;
			}

			u_word operator--() {
				decrement(1);
				return w0;
			}

			u_word operator--(int) {
				decrement(1);
				return w0;
			}

		protected:
			void increment(u_byte value, subreg_enum subreg = subreg_enum::w0);
			void decrement(u_byte value, subreg_enum subreg = subreg_enum::w0);
			// u_word privilege_flags {0};
			// u_word privilege_mask {0};
		};

		class device : public reg {
		public:
			device() = default;
			reg address_reg;
		};

		struct memory_module : public reg {
			u_hword write_byte(reg_value address, u_byte value);
			u_hword write_qword(reg_value address, u_qword value);
			u_hword write_hword(reg_value address, u_hword value);
			u_hword write_word(reg_value address, u_word value);

			size_t read(reg_value const &address, reg_value &reg, subreg_enum subreg);
			size_t read(u_word address, reg_value &reg, subreg_enum subreg);
			size_t read(reg_value const &address, reg_value &reg, size_t count, size_t dst_idx);
			size_t read(u_word address, reg_value &reg, size_t count, size_t dst_idx);
			size_t read(reg_value address, u_hword count, std::vector<u_byte> &retval);
			std::vector<u_byte> read(reg_value address, u_hword count);
			u_byte read_byte(u_word address);
			u_word last_block() const;
			
			const u_hword block_size {0x100};

		protected:
			reg address_reg {0};
			u_word address_mask {0xFFFFFFFFFFFFFF00};
			u_word cache_base {0xFFFFFFFFFFFFFFFF};
			reg_value cache_address;

			size_t load_size {3};
			subreg_mask_enum store_mask {subreg_mask_enum::w0};
			bus* pload_bus {nullptr};
			bus* pstore_bus {nullptr};

			std::map<u_word, u_byte*> memory_map;
			u_byte* cache {nullptr};

			size_t set_cache_address(u_word address);
		};

		class arithmetic_logic_unit : public reg {
		public:
			static const opcode opctrl_carryin	{0x80};
			static const opcode opflag_code		{0x3F}; // 0011`1111

			reg op1_reg;
			reg op2_reg;
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
			const opcode st_immVal_regAddr		{st_opcode | opcode_flag_srcImm};

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

			const opcode cmpxchg_opcode			{0x11};
			const opcode cmpxchg_regVal_regreg	{cmpxchg_opcode | opcode_flag_srcReg};
			const opcode cmpxchg_immVal_regreg	{cmpxchg_opcode | opcode_flag_srcImm};
			const opcode cmpxchg_regAddr_regreg	{cmpxchg_opcode | opcode_flag_srcAddr};
			const opcode cmpxchg_immAddr_regreg	{cmpxchg_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode lea_opcode				{0x12};
			const opcode lea_regVal_regreg		{lea_opcode | opcode_flag_srcReg};
			const opcode lea_immVal_regreg		{lea_opcode | opcode_flag_srcImm};
			const opcode lea_regAddr_regreg		{lea_opcode | opcode_flag_srcAddr};
			const opcode lea_immAddr_regreg		{lea_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode ldz_opcode				{0x13};
			const opcode ldz_regVal_reg			{ldz_opcode | opcode_flag_srcReg};
			const opcode ldz_immVal_reg			{ldz_opcode | opcode_flag_srcImm};
			const opcode ldz_regAddr_reg		{ldz_opcode | opcode_flag_srcAddr};
			const opcode ldz_immAddr_reg		{ldz_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

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

			const opcode testind_opcode			{0x30};
			const opcode testind_regVal_regAddr	{testind_opcode | opcode_flag_srcReg};
			const opcode testind_immVal_regAddr	{testind_opcode | opcode_flag_srcImm};

			const opcode inc_opcode				{0x31};
			const opcode inc_regVal				{inc_opcode | opcode_flag_srcReg};
			
			const opcode dec_opcode				{0x32};
			const opcode dec_regVal				{dec_opcode | opcode_flag_srcReg};
			
			const opcode not_opcode				{0x33};
			const opcode not_regVal				{not_opcode | opcode_flag_srcReg};

			const opcode sys_opcode				{0x34};
			const opcode sys_regVal				{sys_opcode | opcode_flag_srcReg};
			const opcode sys_immVal				{sys_opcode | opcode_flag_srcImm};

			const opcode nop_opcode				{0xAA};

			const opcode xchg_opcode			{0xE0};

			const opcode setcry_opcode			{0xE1};

			const opcode clrcry_opcode			{0xE2};

			const opcode clrint_opcode			{0xE3};

			const opcode dup_opcode				{0xE4};

			const opcode swap_opcode			{0xE5};

			const opcode brk_opcode				{0xFF};

		}

		namespace regs {
			// The CPU's general registers are defined here
			extern reg r0;
			extern reg r1;
			extern reg r2;
			extern reg r3;
			extern reg r4;
			extern reg r5;
			extern reg r6;
			extern reg r7;
			extern reg r8;
			extern reg r9;
			extern reg rt;
			extern reg rv;
			extern reg rf; // flags register
			extern reg ri; // instruction register
			extern reg rp; // program execution register
			extern reg rs; // stack register
		}

		extern bus address_bus;
		extern bus data_bus_0;
		extern bus data_bus_1;
		extern bus io_bus;

		extern memory_module mm;
		extern arithmetic_logic_unit alu;

		void add_device(u_qword id, device& new_device);
		void run();


	} // namespace cpu; 

} // namespace maize
