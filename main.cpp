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


		enum reg_enum {
			a_reg = 0x00,
			b_reg = 0x01,
			c_reg = 0x02,
			d_reg = 0x03,
			e_reg = 0x04,
			g_reg = 0x05,
			h_reg = 0x06,
			j_reg = 0x07,
			k_reg = 0x08,
			l_reg = 0x09,
			m_reg = 0x0A,
			z_reg = 0x0B,
			fl_reg = 0x0C,
			in_reg = 0x0D,
			pc_reg = 0x0E,
			sp_reg = 0x0F
		};

		enum subreg_enum {
			b0_subreg = 0x00,
			b1_subreg = 0x01,
			b2_subreg = 0x02,
			b3_subreg = 0x03,
			b4_subreg = 0x04,
			b5_subreg = 0x05,
			b6_subreg = 0x06,
			b7_subreg = 0x07,
			q0_subreg = 0x08,
			q1_subreg = 0x09,
			q2_subreg = 0x0A,
			q3_subreg = 0x0B,
			h0_subreg = 0x0C,
			h1_subreg = 0x0D,
			w0_subreg = 0x0E
		};

		enum subreg_mask : uint64_t {
			b0_mask = 0b0000000000000000000000000000000000000000000000000000000011111111,
			b1_mask = 0b0000000000000000000000000000000000000000000000001111111100000000,
			b2_mask = 0b0000000000000000000000000000000000000000111111110000000000000000,
			b3_mask = 0b0000000000000000000000000000000011111111000000000000000000000000,
			b4_mask = 0b0000000000000000000000001111111100000000000000000000000000000000,
			b5_mask = 0b0000000000000000111111110000000000000000000000000000000000000000,
			b6_mask = 0b0000000011111111000000000000000000000000000000000000000000000000,
			b7_mask = 0b1111111100000000000000000000000000000000000000000000000000000000,
			q0_mask = 0b0000000000000000000000000000000000000000000000001111111111111111,
			q1_mask = 0b0000000000000000000000000000000011111111111111110000000000000000,
			q2_mask = 0b0000000000000000111111111111111100000000000000000000000000000000,
			q3_mask = 0b1111111111111111000000000000000000000000000000000000000000000000,
			h0_mask = 0b0000000000000000000000000000000011111111111111111111111111111111,
			h1_mask = 0b1111111111111111111111111111111100000000000000000000000000000000,
			w0_mask = 0b1111111111111111111111111111111111111111111111111111111111111111,
		};

		/*
		byte SrcImmSizeFlag = (byte)(Decoder.RegData.B1 & OpFlag_ImmSize);
		int src_imm_size = 1 << src_imm_size_flag;
		byte src_reg_flag = (byte)(Decoder.RegData.B1 & OpFlag_Reg);
		byte src_subreg_flag = (byte)(Decoder.RegData.B1 & OpFlag_SubReg);
		byte DestImmSizeFlag = (byte)(Decoder.RegData.B2 & OpFlag_ImmSize);
		int DestImmSize = 1 << dest_imm_size_flag;
		byte DestRegisterFlag = (byte)(Decoder.RegData.B2 & OpFlag_Reg);
		byte DestSubRegisterFlag = (byte)(Decoder.RegData.B2 & OpFlag_SubReg);
		*/

		const uint8_t OpcodeFlag = 0b11000000;
		const uint8_t OpcodeFlag_SrcImm = 0b01000000;
		const uint8_t OpcodeFlag_SrcAddr = 0b10000000;

		const uint8_t OpFlag_Reg  = 0b11110000;
		const uint8_t OpFlag_RegA = 0b00000000;
		const uint8_t OpFlag_RegB = 0b00010000;
		const uint8_t OpFlag_RegC = 0b00100000;
		const uint8_t OpFlag_RegD = 0b00110000;
		const uint8_t OpFlag_RegE = 0b01000000;
		const uint8_t OpFlag_RegG = 0b01010000;
		const uint8_t OpFlag_RegH = 0b01100000;
		const uint8_t OpFlag_RegJ = 0b01110000;
		const uint8_t OpFlag_RegK = 0b10000000;
		const uint8_t OpFlag_RegL = 0b10010000;
		const uint8_t OpFlag_RegM = 0b10100000;
		const uint8_t OpFlag_RegZ = 0b10110000;
		const uint8_t OpFlag_RegF = 0b11000000;
		const uint8_t OpFlag_RegI = 0b11010000;
		const uint8_t OpFlag_RegP = 0b11100000;
		const uint8_t OpFlag_RegS = 0b11110000;

		const uint8_t OpFlag_RegSP = 0b11111100; // S.H0 = stack pointer
		const uint8_t OpFlag_RegBP = 0b11111101; // S.H1 = base pointer
		const uint8_t OpFlag_RegPC = 0b11101100; // P.H0 = program counter
		const uint8_t OpFlag_RegCS = 0b11101101; // P.H1 = program segment
		const uint8_t OpFlag_RegFL = 0b11001100; // F.H0 = flags

		const uint8_t OpFlag_SubReg = 0b00001111;
		const uint8_t OpFlag_RegB0 = 0b00000000;
		const uint8_t OpFlag_RegB1 = 0b00000001;
		const uint8_t OpFlag_RegB2 = 0b00000010;
		const uint8_t OpFlag_RegB3 = 0b00000011;
		const uint8_t OpFlag_RegB4 = 0b00000100;
		const uint8_t OpFlag_RegB5 = 0b00000101;
		const uint8_t OpFlag_RegB6 = 0b00000110;
		const uint8_t OpFlag_RegB7 = 0b00000111;
		const uint8_t OpFlag_RegQ0 = 0b00001000;
		const uint8_t OpFlag_RegQ1 = 0b00001001;
		const uint8_t OpFlag_RegQ2 = 0b00001010;
		const uint8_t OpFlag_RegQ3 = 0b00001011;
		const uint8_t OpFlag_RegH0 = 0b00001100;
		const uint8_t OpFlag_RegH1 = 0b00001101;
		const uint8_t OpFlag_RegW0 = 0b00001110;

		const uint8_t OpFlag_ImmSize = 0b00000111;
		const uint8_t OpFlag_Imm08b = 0b00000000;
		const uint8_t OpFlag_Imm16b = 0b00000001;
		const uint8_t OpFlag_Imm32b = 0b00000010;
		const uint8_t OpFlag_Imm64b = 0b00000011;

		const uint8_t OpFlag_ImmRes01 = 0b01000000;
		const uint8_t OpFlag_ImmRes02 = 0b01010000;
		const uint8_t OpFlag_ImmRes03 = 0b01100000;
		const uint8_t OpFlag_ImmRes04 = 0b01110000;

		uint8_t offset_map[] = {
			0,  // b0_subreg
			8,  // b1
			16, // b2
			24, // b3
			32, // b4
			40, // b5
			48, // b6
			56, // b7
			0,  // q0_subreg
			16, // q1
			32, // q2
			48, // q3
			0,  // h0_subreg
			32, // h1
			0   // w0_subreg
		};

		const subreg_mask imm_size_subreg_mask_map[] = {
			b0_mask,
			q0_mask,
			h0_mask,
			w0_mask
		};

		const subreg_enum imm_size_subreg_map[] = {
			b0_subreg,
			q0_subreg,
			h0_subreg,
			w0_subreg
		};

		subreg_mask subreg_mask_map[] = {
			b0_mask,
			b1_mask,
			b2_mask,
			b3_mask,
			b4_mask,
			b5_mask,
			b6_mask,
			b7_mask,
			q0_mask,
			q1_mask,
			q2_mask,
			q3_mask,
			h0_mask,
			h1_mask,
			w0_mask
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

		/*
		byte AluOpSizeMap[] = {
			Alu.OpSize_Byte,
			Alu.OpSize_Byte,
			Alu.OpSize_Byte,
			Alu.OpSize_Byte,
			Alu.OpSize_Byte,
			Alu.OpSize_Byte,
			Alu.OpSize_Byte,
			Alu.OpSize_Byte,
			Alu.OpSize_QuarterWord,
			Alu.OpSize_QuarterWord,
			Alu.OpSize_QuarterWord,
			Alu.OpSize_QuarterWord,
			Alu.OpSize_HalfWord,
			Alu.OpSize_HalfWord,
			Alu.OpSize_Word
		};
		*/

		struct reg;
		static const size_t max_increment_count = 32;
		size_t increment_count{0};
		std::pair<reg*, int64_t> increment_array[max_increment_count];

		struct reg_op_info {
			reg_op_info() = default;

			reg* op_reg{nullptr};
			subreg_mask mask{0};
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
				// return ++w.w0;
				increment();
				return w0;
			}

			uint64_t operator++(int) {
				// uint64_t val = w.w0;
				// operator++();
				// return val;
				increment();
				return w0;
			}

			uint64_t operator--() {
				// return --w.w0;
				decrement();
				return w0;
			}

			uint64_t operator--(int) {
				// uint64_t val = w.w0;
				// operator--();
				// return val;
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
			subreg_mask enable_subreg_mask{0};
			uint8_t enable_offset{0};

			static const size_t max_bus_set = 16;
			size_t set_count{0};
			reg_op_info bus_set_array[max_bus_set];

		public:
			void enable_reg(reg* pen_reg, subreg_enum subreg) {
				enable_reg(*pen_reg, subreg);
			}

			void enable_reg(reg& en_reg, subreg_enum subreg) {
				enable_subreg_mask = subreg_mask_map[subreg];
				enable_offset = offset_map[subreg];
				enabled_reg = &en_reg;
			}

			void set_reg(reg* pset_reg, subreg_enum subreg) {
				set_reg(*pset_reg, subreg);
			}

			void set_reg(reg& set_reg, subreg_enum subreg) {
				if (set_count < max_bus_set) {
					bus_set_array[set_count].op_reg = &set_reg;
					bus_set_array[set_count].mask = subreg_mask_map[subreg];
					bus_set_array[set_count].offset = offset_map[subreg];
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

			uint16_t read_quarter_word(uint64_t address) {
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

			uint32_t read_half_word(uint64_t address) {
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

			subreg_mask mask{0};
			bus* pload_bus {nullptr};
			bus* pstore_bus {nullptr};

			void set_address(bus& source_bus) {
				// MemoryModule.AddressRegister.SetFromAddressBus(SubRegister.H0);
				source_bus.set_reg(address_reg, h0_subreg);
			}

			void load(bus& load_bus) {
				// MemoryModule.DataRegister.EnableToDataBus(SubRegister.W0);
				pload_bus = &load_bus;
			}

			void on_load() {
				if (pload_bus) {
					pload_bus->enable_reg(data_reg, w0_subreg);
					data_reg.w0 = read_word(address_reg.w0);
					pload_bus = nullptr;
				}
			}

			void store(bus& store_bus, subreg_enum subreg) {
				// MemoryModule.DataRegister.SetFromDataBus((SubRegister)src_subreg_flag);
				pstore_bus = &store_bus;
				mask = subreg_mask_map[subreg];
				store_bus.set_reg(data_reg, subreg);
			}

			void on_store() {
				if (pstore_bus) {
					pstore_bus = nullptr;

					switch (mask) {
						case b0_mask:
							write_byte(address_reg.w0, data_reg.b0);
							break;

						case b1_mask:
							write_byte(address_reg.w0, data_reg.b1);
							break;

						case b2_mask:
							write_byte(address_reg.w0, data_reg.b2);
							break;

						case b3_mask:
							write_byte(address_reg.w0, data_reg.b3);
							break;

						case b4_mask:
							write_byte(address_reg.w0, data_reg.b4);
							break;

						case b5_mask:
							write_byte(address_reg.w0, data_reg.b5);
							break;

						case b6_mask:
							write_byte(address_reg.w0, data_reg.b6);
							break;

						case b7_mask:
							write_byte(address_reg.w0, data_reg.b7);
							break;

						case q0_mask:
							write_quarter_word(address_reg.w0, data_reg.q0);
							break;

						case q1_mask:
							write_quarter_word(address_reg.w0, data_reg.q1);
							break;

						case q2_mask:
							write_quarter_word(address_reg.w0, data_reg.q2);
							break;

						case q3_mask:
							write_quarter_word(address_reg.w0, data_reg.q3);
							break;

						case h0_mask:
							write_half_word(address_reg.w0, data_reg.h0);
							break;

						case h1_mask:
							write_half_word(address_reg.w0, data_reg.h1);
							break;

						case w0_mask:
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
						memory_map[address_base] = new uint8_t[0x100];
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

		std::array<reg*, 16> reg_map {
				&a, &b, &c, &d, &e, &g, &h, &j, &k, &l, &m, &z, &fl, &in, &pc, &sp
		};

		memory_module mm;

		uint8_t src_imm_size_flag() {
			return in.b1 & OpFlag_ImmSize;
		}

		int src_imm_size() {
			return 1 << src_imm_size_flag();
		}

		uint8_t src_reg_flag() {
			return in.b1 & OpFlag_Reg;
		}

		uint8_t src_reg_index() {
			return src_reg_flag() >> 4;
		}

		subreg_enum src_subreg_flag()  {
			return static_cast<subreg_enum>(in.b1 & OpFlag_SubReg);
		}

		uint8_t dest_imm_size_flag() {
			return in.b2 & OpFlag_ImmSize;
		}

		uint8_t dest_imm_size() {
			return 1 << dest_imm_size_flag();
		}

		uint8_t dest_reg_flag() {
			return in.b2 & OpFlag_Reg;
		}

		uint8_t dest_reg_index() {
			return dest_reg_flag() >> 4;
		}

		subreg_enum dest_subreg_flag() {
			return static_cast<subreg_enum>(in.b2 & OpFlag_SubReg);
		}

		namespace instr {
			const opcode halt               {0x00};
			const opcode ld_regVal_reg      {0x01};
			const opcode st_regVal_regAddr  {0x02};
			const opcode add_regVal_reg     {0x03};
			const opcode ld_immVal_reg      {0x41};
		}

		namespace micro_instr {
			const int road {0x100}; // Read opcode and decode
		}

		bool running = true;
		int cycle{0};
		int step{0};

		enum run_states {
			run_state_decode,
			run_state_execute
		};

		run_states run_state = run_state_decode;

		void ins_complete() {
			run_state = run_state_decode;
			cycle = 0;
			step = 0;
			in = micro_instr::road; // Read and dispatch
		}

		void ins_substep_complete() {
			run_state = run_state_decode;
		}

		void ins_continue() {
			run_state = run_state_execute;
			step = 0;
		}

		void init() {

		}

		void run() {
			reg* psrc_reg {nullptr};
			reg* pdest_reg {nullptr};

			while (running) {
				switch (run_state) {
					case run_state_decode:
						switch (cycle) {
							case 0:
								// PC.EnableToAddressBus(SubRegister.H0);
								address_bus.enable_reg(pc, h0_subreg);
								// MemoryModule.AddressRegister.SetFromAddressBus(SubRegister.H0);
								mm.set_address(address_bus);
								break;

							case 1:
								// MemoryModule.DataRegister.EnableToDataBus(SubRegister.W0);
								mm.load(data_bus);
								// Decoder.SetFromDataBus(SubRegister.W0);
								data_bus.set_reg(in, w0_subreg);
								++pc;
								ins_continue();
								break;
						}

						++cycle;
						break;

					case run_state_execute:
						++cycle;

						switch (in.b0) {
							case instr::halt: {
								switch (step) {
									case 0:
										running = false;
										cycle = 0;
										break;
								}

								break;
							}

							case instr::ld_regVal_reg: {
								switch (step) {
									case 0:
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

								break;
							}

							case instr::ld_immVal_reg: {
								switch (step) {
									case 0: {
										address_bus.enable_reg(pc, h0_subreg);
										// P.Increment(2);
										pc.increment(2);
										// Decoder.ReadAndEnableImmediate(src_imm_size);
										mm.set_address(address_bus);
										break;
									}

									case 1: {
										uint8_t size = src_imm_size();
										pc.increment(size);
										mm.load(data_bus);
										break;
									}

									case 2: {
										// DestReg = Decoder.RegisterMap[DestRegisterFlag >> 4];
										size_t reg_index = dest_reg_flag() >> 4;
										pdest_reg = reg_map[reg_index];
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
									case 0:
										// P.Increment(2);
										pc.increment(2);
										// SrcReg = Decoder.RegisterMap[src_reg_flag >> 4];
										// DestReg = Decoder.RegisterMap[DestRegisterFlag >> 4];
										// DestReg.EnableToAddressBus((SubRegister)DestSubRegisterFlag);
										// MemoryModule.AddressRegister.SetFromAddressBus(SubRegister.W0);
										break;

									case 1:
										// SrcReg.EnableToDataBus((SubRegister)src_subreg_flag);
										// MemoryModule.DataRegister.SetFromDataBus((SubRegister)src_subreg_flag);
										ins_complete();
										break;
								}

								break;
							}

							case instr::add_regVal_reg: {
								switch (step) {
									case 0:
										ins_complete();
										break;
								}

								break;
							}
						}

						++step;
						break;
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
	cpu::instr::ld_immVal_reg, 0x01, 0x11, 0x88,
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

