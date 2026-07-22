#pragma once 
#include <cstdint>
#include <cstddef>
#include <vector>
#include <map>
#include <unordered_map>
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

		/* The compile-time-positioned subword_ref proxy class (maize-30) is gone
		   (maize-210). reg_value no longer holds 14 self-referential subword_ref
		   members over a separate backing word; the register IS its single u_word
		   w0, and every sub-register view is now a bit-exact accessor method
		   (sub_get/sub_set over w0) on reg_value below. This collapses sizeof from
		   ~128 bytes (8 bytes of state + 15 reference-init proxies) to 8 bytes and
		   drops the 15 pointer-init stores every reg_value temporary paid. The
		   shift/mask arithmetic is reproduced verbatim in sub_get/sub_set, so the
		   read/write masking stays byte-identical to the old proxy operators. */

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
			reg_value(u_word init) : w0 {init} {}

			/* The register IS w0 now (maize-210): a single plain u_word data member,
			   no separate backing word and no self-referential proxy members. Copy
			   is therefore a trivial 8-byte copy of w0; the explicit forms are kept
			   only to document that intent (a compiler-generated copy is identical
			   and no longer opens the reference-aliasing footgun the old proxy
			   members did). */
			reg_value(const reg_value& other) : w0 {other.w0} {}
			reg_value& operator=(const reg_value& other) {
				w0 = other.w0;
				return *this;
			}

			operator u_word() { return w0; }

			explicit operator u_hword() {
				return h0();
			}

			explicit operator u_qword() {
				return q0();
			}

			explicit operator u_byte() {
				return b0();
			}

			u_word operator=(u_word value) {
				return w0 = value;
			}

			byte_ref operator[](size_t index) {
				return byte_ref {w0, index};
			}

			u_byte operator[](size_t index) const {
				return static_cast<u_byte>(w0 >> (index * 8));
			}

			u_byte byte_index(size_t index) const {
				// TODO: range error handling
				return static_cast<u_byte>(w0 >> (index * 8));
			}

			u_qword qword_index(size_t index) const {
				// TODO: range error handling
				return static_cast<u_qword>(w0 >> (index * 16));
			}

			u_hword hword_index(size_t index) const {
				// TODO: range error handling
				return static_cast<u_hword>(w0 >> (index * 32));
			}

			/* w0 is THE backing word and the sole data member (replaces the old
			   storage_ + 15 proxies). sizeof(reg_value) == 8. */
			u_word w0 {0};

			/* Bit-exact reproduction of the old subword_ref<T,Shift> operators over
			   w0. sub_get is subword_ref::operator T(); sub_set is its operator=(T),
			   mask expression verbatim (the ~u_word{0} >> (64 - bits) form is safe at
			   bits==64, unlike (1 << bits) - 1). Do not "simplify" the mask. */
			template <typename T, unsigned Shift> T sub_get() const {
				return static_cast<T>(w0 >> Shift);
			}
			template <typename T, unsigned Shift> void sub_set(T value) {
				constexpr u_word mask = (~u_word {0} >> (64 - sizeof(T) * 8)) << Shift;
				w0 = (w0 & ~mask) | ((static_cast<u_word>(value) << Shift) & mask);
			}

			/* Named sub-register accessors. Getters keep the bare subword name
			   (reg.b3 -> reg.b3()); setters take a set_ prefix (reg.b3 = x ->
			   reg.set_b3(x)). Shift constants match the old member declarations. */
			u_hword h0() const { return sub_get<u_hword,  0>(); }
			u_hword h1() const { return sub_get<u_hword, 32>(); }
			u_qword q0() const { return sub_get<u_qword,  0>(); }
			u_qword q1() const { return sub_get<u_qword, 16>(); }
			u_qword q2() const { return sub_get<u_qword, 32>(); }
			u_qword q3() const { return sub_get<u_qword, 48>(); }
			u_byte  b0() const { return sub_get<u_byte,   0>(); }
			u_byte  b1() const { return sub_get<u_byte,   8>(); }
			u_byte  b2() const { return sub_get<u_byte,  16>(); }
			u_byte  b3() const { return sub_get<u_byte,  24>(); }
			u_byte  b4() const { return sub_get<u_byte,  32>(); }
			u_byte  b5() const { return sub_get<u_byte,  40>(); }
			u_byte  b6() const { return sub_get<u_byte,  48>(); }
			u_byte  b7() const { return sub_get<u_byte,  56>(); }

			void set_h0(u_hword v) { sub_set<u_hword,  0>(v); }
			void set_h1(u_hword v) { sub_set<u_hword, 32>(v); }
			void set_q0(u_qword v) { sub_set<u_qword,  0>(v); }
			void set_q1(u_qword v) { sub_set<u_qword, 16>(v); }
			void set_q2(u_qword v) { sub_set<u_qword, 32>(v); }
			void set_q3(u_qword v) { sub_set<u_qword, 48>(v); }
			void set_b0(u_byte v) { sub_set<u_byte,   0>(v); }
			void set_b1(u_byte v) { sub_set<u_byte,   8>(v); }
			void set_b2(u_byte v) { sub_set<u_byte,  16>(v); }
			void set_b3(u_byte v) { sub_set<u_byte,  24>(v); }
			void set_b4(u_byte v) { sub_set<u_byte,  32>(v); }
			void set_b5(u_byte v) { sub_set<u_byte,  40>(v); }
			void set_b6(u_byte v) { sub_set<u_byte,  48>(v); }
			void set_b7(u_byte v) { sub_set<u_byte,  56>(v); }
		};

		static_assert(sizeof(reg_value) == 8, "maize-210: reg_value must collapse to a bare u_word");

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

		/* Device base. A device is the shipped (address, data) register pair PLUS two
		   behavioral hooks. The base hooks are the passive-register passthrough shipped
		   before: on_port_write copies the CPU-side value into the backing reg's w0, and
		   on_port_read returns that w0. A plain `device` (the loopback scratch port, the
		   timer's three registers, the reserved block ports) therefore keeps identical
		   behavior. A host-backed device (console, keyboard, framebuffer) overrides the two
		   hooks to act at the moment of the port access. Adding virtuals gives `device` a
		   vtable, an internal layout change with no ISA or binary-format impact (the reg
		   data members are still accessed the same way). */
		class device : public reg {
		public:
			device() = default;
			virtual ~device() = default;
			reg address_reg;

			virtual void      on_port_write(reg_value const& value, subreg_enum value_subreg);
			virtual reg_value on_port_read(subreg_enum dst_subreg);
		};

		/* An instruction-boundary input source. At most one is active per run (the single
		   stdin consumer): the run loop calls on_input_tick() once per executed instruction,
		   mirroring the timer's per-instruction countdown tick, so a host input device pulls its bytes
		   and raises its IRQ on the CPU thread and never races the CPU thread's unsynchronized
		   RF accesses. */
		class input_device {
		public:
			virtual ~input_device() = default;
			virtual void on_input_tick() = 0;
		};

		/* Timer device (card maize-21): the first interrupt source and the end-to-end
		   proof of the interrupt mechanism. Time base is instruction-tick counting: the
		   active timer is advanced one countdown tick once per executed instruction and
		   fires its IRQ when the countdown reaches zero (deterministic and reproducible;
		   the host-time backend is deferred to the device-plugin work). The three
		   registers are reached as separate ports in a reserved low-port block; the
		   concrete pinout below is PROVISIONAL and is finalized with the device pinout.

		   Register model:
		     period_reg  (port timer_port_period):  reload value, in instruction ticks.
		     control_reg (port timer_port_control): bit 0 = enable, bit 1 = periodic.
		     status_reg  (port timer_port_status):  bit 0 = tick-pending. The handler
		       acknowledges by writing a value with bit 0 clear, which clears the source
		       and (periodic mode) re-arms the countdown. */
		class timer_device {
		public:
			timer_device() = default;
			device period_reg;
			device control_reg;
			device status_reg;
			u_word counter {0};
			u_byte irq_vector {32};
		};

		struct memory_module : public reg {
			~memory_module();

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
			/* Bulk-copy count bytes of guest memory into a host buffer (block-at-a-time
			   memcpy through the cache), no allocation. Used by the framebuffer present. */
			void read_into(u_word address, u_byte* dst, size_t count);
			/* Bulk-copy count bytes from a host buffer into guest memory (block-at-a-time
			   memcpy through the cache), no allocation. The symmetric write twin of
			   read_into; used by the maize-213 palette-blit syscall to land the converted
			   XRGB8888 destination at host memcpy speed instead of per-pixel write_word. */
			void write_from(u_word address, const u_byte* src, size_t count);
			u_byte read_byte(u_word address);
			u_word last_block() const;
			
			/* Block size is 2^block_shift bytes. 12 => 4 KB, the measured sweet spot for the
			   DOOM workload: the 256B->4KB step is +14% (fewer boundary crossings, wider L1
			   coverage), and larger blocks are flat/noise while wasting memory on sparse
			   guests. Changing block_shift is the only edit needed: block_mask and address_mask
			   derive from it, and the in-block offset is address & block_mask. */
			static constexpr unsigned block_shift {12};
			static constexpr u_word block_size {u_word {1} << block_shift};
			static constexpr u_word block_mask {block_size - 1};

			/* maize-330 (JIT J1): expose the L1 arrays' storage to the JIT so compiled
			   code can inline the block-cache hit path (a slot probe + tag compare +
			   host load/store), exactly mirroring set_cache_address's fast path. Read
			   access to the arrays only; guest bytes are written through the block
			   pointers the arrays already hand out. */
			u_byte* const* jit_l1_ptr_data() const { return l1_ptr.data(); }
			const u_word* jit_l1_base_data() const { return l1_base.data(); }
			static constexpr size_t jit_l1_mask() { return l1_mask; }

		protected:
			reg address_reg {0};
			u_word address_mask {~block_mask};
			u_word cache_base {~u_word {0}};

			size_t load_size {3};
			subreg_mask_enum store_mask {subreg_mask_enum::w0};
			bus* pload_bus {nullptr};
			bus* pstore_bus {nullptr};

			/* Guest RAM is sparse 256-byte blocks. The authoritative store is a hash map
			   (O(1) average), fronted by a direct-mapped L1 block cache so the hot access
			   pattern (code / texture / framebuffer / stack blocks alternating) resolves
			   with a single array index + tag compare, no tree walk and no hash. Blocks are
			   never freed during a run, so cached pointers stay valid (no invalidation). */
			static constexpr size_t l1_bits {13};
			static constexpr size_t l1_count {size_t(1) << l1_bits};   // 8192 slots
			static constexpr size_t l1_mask {l1_count - 1};
			std::unordered_map<u_word, u_byte*> memory_map;
			std::array<u_byte*, l1_count> l1_ptr {};   // nullptr = empty slot
			std::array<u_word, l1_count> l1_base {};   // block base for the slot (valid iff l1_ptr set)
			u_word highest_block_ {0};                 // max allocated block base (for last_block())
			bool any_block_ {false};
			u_byte* cache {nullptr};

			/* Resolve the block for `address` and return bytes-remaining-in-block. Defined
			   inline here (called ~150M times/frame from read/write) so it inlines into the
			   memory hot loops: the fast path is a base compare + one L1 array probe, and the
			   rare L1 miss (first touch / eviction) is the out-of-line resolve_block_miss. The
			   in-block offset the callers need is block_size - (return value). */
			size_t set_cache_address(u_word address) {
				u_word base = address & address_mask;
				if (cache_base != base) {
					cache_base = base;
					size_t slot = (base >> block_shift) & l1_mask;
					if (l1_ptr[slot] != nullptr && l1_base[slot] == base) {
						cache = l1_ptr[slot];
					}
					else {
						resolve_block_miss(base, slot);
					}
				}
				return block_size - (address & block_mask);
			}
			void resolve_block_miss(u_word base, size_t slot);
			u_hword write_bytes(u_word address, u_word value, size_t count);
		};

		class arithmetic_logic_unit : public reg {
		public:
			static const opcode opctrl_carryin	{0x80};
			static const opcode opflag_code		{0x3F}; // 0011`1111

			reg op1_reg;
			reg op2_reg;
		};


		/* Trap cause / vector numbering (card maize-78). Each synchronous trap has a
		   stable numeric cause that doubles as its index into the shared trap+interrupt
		   vector table (co-authored with maize-21). Synchronous traps occupy the low
		   range 0..31; external / device interrupts occupy 32.. (maize-21). Vector 1 is
		   intentionally left unassigned, reserving a slot for a future debug / single-step
		   trap. Vector 4 reserves privileged-op-in-user (candidate set finalized with
		   maize-21 / maize-92); vectors 5 and 6 reserve segment/bounds and stack-fault,
		   whose enforcement mechanism ships with maize-92; vector 7 reserves SYS / syscall
		   entry (a deliberate synchronous software trap, ABI owned by maize-82 / maize-21).
		   These numbers are frozen ISA contract; see docs/spec/trap-model.md for the full
		   model. */
		namespace trap {
			const u_byte cause_illegal_instruction	{0}; // unknown opcode or unallocated condition encoding
			const u_byte cause_divide_error			{2}; // divide-by-zero or signed INT_MIN/-1 quotient overflow
			const u_byte cause_breakpoint			{3}; // BRK ($FF)
			const u_byte cause_privileged_op		{4}; // reserved: privileged instruction executed in user mode
			const u_byte cause_segment_bounds		{5}; // reserved (maize-92): segment / bounds violation
			const u_byte cause_stack_fault			{6}; // reserved (maize-92): stack-limit violation
			const u_byte cause_syscall				{7}; // reserved: SYS / syscall entry (deliberate software trap; ABI owned by maize-82 / maize-21)
			const u_byte cause_page_fault			{8}; // Sv48 page-table walk: not-present or permission violation (maize-194)
		}

		/* Shared trap / interrupt vector table format (card maize-21, co-authored with
		   docs/spec/trap-model.md and docs/spec/device-surface.md). Fixed low base, 256
		   entries of 8 bytes each (2 KiB), indexed by cause number: entry[cause] holds a
		   full 64-bit handler address (synchronous traps 0..31, external interrupts
		   32..255). The base is pinned at 0x1000, one 4 KiB page above the null/zero
		   location, so the "null pointer reads 0" convention stays clean and an
		   uninstalled (zero) entry is unambiguous. An out-of-range vector or a zero
		   entry is a deterministic halt with the cause surfaced, never an out-of-bounds
		   read or a map-miss dereference. A relocatable base via the reserved control
		   register is a deferred v1.x extension. */
		const u_word trap_vector_table_base		{0x0000000000001000};
		const u_word trap_vector_entry_size		{8};
		const u_word trap_vector_entry_count	{256};

		/* Provisional timer pinout (card maize-21). A reserved low-port block and IRQ
		   vector 32 (the first index above the synchronous-trap range) so the end-to-end
		   timer proof runs without blocking on the device-pinout work, which finalizes
		   the concrete port assignments. The block sits below port 0x80 so a natural
		   8-bit immediate port operand reaches it without the shipped immediate
		   sign-extension pushing the low-16-bit port id (the frozen .q0 field) into the
		   high port range. */
		const u_qword timer_port_period			{0x0040};
		const u_qword timer_port_control		{0x0041};
		const u_qword timer_port_status			{0x0042};
		const u_byte  timer_irq_vector			{32};

		/* Concrete standard-device pinout (ratified). All standard devices sit in a
		   reserved low-port block below 0x80 so a natural 8-bit immediate port operand
		   reaches them without the frozen .q0 truncation + immediate sign-extension gotcha.
		   IRQ vectors are external-interrupt vectors (32..255). Block ports are reserved on
		   this card (no backend). The framebuffer is memory-backed: pixels live in ordinary
		   guest RAM and are presented through the base-address + present control ports, not
		   a per-pixel data port. */
		const u_qword console_port_data			{0x0000};   // R: input byte   W: output byte
		const u_qword console_port_status		{0x0001};   // R: bit0 input-available, bit1 output-ready, bit2 end-of-input (EOF, latched)
		const u_byte  console_irq_vector		{33};

		const u_qword loopback_test_port		{0x000F};   // R/W passive scratch (relocated from port 1)

		const u_qword keyboard_port_data		{0x0010};   // R: scancode (read clears key-available)
		const u_qword keyboard_port_status		{0x0011};   // R: bit0 key-available
		const u_byte  keyboard_irq_vector		{34};

		const u_qword block_port_lba			{0x0020};   // reserved: no backend this card
		const u_qword block_port_data			{0x0021};
		const u_qword block_port_control		{0x0022};
		const u_byte  block_irq_vector			{35};

		const u_qword fb_port_width				{0x0050};   // R: pixels (host config)
		const u_qword fb_port_height			{0x0051};   // R: pixels (host config)
		const u_qword fb_port_format			{0x0052};   // R: format id (1 = XRGB8888)
		const u_qword fb_port_base				{0x0053};   // R/W: guest address of the pixel buffer
		const u_qword fb_port_present			{0x0054};   // W: present a frame; R: bit0 last-present-valid
		const u_qword fb_port_status			{0x0055};   // R: bit0 vsync-pending, bit2 register-rejected; W bit1 vsync-IRQ-enable / ack
		/* maize-236: the registration table control ports. $53/$54/$55 above are
		   slot-relative on whichever slot $56 selects; $57 switches the scanned-out
		   surface. Both sit in the free range immediately after the fb block. */
		const u_qword fb_port_slot				{0x0056};   // R/W: select the target slot (0..fb_max_slots-1) for $53/$54/$55
		const u_qword fb_port_activate			{0x0057};   // R/W: W switch active surface (slot or fb_console_sentinel); R the active slot
		const u_byte  fb_irq_vector				{36};
		const u_hword fb_format_xrgb8888		{1};
		/* maize-236: fixed registration-table capacity (Decision Q3). A fixed constant
		   leaks into no ABI: the $56 slot-select range and the syscall contract are
		   identical whether this is a compile-time constant or a boot flag. */
		const int     fb_max_slots				{8};
		/* maize-236: the $57 ACTIVATE sentinel selecting the text console (no framebuffer
		   slot). A guest slot index is 0..fb_max_slots-1, so this out-of-range value can
		   never collide with a real slot. */
		const u_word  fb_console_sentinel		{0xFFFFFFFFu};


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
			   the address forms ($93 / $D3) are LDZ, the zero-extending load (reintroduced by
			   card maize-204 after maize-29 briefly removed the original LDZ as redundant). */
			const opcode cpz_opcode				{0x13};
			const opcode cpz_regVal_reg			{cpz_opcode | opcode_flag_srcReg};
			const opcode cpz_immVal_reg			{cpz_opcode | opcode_flag_srcImm};

			/* LDZ (reintroduced, card maize-204): the zero-extending load. Shares CPZ's base
			   slot ($13) the way LD shares CP's ($01); only the two address forms are defined
			   (a load always reads memory). $93/$D3 were reserved for this since maize-29. */
			const opcode ldz_regAddr_reg		{cpz_opcode | opcode_flag_srcAddr};
			const opcode ldz_immAddr_reg		{cpz_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

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
			/* maize-272: PUSHALL spends the PUSH slot's reserved row 2 ($A0), the
			   micro-op-headroom pattern LDZ/SETSYSG established (in-family spend, no
			   free base slot consumed; $37/$38 stay earmarked per reservations.md).
			   Zero-operand: pushes the 13-register process-context block R0..R9,RT,
			   RV,RB (R0 pushed last, so RS lands on the R0 slot), the exact frame
			   quesOS's trap handlers save. POPALL ($B2, the POP slot's reserved row
			   2) is the exact inverse. */
			const opcode pushall_opcode {static_cast<opcode>(0xA0)};

			/* Unary register-only slot B (card maize-64): CLR (row0 $32), POP (row1 $72);
			   row 3 ($F2) reserved; row 2 ($B2) spent as POPALL (maize-272, above). */
			const opcode clr_opcode {0x32};
			const opcode popall_opcode {static_cast<opcode>(0xB2)};

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

			/* Syscall-provider mode select (card maize-24, decision D9 Shape B): two
			   zero-operand set/clear ops mirroring SETINT/CLRINT. They toggle the RF
			   syscall-guest bit (bit_syscall_guest) that selects which provider SYS
			   dispatches to. CLEAR (boot default): SYS calls the native sys::call
			   provider byte-identical to v1.0. SET: SYS traps through cause 7 to the
			   guest-installed handler (deliver_vectored), the guest-owned dispatch path.
			   quesOS sets the flag once its cause-7 handler is resident and toggles it
			   clear/set around a re-issued SYS to pass file/IO calls through to native.
			   Encoding: the two free reserved rows of the $24 INT (software-interrupt)
			   slot, $A4 / $E4, an adjacent pair in the trap-model neighborhood. This
			   spends no fully-free base slot ($26/$28/$37/$38 stay reserved for their
			   v1.x claimants; $26/$28 feed the paging/segment control-register work in
			   maize-180). Default-clear preserves every v1.0 binary (hello.mzb pinned). */
			const opcode setsysg_opcode {static_cast<opcode>(cond_row_2 | 0x24)}; // $A4
			const opcode clrsysg_opcode {static_cast<opcode>(cond_row_3 | 0x24)}; // $E4
			/* $E5 (former SWAP, freed in card maize-64) remains a reserved ghost row: no
			   dispatch, mazm entry, or README row. Its pair $E4 (former DUP) has since been
			   reclaimed as clrsysg_opcode above (card maize-24). */

			/* MMU foundation: control-register access + TLB control (card maize-180). Two
			   privileged base slots the freeze reserved for the paging/MMU work
			   (docs/spec/reservations.md §1: $26 control-register access, $28 paging / MMU
			   control), row-packed the same way $24 row-packs INT / SETSYSG / CLRSYSG: the
			   row bit selects a distinct privileged instruction, it is NOT an addressing-mode
			   tag. All forms are privileged (cause-4 privileged-operation fault in user mode).

			   MOVTCR / MOVFCR ($26) reach the small privileged control-register file (the D2
			   access mechanism, distinct from device ports and the syscall path):
			     $26 MOVTCR Rsrc, crn (regVal-imm)   $66 MOVTCR imm, crn (immVal-imm)
			     $A6 MOVFCR crn, Rdst (immVal-reg)   $E6 reserved (future reg-indexed MOVFCR)
			   MOVTCR's two forms follow the source-mode bit ($26 register source, $66
			   immediate source); MOVFCR is a fixed opcode ($A6), its operands a leading
			   immediate CR index and a trailing destination register.

			   TLBINV / TLBINVA ($28) invalidate software-TLB entries. Under maize-180 both
			   are privileged NO-OPS: no software TLB exists yet (maize-194 builds the walk +
			   TLB and gives these bodies). They still assemble, decode, and privilege-gate.
			     $28 TLBINV (zero-op, invalidate-all)   $68 TLBINVA Rn (invalidate-one)
			     $A8 / $E8 reserved (future ASID-scoped invalidate).
			   "Paging-enable" gets no dedicated opcode: it is a MOVTCR write to CR0.MODE. */
			const opcode movtcr_opcode {0x26};
			const opcode movtcr_regVal_imm {static_cast<opcode>(movtcr_opcode | opcode_flag_srcReg)};  // $26
			const opcode movtcr_immVal_imm {static_cast<opcode>(movtcr_opcode | opcode_flag_srcImm)};  // $66
			const opcode movfcr_immVal_reg {static_cast<opcode>(movtcr_opcode | opcode_flag_srcAddr)}; // $A6

			const opcode tlbinv_opcode {0x28};                                    // $28 (row 0, zero-op)
			const opcode tlbinva_opcode {static_cast<opcode>(cond_row_1 | 0x28)}; // $68 (row 1, one register operand)

			const opcode brk_opcode {0xFF}; // $FF sentinel: a run of $FF-filled / erased memory traps, mirroring HALT at $00. Occupies only base $3F's mode-11 form; $3F/$7F/$BF stay free for future full-byte-dispatched ops (a mask-to-base ALU op cannot sit at base $3F, since its immAddr form would be $FF).

			/* ===== Floating-point ISA (card maize-122) =====================
			   Zfinx: FP ops read operands from and write results to the existing
			   integer register file; format width (binary32 vs binary64) comes
			   from the per-operand subregister field (H0/H1 => binary32, W0 =>
			   binary64; a B* or Q* subregister on an FP operand is an illegal-operand
			   trap). No separate FP bank, no FP load/store/move (LD/ST/CP already
			   move the bits), no NaN-boxing. Base slots are drawn from maize-64's
			   freed pool; every addressing-mode form was verified collision-free
			   against the landed post-maize-64 opcode map before commit. */

			/* 3a. Arithmetic: four addressing-mode forms each, same shape as ADD. */
			const opcode fadd_opcode			{0x1A};
			const opcode fadd_regVal_reg		{fadd_opcode | opcode_flag_srcReg};
			const opcode fadd_immVal_reg		{fadd_opcode | opcode_flag_srcImm};
			const opcode fadd_regAddr_reg		{fadd_opcode | opcode_flag_srcAddr};
			const opcode fadd_immAddr_reg		{fadd_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode fsub_opcode			{0x1B};
			const opcode fsub_regVal_reg		{fsub_opcode | opcode_flag_srcReg};
			const opcode fsub_immVal_reg		{fsub_opcode | opcode_flag_srcImm};
			const opcode fsub_regAddr_reg		{fsub_opcode | opcode_flag_srcAddr};
			const opcode fsub_immAddr_reg		{fsub_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode fmul_opcode			{0x1C};
			const opcode fmul_regVal_reg		{fmul_opcode | opcode_flag_srcReg};
			const opcode fmul_immVal_reg		{fmul_opcode | opcode_flag_srcImm};
			const opcode fmul_regAddr_reg		{fmul_opcode | opcode_flag_srcAddr};
			const opcode fmul_immAddr_reg		{fmul_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode fdiv_opcode			{0x21};
			const opcode fdiv_regVal_reg		{fdiv_opcode | opcode_flag_srcReg};
			const opcode fdiv_immVal_reg		{fdiv_opcode | opcode_flag_srcImm};
			const opcode fdiv_regAddr_reg		{fdiv_opcode | opcode_flag_srcAddr};
			const opcode fdiv_immAddr_reg		{fdiv_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			/* 3e. Compare: four addressing-mode forms, like CMP; writes integer flags. */
			const opcode fcmp_opcode			{0x2A};
			const opcode fcmp_regVal_reg		{fcmp_opcode | opcode_flag_srcReg};
			const opcode fcmp_immVal_reg		{fcmp_opcode | opcode_flag_srcImm};
			const opcode fcmp_regAddr_reg		{fcmp_opcode | opcode_flag_srcAddr};
			const opcode fcmp_immAddr_reg		{fcmp_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			/* 3b. Unary register-only, row-packed at base $22: FSQRT (row0 $22),
			   FNEG (row1 $62), FABS (row2 $A2); row3 ($E2) reserved. */
			const opcode fsqrt_opcode {static_cast<opcode>(cond_row_0 | 0x22)}; // $22
			const opcode fneg_opcode  {static_cast<opcode>(cond_row_1 | 0x22)}; // $62
			const opcode fabs_opcode  {static_cast<opcode>(cond_row_2 | 0x22)}; // $A2

			/* 3c. Fused multiply-add: 3-operand regreg form, like MULW. FNMADD/
			   FNMSUB are synthesized via the exact FNEG (not primitives). */
			const opcode fmadd_opcode			{0x23};
			const opcode fmadd_regVal_regreg	{fmadd_opcode | opcode_flag_srcReg};
			const opcode fmadd_immVal_regreg	{fmadd_opcode | opcode_flag_srcImm};
			const opcode fmadd_regAddr_regreg	{fmadd_opcode | opcode_flag_srcAddr};
			const opcode fmadd_immAddr_regreg	{fmadd_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			const opcode fmsub_opcode			{0x25};
			const opcode fmsub_regVal_regreg	{fmsub_opcode | opcode_flag_srcReg};
			const opcode fmsub_immVal_regreg	{fmsub_opcode | opcode_flag_srcImm};
			const opcode fmsub_regAddr_regreg	{fmsub_opcode | opcode_flag_srcAddr};
			const opcode fmsub_immAddr_regreg	{fmsub_opcode | opcode_flag_srcImm | opcode_flag_srcAddr};

			/* 3d. Min/max register-only, row-packed at base $33: FMIN (row0 $33),
			   FMAX (row1 $73); rows 2/3 ($B3/$F3) reserved. */
			const opcode fmin_opcode {static_cast<opcode>(cond_row_0 | 0x33)}; // $33
			const opcode fmax_opcode {static_cast<opcode>(cond_row_1 | 0x33)}; // $73

			/* 3f. Conversions register-only, row-packed. Base $39: FCVTFF (row0
			   $39), FCVTFS (row1 $79), FCVTFU (row2 $B9); row3 ($F9) reserved.
			   Base $3A: FCVTSF (row0 $3A), FCVTUF (row1 $7A); rows 2/3 ($BA/$FA)
			   reserved. Per-operand widths come from the two subregister fields. */
			const opcode fcvtff_opcode {static_cast<opcode>(cond_row_0 | 0x39)}; // $39 float<->float
			const opcode fcvtfs_opcode {static_cast<opcode>(cond_row_1 | 0x39)}; // $79 float->signed int
			const opcode fcvtfu_opcode {static_cast<opcode>(cond_row_2 | 0x39)}; // $B9 float->unsigned int
			const opcode fcvtsf_opcode {static_cast<opcode>(cond_row_0 | 0x3A)}; // $3A signed int->float
			const opcode fcvtuf_opcode {static_cast<opcode>(cond_row_1 | 0x3A)}; // $7A unsigned int->float

			/* FCSR access, register-only, row-packed at base $15 (a maize-64 freed
			   slot outside the spec's 11-op arithmetic budget). The dedicated FCSR
			   architectural register (FRM + FFLAGS) is not one of the 16 operand-
			   addressable registers, so these two ops are the software path to read
			   and write it (set the rounding mode, read/clear the sticky flags).
			   FGETCSR (row0 $15) copies FCSR into the operand register; FSETCSR
			   (row1 $55) copies the operand register into FCSR; rows 2/3 reserved. */
			const opcode fgetcsr_opcode {static_cast<opcode>(cond_row_0 | 0x15)}; // $15
			const opcode fsetcsr_opcode {static_cast<opcode>(cond_row_1 | 0x15)}; // $55

			/* Unordered predicate JP/SETP (card maize-122): claims one of maize-64's
			   two reserved spare condition encodings (the recorded "IEEE unordered-
			   compare" claimant). JP is Jcc row3 col1 ($D8); SETP is SETcc row3 col1
			   ($EC). Both fold to the same shared-predicate index (10), which
			   eval_condition maps to the parity bit set by FCMP on an unordered
			   result. JNP/SETNP are synthesized by branch inversion (no opcode); the
			   remaining spares ($D9/$ED) stay reserved for the JO/JNO claimant. */
			const opcode jp_opcode   {static_cast<opcode>(cond_row_3 | (jcc_base + 1))};   // $D8
			const opcode setp_opcode {static_cast<opcode>(cond_row_3 | (setcc_base + 1))}; // $EC

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
			extern reg fcsr; // FP control/status register: FRM + FFLAGS (maize-122); not operand-addressable
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
		void enable_perf_counter();   // --show-perf: start counting guest instructions
		void enable_profile(u_word interval);   // --profile: sample RP every `interval` instructions (maize-261)
		const std::unordered_map<u_word, std::uint64_t>& profile_histogram();   // sampled PC -> hits
		void enable_jit_survey();               // --jit-survey: block-shape survey, no codegen (JIT J0)
		void jit_survey_report(std::ostream& out);   // printed at exit when the survey ran

		/* Tier-1 template JIT (maize-330, JIT J1). enable_jit(false) turns on compiled
		   execution of hot Bare-mode code paths; enable_jit(true) additionally verifies
		   every compiled block against the interpreter (--jit-check, the differential
		   miscompile net). On a host CPU with no JIT backend both print a note and the
		   machine runs interpreted. The setters must be called before enable_jit. */
		void enable_jit(bool check_mode);
		void set_jit_cache_bytes(u_word bytes);      // --jit-cache-mb (default 64 MiB)
		void set_jit_threshold(u_word t);            // block hotness threshold (default 50)
		void jit_report(std::ostream& out);          // printed at exit when --jit ran
		u_word instruction_count();   // running guest-instruction count (for MIPS)

		/* Extract the zero-extended value of a named subregister from a port operand.
		   Host-backed device hooks call this to read the guest's written value as a plain
		   u_word (the CPU-side operand, per the shipped .q0 / sub-register selection). */
		u_word port_value_bits(reg_value const& value, subreg_enum value_subreg);

		/* Install the single active instruction-tick input source (the single stdin
		   consumer). null means no host input device drives stdin: the SYS console path
		   reads stdin on demand, the default. */
		void set_active_input(input_device* src);

		/* Interrupt controller seam (card maize-21). A device raises an IRQ by making a
		   vector pending through raise_irq; the flat controller coalesces multiple raises
		   to a single pending-vector latch. set_active_timer installs the instruction-tick
		   timer the run loop advances once per executed instruction.

		   Precondition: raise_irq's vector is an external-interrupt vector in [32, 255].
		   The synchronous-trap range 0..31 is not an IRQ source and must not be raised
		   through this seam. Called on the CPU thread today (see the definition for the
		   cross-thread RF-mirror constraint). */
		void raise_irq(u_byte vector);
		void set_active_timer(timer_device* timer);

		/* maize-140: idle-park hook for a blocking console read (fd 0). A blocking
		   read with no input available parks the CPU by clearing the run bit (bit_running
		   in RF) so the MIPS readout drops to idle and there is no busy-spin; the host
		   console clears it while it waits on the keyboard event, then restores it before
		   the syscall returns. This reuses the HALT/run-bit substrate (card maize-21) that
		   HALT itself parks with, rather than inventing a second idle mechanism; the wait
		   itself is on the console's own scancode condition (windowed) or a blocking host
		   stdin read (headless deterministic source), and the console breaks its wait on
		   window close via its own stopped_ latch (see text_console::stop). */
		void set_running(bool on);


	} // namespace cpu;

} // namespace maize
