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
		const u_byte opflag_reg_rb	= 0b11010000; // slot $D: base pointer (BP); RI is decoder-internal, not operand-addressable (maize-41)
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

		class reg;
		class bus;
		class device;

		/* The reg_byte/reg_qword/reg_hword/reg_word union-alternative structs that
		   used to live here are gone (maize-30): reg_value now holds a single
		   non-union backing word and derives every sub-register view by explicit
		   shift/mask arithmetic, so there is no inactive-union-member type punning
		   for the optimizer to miscompile. The flag<> template that shared this
		   block has moved to after class reg's complete definition (below), so its
		   flag_reg.w0 access resolves against a complete type under two-phase
		   lookup on every compiler (clang included). */

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

		/* Compile-time-positioned sub-register view over a single backing word
		   (maize-30). Reproduces the old union's bit-position-to-subregister
		   mapping (README: $FEDCBA9876543210 => b7=$FE .. b0=$10) via explicit
		   shift/mask arithmetic, so it is host-endianness-independent and free of
		   inactive-union-member type punning. T is the primitive value type
		   (u_hword/u_qword/u_byte); Shift is the low-bit position of the field. */
		template <typename T, unsigned Shift>
		class subword_ref {
		public:
			explicit subword_ref(u_word& backing) : storage_ {backing} {}

			operator T() const { return static_cast<T>(storage_ >> Shift); }

			subword_ref& operator=(T value) {
				// ~u_word{0} >> (64 - bits) is safe for bits==64 (shift-by-0);
				// (1 << bits) - 1 would be UB for the 64-bit case (shift-by-64).
				constexpr u_word mask = (~u_word {0} >> (64 - sizeof(T) * 8)) << Shift;
				storage_ = (storage_ & ~mask) | ((static_cast<u_word>(value) << Shift) & mask);
				return *this;
			}

			subword_ref& operator=(const subword_ref& other) { return *this = static_cast<T>(other); }

			subword_ref& operator++() { return *this = static_cast<T>(static_cast<T>(*this) + 1); }
			T operator++(int) { T old = *this; ++*this; return old; }
			subword_ref& operator--() { return *this = static_cast<T>(static_cast<T>(*this) - 1); }
			T operator--(int) { T old = *this; --*this; return old; }
			subword_ref& operator+=(T v) { return *this = static_cast<T>(static_cast<T>(*this) + v); }
			subword_ref& operator-=(T v) { return *this = static_cast<T>(static_cast<T>(*this) - v); }

		private:
			u_word& storage_;
		};

		/* Runtime-indexed byte view over the backing word, returned by value from
		   reg_value::operator[] (maize-30). No call site takes its address or binds
		   it to a u_byte&, so a value proxy with operator=/operator u_byte()
		   preserves every existing operator[] use. */
		class byte_ref {
		public:
			byte_ref(u_word& backing, size_t index) : storage_ {backing}, index_ {index} {}

			operator u_byte() const { return static_cast<u_byte>(storage_ >> (index_ * 8)); }

			byte_ref& operator=(u_byte value) {
				const u_word mask = u_word {0xFF} << (index_ * 8);
				storage_ = (storage_ & ~mask) | ((static_cast<u_word>(value) << (index_ * 8)) & mask);
				return *this;
			}

			byte_ref& operator=(const byte_ref& other) { return *this = static_cast<u_byte>(other); }

		private:
			u_word& storage_;
			size_t index_;
		};

		struct reg_value {
			reg_value() {}
			reg_value(u_word init) : storage_ {init} {}

			/* Explicit copy operations that name ONLY storage_ (maize-30). Because
			   w0/h0/h1/q0..q3/b0..b7 are not named here, each falls back to its own
			   default member initializer ({storage_}), which per [class.base.init]
			   binds to THIS object's storage_, not the source's, closing the
			   reference-aliasing footgun a compiler-generated copy would open. */
			reg_value(const reg_value& other) : storage_ {other.storage_} {}
			reg_value& operator=(const reg_value& other) {
				storage_ = other.storage_;
				return *this;
			}

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

			byte_ref operator[](size_t index) {
				return byte_ref {storage_, index};
			}

			u_byte operator[](size_t index) const {
				return static_cast<u_byte>(storage_ >> (index * 8));
			}

			u_byte byte_index(size_t index) const {
				// TODO: range error handling
				return static_cast<u_byte>(storage_ >> (index * 8));
			}

			u_qword qword_index(size_t index) const {
				// TODO: range error handling
				return static_cast<u_qword>(storage_ >> (index * 16));
			}

			u_hword hword_index(size_t index) const {
				// TODO: range error handling
				return static_cast<u_hword>(storage_ >> (index * 32));
			}

			/* storage_ MUST be the first data member: declaration order governs
			   initialization order, so the single real object must exist before any
			   reference/proxy member's default member initializer binds to it. */
			u_word storage_ {0};

			u_word& w0 {storage_};
			subword_ref<u_hword,  0> h0 {storage_};
			subword_ref<u_hword, 32> h1 {storage_};
			subword_ref<u_qword,  0> q0 {storage_};
			subword_ref<u_qword, 16> q1 {storage_};
			subword_ref<u_qword, 32> q2 {storage_};
			subword_ref<u_qword, 48> q3 {storage_};
			subword_ref<u_byte,   0> b0 {storage_};
			subword_ref<u_byte,   8> b1 {storage_};
			subword_ref<u_byte,  16> b2 {storage_};
			subword_ref<u_byte,  24> b3 {storage_};
			subword_ref<u_byte,  32> b4 {storage_};
			subword_ref<u_byte,  40> b5 {storage_};
			subword_ref<u_byte,  48> b6 {storage_};
			subword_ref<u_byte,  56> b7 {storage_};
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

		/* flag<> lives here, AFTER class reg is a complete type (maize-30), so
		   flag_reg.w0 resolves against a complete type at template-definition parse
		   time under two-phase lookup on every compiler (clang no longer errors).
		   Latent bugs fixed while relocating: operator= read a nonexistent
		   that.value (now that.get()), compared this==that (flag* vs const flag&;
		   now this!=&that), and get()/operator bool() are const-qualified so they
		   are callable on the const reference operator= receives. */
		template <u_word flag_bit> class flag {
		public:
			flag(reg& reg_init, bool value = false) : flag_reg {reg_init} {
				set(value);
			}

			bool get() const {
				return (flag_reg.w0 & flag_bit);
			}

			void set(bool value) {
				flag_reg.w0 = ((flag_reg.w0 & ~flag_bit) | (value ? flag_bit : 0));
			}

			flag& operator=(const flag& that) {
				if (this != &that) {
					set(that.get());
				}

				return *this;
			}

			flag& operator=(bool value) {
				set(value);
				return *this;
			}

			bool operator==(bool value) {
				return get() == value;
			}

			operator bool() const {
				return get();
			}

		private:
			reg& flag_reg;
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
			std::vector<u_byte> read(reg_value address, u_word count);
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
			u_hword write_bytes(u_word address, u_word value, size_t count);
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

			/* The first two instructions */
			const opcode ld_opcode				{0x01};
			const opcode cp_regVal_reg			{ld_opcode | opcode_flag_srcReg};
			const opcode cp_immVal_reg			{ld_opcode | opcode_flag_srcImm};
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

			// DIV/MOD are signed; UDIV/UMOD are the unsigned variants (card maize-5).
			const opcode udiv_opcode			{0x35};
			const opcode udiv_regVal_reg		{udiv_opcode | opcode_flag_srcReg};
			const opcode udiv_immVal_reg		{udiv_opcode | opcode_flag_srcImm};
			const opcode udiv_regAddr_reg		{udiv_opcode | opcode_flag_srcAddr};
			const opcode udiv_immAddr_reg		{udiv_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode umod_opcode			{0x36};
			const opcode umod_regVal_reg		{umod_opcode | opcode_flag_srcReg};
			const opcode umod_immVal_reg		{umod_opcode | opcode_flag_srcImm};
			const opcode umod_regAddr_reg		{umod_opcode | opcode_flag_srcAddr};
			const opcode umod_immAddr_reg		{umod_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			// Add-with-carry / subtract-with-borrow (card maize-6): ADC = dst + src + C, SBB = dst - src - C.
			const opcode adc_opcode				{0x3B};
			const opcode adc_regVal_reg			{adc_opcode | opcode_flag_srcReg};
			const opcode adc_immVal_reg			{adc_opcode | opcode_flag_srcImm};
			const opcode adc_regAddr_reg		{adc_opcode | opcode_flag_srcAddr};
			const opcode adc_immAddr_reg		{adc_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode sbb_opcode				{0x3C};
			const opcode sbb_regVal_reg			{sbb_opcode | opcode_flag_srcReg};
			const opcode sbb_immVal_reg			{sbb_opcode | opcode_flag_srcImm};
			const opcode sbb_regAddr_reg		{sbb_opcode | opcode_flag_srcAddr};
			const opcode sbb_immAddr_reg		{sbb_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			/* Wide multiply (card maize-7): 3-operand src/dst/hi form (same encoding shape as
			   LEA/CMPXCHG). MULW is signed, UMULW unsigned; both write the low half to dst and
			   the high half to hi. Plain MUL ($05) is unchanged. */
			const opcode mulw_opcode			{0x3D};
			const opcode mulw_regVal_regreg		{mulw_opcode | opcode_flag_srcReg};
			const opcode mulw_immVal_regreg		{mulw_opcode | opcode_flag_srcImm};
			const opcode mulw_regAddr_regreg	{mulw_opcode | opcode_flag_srcAddr};
			const opcode mulw_immAddr_regreg	{mulw_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode umulw_opcode			{0x3E};
			const opcode umulw_regVal_regreg	{umulw_opcode | opcode_flag_srcReg};
			const opcode umulw_immVal_regreg	{umulw_opcode | opcode_flag_srcImm};
			const opcode umulw_regAddr_regreg	{umulw_opcode | opcode_flag_srcAddr};
			const opcode umulw_immAddr_regreg	{umulw_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

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

			// Arithmetic (sign-preserving) right shift (card maize-54). Base opcode $2E, from
			// the spec's surveyed-free set. The spec recommended $3F, but $3F's immAddr form
			// ($3F | srcImm | srcAddr = $FF) collides with brk_opcode; likewise $21/$23/$25's
			// immAddr forms hit SETCRY/CLRINT/SWAP and $2A's srcAddr form hits NOP. $2E's four
			// forms ($2E/$6E/$AE/$EE) are all unused. run_alu switches on
			// (alu.b0 & opflag_code=$3F), so all four forms collapse to the single sar_opcode case.
			const opcode sar_opcode				{0x2E};
			const opcode sar_regVal_reg			{sar_opcode | opcode_flag_srcReg};
			const opcode sar_immVal_reg			{sar_opcode | opcode_flag_srcImm};
			const opcode sar_regAddr_reg		{sar_opcode | opcode_flag_srcAddr};
			const opcode sar_immAddr_reg		{sar_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

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

			/* CPZ is the zero-extending copy. Its base opcode ($13) keeps the two value forms;
			   the address forms ($93 / $D3) were LDZ, removed as redundant (card maize-29) and
			   returned to reserved. */
			const opcode cpz_opcode				{0x13};
			const opcode cpz_regVal_reg			{cpz_opcode | opcode_flag_srcReg};
			const opcode cpz_immVal_reg			{cpz_opcode | opcode_flag_srcImm};

			const opcode out_opcode				{0x14};
			const opcode out_regVal_imm			{out_opcode | opcode_flag_srcReg};
			const opcode out_immVal_imm			{out_opcode | opcode_flag_srcImm};
			const opcode out_regAddr_imm		{out_opcode | opcode_flag_srcAddr};
			const opcode out_immAddr_imm		{out_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			/* JMP (base slot $16) keeps all four operand forms. In the flat-64 model a
			   control-flow target is always a full 64-bit address, so JMP now targets the
			   full width regardless of any operand sub-register selection (the divergent
			   full-width-forcing role LNGJMP used to play is folded in here). LNGJMP is
			   removed and base slot $15 is freed (card maize-64). */
			const opcode jmp_opcode {0x16};
			const opcode jmp_regVal {jmp_opcode | opcode_flag_srcReg};
			const opcode jmp_immVal {jmp_opcode | opcode_flag_srcImm};
			const opcode jmp_regAddr {jmp_opcode | opcode_flag_srcAddr};
			const opcode jmp_immAddr {jmp_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			/* Condition family (cards maize-55 / maize-64): the two high opcode bits select
			   the condition "row" and the base slot selects the "column". Together
			   (row * 3 + col) they index ONE predicate table (cpu.cpp's eval_condition)
			   shared by both SETcc and Jcc, so the flag formulas have a single source of
			   truth. Condition order:
			     0 Z   1 NZ  2 LT | 3 B   4 GT  5 A | 6 GE  7 LE  8 BE | 9 AE
			   Row 11 columns 1 and 2 are the two UNALLOCATED spare encodings per family
			   (Jcc $D8/$D9, SETcc $EC/$ED); recorded claimants: integer-overflow JO/JNO +
			   SETO/SETNO, and IEEE unordered-compare. */
			const u_byte cond_row_0 {0b00000000};
			const u_byte cond_row_1 {0b01000000};
			const u_byte cond_row_2 {0b10000000};
			const u_byte cond_row_3 {0b11000000};

			/* Conditional branches (Jcc), card maize-64: IMMEDIATE target only (the register
			   / indirect forms are removed; conditional-indirect synthesizes as an inverted
			   Jcc over JMP reg). Three base slots ($17/$18/$19); the freed slots
			   $1A/$1B/$1C/$37/$38/$39/$3A return to reserved. */
			const opcode jcc_base {0x17}; // column 0; col1 = $18, col2 = $19
			const opcode jz_opcode  {static_cast<opcode>(cond_row_0 | (jcc_base + 0))}; // $17
			const opcode jnz_opcode {static_cast<opcode>(cond_row_0 | (jcc_base + 1))}; // $18
			const opcode jlt_opcode {static_cast<opcode>(cond_row_0 | (jcc_base + 2))}; // $19
			const opcode jb_opcode  {static_cast<opcode>(cond_row_1 | (jcc_base + 0))}; // $57
			const opcode jgt_opcode {static_cast<opcode>(cond_row_1 | (jcc_base + 1))}; // $58
			const opcode ja_opcode  {static_cast<opcode>(cond_row_1 | (jcc_base + 2))}; // $59
			const opcode jge_opcode {static_cast<opcode>(cond_row_2 | (jcc_base + 0))}; // $97
			const opcode jle_opcode {static_cast<opcode>(cond_row_2 | (jcc_base + 1))}; // $98
			const opcode jbe_opcode {static_cast<opcode>(cond_row_2 | (jcc_base + 2))}; // $99
			const opcode jae_opcode {static_cast<opcode>(cond_row_3 | (jcc_base + 0))}; // $D7

			// SETcc (card maize-55): materialize a condition as 0/1 in one register operand,
			// using the shared predicate table above. Three base slots ($2B/$2C/$2D) with the
			// condition row in the two high bits; $EC/$ED remain reserved.
			const opcode setcc_base {0x2B}; // column 0; col1 = $2C, col2 = $2D
			const opcode setz_opcode  {static_cast<opcode>(cond_row_0 | (setcc_base + 0))}; // $2B
			const opcode setnz_opcode {static_cast<opcode>(cond_row_0 | (setcc_base + 1))}; // $2C
			const opcode setlt_opcode {static_cast<opcode>(cond_row_0 | (setcc_base + 2))}; // $2D
			const opcode setb_opcode  {static_cast<opcode>(cond_row_1 | (setcc_base + 0))}; // $6B
			const opcode setgt_opcode {static_cast<opcode>(cond_row_1 | (setcc_base + 1))}; // $6C
			const opcode seta_opcode  {static_cast<opcode>(cond_row_1 | (setcc_base + 2))}; // $6D
			const opcode setge_opcode {static_cast<opcode>(cond_row_2 | (setcc_base + 0))}; // $AB
			const opcode setle_opcode {static_cast<opcode>(cond_row_2 | (setcc_base + 1))}; // $AC
			const opcode setbe_opcode {static_cast<opcode>(cond_row_2 | (setcc_base + 2))}; // $AD
			const opcode setae_opcode {static_cast<opcode>(cond_row_3 | (setcc_base + 0))}; // $EB

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

			/* Unary register-only slot B (card maize-64): CLR (row0 $32), POP (row1 $72);
			   rows 2/3 ($B2/$F2) reserved. */
			const opcode clr_opcode {0x32};

			const opcode int_opcode				{0x24};
			const opcode int_regVal				{int_opcode | opcode_flag_srcReg};
			const opcode int_immVal				{int_opcode | opcode_flag_srcImm};

			const opcode pop_opcode {static_cast<opcode>(cond_row_1 | 0x32)}; // $72

			/* Zero-operand family (card maize-64): both mode bits dead, four per slot.
			   HALT stays pinned at $00 (zeroed memory halts; dedicated slot).
			     slot $27: RET (row0 $27), IRET (row1 $67), NOP (row2 $A7), BRK (row3 $E7)
			     slot $29: SETINT (row0 $29), CLRINT (row1 $69), SETCRY (row2 $A9), CLRCRY (row3 $E9) */
			const opcode ret_opcode {static_cast<opcode>(cond_row_0 | 0x27)}; // $27
			const opcode iret_opcode {static_cast<opcode>(cond_row_1 | 0x27)}; // $67
			const opcode setint_opcode {static_cast<opcode>(cond_row_0 | 0x29)}; // $29

			const opcode cmpind_opcode			{0x2F};
			const opcode cmpind_regVal_regAddr	{cmpind_opcode | opcode_flag_srcReg};
			const opcode cmpind_immVal_regAddr	{cmpind_opcode | opcode_flag_srcImm};

			const opcode testind_opcode			{0x30};
			const opcode testind_regVal_regAddr	{testind_opcode | opcode_flag_srcReg};
			const opcode testind_immVal_regAddr	{testind_opcode | opcode_flag_srcImm};

			/* Unary register-only slot A (card maize-64): the ALU micro-ops, packed by the
			   condition-style row bits. INC (row0 $31), DEC (row1 $71), NOT (row2 $B1),
			   NEG (row3 $F1). NEG is two's-complement negate (QBE emits `neg`). tick()
			   translates the row to a low-6-unique ALU selector before run_alu. */
			const opcode inc_opcode {static_cast<opcode>(cond_row_0 | 0x31)}; // $31
			const opcode dec_opcode {static_cast<opcode>(cond_row_1 | 0x31)}; // $71
			const opcode not_opcode {static_cast<opcode>(cond_row_2 | 0x31)}; // $B1
			const opcode neg_opcode {static_cast<opcode>(cond_row_3 | 0x31)}; // $F1

			const opcode sys_opcode				{0x34};
			const opcode sys_regVal				{sys_opcode | opcode_flag_srcReg};
			const opcode sys_immVal				{sys_opcode | opcode_flag_srcImm};

			const opcode nop_opcode {static_cast<opcode>(cond_row_2 | 0x27)}; // $A7

			const opcode xchg_opcode			{0xE0};

			const opcode setcry_opcode {static_cast<opcode>(cond_row_2 | 0x29)}; // $A9
			const opcode clrcry_opcode {static_cast<opcode>(cond_row_3 | 0x29)}; // $E9
			const opcode clrint_opcode {static_cast<opcode>(cond_row_1 | 0x29)}; // $69
			/* DUP ($E4) and SWAP ($E5) are killed (card maize-64): header-only ghosts with
			   no dispatch, mazm entry, or README row; removed before the freeze. */

			const opcode brk_opcode {0xFF}; // $FF sentinel: a run of $FF-filled / erased memory traps, mirroring HALT at $00. Occupies only base $3F's mode-11 form; $3F/$7F/$BF stay free for future full-byte-dispatched ops (a mask-to-base ALU op cannot sit at base $3F, since its immAddr form would be $FF).

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
			extern reg ri; // instruction register (decoder-internal; not operand-addressable, maize-41)
			extern reg rb; // base pointer register (BP); operand slot $D (maize-41)
			extern reg rp; // program execution register (PC); full 64-bit (maize-41)
			extern reg rs; // stack register (SP); full 64-bit (maize-41)
		}

		extern bus address_bus;
		extern bus data_bus_0;
		extern bus data_bus_1;
		extern bus io_bus;

		extern memory_module mm;
		extern arithmetic_logic_unit alu;

		void add_device(u_qword id, device& new_device);
		void run();
		void power_off();


	} // namespace cpu; 

} // namespace maize
