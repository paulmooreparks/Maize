/* Host-backed standard device models (device-plugin API): console, keyboard, and the
   memory-backed framebuffer. Compiled into the `maize` target only; the headless model
   has no external dependency, and the optional SDL2 window backend sits behind
   MAIZE_DISPLAY. See devices.h for the pinout and the memory-backed framebuffer model. */

#include "maize.h"
#include "devices.h"

namespace maize {
	namespace devices {

		using cpu::reg_value;
		using cpu::subreg_enum;

		// ---- Port proxy (forwards the device hooks to the owner's role dispatch) ---------

		void device_port::on_port_write(reg_value const& value, subreg_enum value_subreg) {
			owner->port_write(role, value, value_subreg);
		}

		reg_value device_port::on_port_read(subreg_enum dst_subreg) {
			return owner->port_read(role, dst_subreg);
		}

		// ---- Console / terminal port device (0x00 / 0x01) --------------------------------

		console_device::console_device() = default;

		void console_device::attach() {
			data_port_.owner = this;
			data_port_.role = ROLE_DATA;
			status_port_.owner = this;
			status_port_.role = ROLE_STATUS;
			cpu::add_device(cpu::console_port_data, data_port_);
			cpu::add_device(cpu::console_port_status, status_port_);
		}

		void console_device::port_write(int role, reg_value const& value, subreg_enum value_subreg) {
			u_word bits = cpu::port_value_bits(value, value_subreg);
			if (role == ROLE_DATA) {
				unsigned char b = static_cast<unsigned char>(bits & 0xFF);
				maize::syscall::write(1, &b, 1);
			}
			/* STATUS is a read-only status word; a write is a defined no-op. */
		}

		reg_value console_device::port_read(int role, subreg_enum /*dst_subreg*/) {
			reg_value out;
			if (role == ROLE_DATA) {
				unsigned char b = 0;
				if (available_) {
					b = static_cast<unsigned char>(data_byte_);
					available_ = false;
				}
				else {
					/* On-demand read when the console is not the active injector. */
					u_word n = maize::syscall::read(0, &b, 1);
					if (n != 1) {
						b = 0;
					}
				}
				out.w0 = b;
			}
			else {
				u_word s = 0x2;   // bit1 output-ready (stdout is always writable)
				if (available_) {
					s |= 0x1;          // bit0 input-available
				}
				out.w0 = s;
			}
			return out;
		}

		void console_device::on_input_tick() {
			if (exhausted_ || available_) {
				return;
			}
			unsigned char b = 0;
			u_word n = maize::syscall::read(0, &b, 1);
			if (n != 1) {
				exhausted_ = true;   // EOF or error: stop pulling
				return;
			}
			data_byte_ = b;
			available_ = true;
			cpu::raise_irq(cpu::console_irq_vector);
		}

		// ---- Keyboard device (0x10 / 0x11) -----------------------------------------------

		keyboard_device::keyboard_device() = default;

		void keyboard_device::attach() {
			data_port_.owner = this;
			data_port_.role = ROLE_DATA;
			status_port_.owner = this;
			status_port_.role = ROLE_STATUS;
			cpu::add_device(cpu::keyboard_port_data, data_port_);
			cpu::add_device(cpu::keyboard_port_status, status_port_);
		}

		void keyboard_device::use_window_source() {
			window_source_ = true;
		}

		void keyboard_device::push_event(u_byte scancode) {
			std::lock_guard<std::mutex> lk(queue_mutex_);
			queue_.push_back(scancode);
			queue_size_.store(queue_.size(), std::memory_order_release);
		}

		void keyboard_device::port_write(int /*role*/, reg_value const& /*value*/, subreg_enum /*value_subreg*/) {
			/* The keyboard has no guest-writable data/status register in v1; a write is a
			   defined no-op (the guest reads scancodes and polls the status bit). */
		}

		reg_value keyboard_device::port_read(int role, subreg_enum /*dst_subreg*/) {
			reg_value out;
			if (role == ROLE_DATA) {
				unsigned char b = 0;
				if (available_) {
					b = static_cast<unsigned char>(scancode_);
					available_ = false;   // reading the data register clears key-available
				}
				out.w0 = b;
			}
			else {
				out.w0 = available_ ? 0x1 : 0x0;   // bit0 key-available
			}
			return out;
		}

		void keyboard_device::on_input_tick() {
			if (available_) {
				return;   // a scancode is already latched, waiting to be read
			}
			u_word code = 0;
			if (window_source_) {
				/* Lock-free early out: no scancode pending (the overwhelmingly common
				   case), so skip the per-instruction mutex acquisition entirely. */
				if (queue_size_.load(std::memory_order_acquire) == 0) {
					return;
				}
				std::lock_guard<std::mutex> lk(queue_mutex_);
				if (queue_.empty()) {
					return;
				}
				code = queue_.front();
				queue_.pop_front();
				queue_size_.store(queue_.size(), std::memory_order_release);
			}
			else {
				if (exhausted_) {
					return;
				}
				unsigned char b = 0;
				u_word n = maize::syscall::read(0, &b, 1);
				if (n != 1) {
					exhausted_ = true;   // EOF or error: no more injected scancodes
					return;
				}
				code = b;   // headless: the injected stdin byte IS the Set-1 scancode
			}
			scancode_ = code;
			available_ = true;
			cpu::raise_irq(cpu::keyboard_irq_vector);
		}

		// ---- Memory-backed framebuffer (0x50-0x55) ---------------------------------------

		framebuffer_device::framebuffer_device(u_hword width, u_hword height)
			: width_(width), height_(height),
			  pixel_count_(static_cast<u_word>(width) * static_cast<u_word>(height)),
			  frame_(static_cast<size_t>(pixel_count_), 0u) {
		}

		void framebuffer_device::attach() {
			width_port_.owner = this;   width_port_.role = ROLE_WIDTH;
			height_port_.owner = this;  height_port_.role = ROLE_HEIGHT;
			format_port_.owner = this;  format_port_.role = ROLE_FORMAT;
			base_port_.owner = this;    base_port_.role = ROLE_BASE;
			present_port_.owner = this; present_port_.role = ROLE_PRESENT;
			status_port_.owner = this;  status_port_.role = ROLE_STATUS;
			cpu::add_device(cpu::fb_port_width, width_port_);
			cpu::add_device(cpu::fb_port_height, height_port_);
			cpu::add_device(cpu::fb_port_format, format_port_);
			cpu::add_device(cpu::fb_port_base, base_port_);
			cpu::add_device(cpu::fb_port_present, present_port_);
			cpu::add_device(cpu::fb_port_status, status_port_);
		}

		/* Read the guest pixel buffer [base, base + width*height*4) into the host capture
		   frame and return whether the present was valid. Only guest memory is read (the
		   sparse array, every address defined), and the byte count is the host-fixed
		   resolution, so there is no host OOB read and no guest-controlled allocation. A
		   base of 0 (unset) or a base+size that would wrap the 64-bit address space is an
		   invalid present (defined, non-crashing). */
		bool framebuffer_device::present_frame() {
			u_word size = static_cast<u_word>(pixel_count_) * 4u;
			if (base_ == 0 || (base_ + size) < base_) {
				present_valid_ = false;
				return false;
			}
			std::vector<u_byte> bytes = cpu::mm.read(reg_value(base_), size);
			for (u_word i = 0; i < pixel_count_; ++i) {
				size_t o = static_cast<size_t>(i) * 4u;
				std::uint32_t px = static_cast<std::uint32_t>(bytes[o + 0])
					| (static_cast<std::uint32_t>(bytes[o + 1]) << 8)
					| (static_cast<std::uint32_t>(bytes[o + 2]) << 16)
					| (static_cast<std::uint32_t>(bytes[o + 3]) << 24);
				frame_[i] = px;
			}
			present_valid_ = true;
			return true;
		}

		void framebuffer_device::port_write(int role, reg_value const& value, subreg_enum value_subreg) {
			u_word bits = cpu::port_value_bits(value, value_subreg);
			switch (role) {
				case ROLE_BASE:
					base_ = bits;
					break;
				case ROLE_PRESENT:
					if (present_frame() && (control_ & 0x2)) {
						/* vsync-IRQ is guest-opt-in (bit1); generation is disabled by
						   default, so the deterministic headless suite never fires it. */
						status_ |= 0x1;
						cpu::raise_irq(cpu::fb_irq_vector);
					}
					break;
				case ROLE_STATUS:
					control_ = (bits & 0x2);                       // vsync-IRQ-enable
					status_ &= ~static_cast<u_word>(0x1);     // ack: clear vsync-pending
					break;
				default:
					/* WIDTH / HEIGHT / FORMAT are read-only host config; a write is a
					   defined no-op. */
					break;
			}
		}

		reg_value framebuffer_device::port_read(int role, subreg_enum /*dst_subreg*/) {
			reg_value out;
			switch (role) {
				case ROLE_WIDTH:   out.w0 = width_; break;
				case ROLE_HEIGHT:  out.w0 = height_; break;
				case ROLE_FORMAT:  out.w0 = cpu::fb_format_xrgb8888; break;
				case ROLE_BASE:    out.w0 = base_; break;
				case ROLE_PRESENT: out.w0 = present_valid_ ? 0x1 : 0x0; break;
				case ROLE_STATUS:  out.w0 = status_ & 0x1; break;
				default:           out.w0 = 0; break;
			}
			return out;
		}

	} // namespace devices
} // namespace maize

#ifdef MAIZE_DISPLAY

#include <SDL.h>
#include <atomic>
#include <chrono>
#include <thread>

namespace maize {
	namespace devices {
		namespace display {

			/* A common subset of SDL scancodes mapped to Set-1 (XT) make codes. The break
			   code is the make code with bit 7 set. Unmapped keys are ignored. This is the
			   windowed input path only (opt-in); the headless suite injects scancodes
			   directly through stdin and never reaches this map. */
			static u_byte map_scancode(SDL_Scancode sc) {
				switch (sc) {
					case SDL_SCANCODE_ESCAPE: return 0x01;
					case SDL_SCANCODE_1: return 0x02;
					case SDL_SCANCODE_2: return 0x03;
					case SDL_SCANCODE_3: return 0x04;
					case SDL_SCANCODE_4: return 0x05;
					case SDL_SCANCODE_5: return 0x06;
					case SDL_SCANCODE_6: return 0x07;
					case SDL_SCANCODE_7: return 0x08;
					case SDL_SCANCODE_8: return 0x09;
					case SDL_SCANCODE_9: return 0x0A;
					case SDL_SCANCODE_0: return 0x0B;
					case SDL_SCANCODE_Q: return 0x10;
					case SDL_SCANCODE_W: return 0x11;
					case SDL_SCANCODE_E: return 0x12;
					case SDL_SCANCODE_R: return 0x13;
					case SDL_SCANCODE_T: return 0x14;
					case SDL_SCANCODE_Y: return 0x15;
					case SDL_SCANCODE_U: return 0x16;
					case SDL_SCANCODE_I: return 0x17;
					case SDL_SCANCODE_O: return 0x18;
					case SDL_SCANCODE_P: return 0x19;
					case SDL_SCANCODE_A: return 0x1E;
					case SDL_SCANCODE_S: return 0x1F;
					case SDL_SCANCODE_D: return 0x20;
					case SDL_SCANCODE_F: return 0x21;
					case SDL_SCANCODE_G: return 0x22;
					case SDL_SCANCODE_H: return 0x23;
					case SDL_SCANCODE_J: return 0x24;
					case SDL_SCANCODE_K: return 0x25;
					case SDL_SCANCODE_L: return 0x26;
					case SDL_SCANCODE_Z: return 0x2C;
					case SDL_SCANCODE_X: return 0x2D;
					case SDL_SCANCODE_C: return 0x2E;
					case SDL_SCANCODE_V: return 0x2F;
					case SDL_SCANCODE_B: return 0x30;
					case SDL_SCANCODE_N: return 0x31;
					case SDL_SCANCODE_M: return 0x32;
					case SDL_SCANCODE_COMMA: return 0x33;
					case SDL_SCANCODE_PERIOD: return 0x34;
					case SDL_SCANCODE_RETURN: return 0x1C;
					case SDL_SCANCODE_LCTRL: return 0x1D;
					case SDL_SCANCODE_LSHIFT: return 0x2A;
					case SDL_SCANCODE_LALT: return 0x38;
					case SDL_SCANCODE_SPACE: return 0x39;
					case SDL_SCANCODE_UP: return 0x48;
					case SDL_SCANCODE_LEFT: return 0x4B;
					case SDL_SCANCODE_RIGHT: return 0x4D;
					case SDL_SCANCODE_DOWN: return 0x50;
					default: return 0;
				}
			}

			void run(framebuffer_device& fb, keyboard_device& kbd, unsigned scale) {
				kbd.use_window_source();

				if (SDL_Init(SDL_INIT_VIDEO) != 0) {
					/* No display available: fall back to headless execution. */
					cpu::run();
					return;
				}

				int w = static_cast<int>(fb.width());
				int h = static_cast<int>(fb.height());
				if (scale < 1) { scale = 1; }

				/* The guest renders at the native framebuffer resolution (w x h); the window
				   opens at that size magnified by `scale` and is resizable. A logical render
				   size of w x h lets SDL scale the presented frame to any window size while
				   preserving aspect ratio (letterboxing as needed), so drag-resize and
				   maximize stay correct. */
				SDL_Window* win = SDL_CreateWindow("Maize",
					SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
					w * static_cast<int>(scale), h * static_cast<int>(scale),
					SDL_WINDOW_RESIZABLE);
				SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
				SDL_RenderSetLogicalSize(ren, w, h);
				SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
					SDL_TEXTUREACCESS_STREAMING, w, h);

				std::thread guest([]() { cpu::run(); });

				bool running = true;
				while (running) {
					SDL_Event e;
					while (SDL_PollEvent(&e)) {
						if (e.type == SDL_QUIT) {
							running = false;
						}
						else if (e.type == SDL_KEYDOWN && e.key.repeat == 0) {
							u_byte sc = map_scancode(e.key.keysym.scancode);
							if (sc) {
								kbd.push_event(sc);
							}
						}
						else if (e.type == SDL_KEYUP) {
							u_byte sc = map_scancode(e.key.keysym.scancode);
							if (sc) {
								kbd.push_event(static_cast<u_byte>(sc | 0x80));   // break code
							}
						}
					}

					const std::vector<std::uint32_t>& frame = fb.frame();
					if (!frame.empty()) {
						SDL_UpdateTexture(tex, nullptr, frame.data(),
							w * static_cast<int>(sizeof(std::uint32_t)));
						SDL_RenderClear(ren);
						SDL_RenderCopy(ren, tex, nullptr, nullptr);
						SDL_RenderPresent(ren);
					}

					std::this_thread::sleep_for(std::chrono::milliseconds(16));
				}

				cpu::power_off();
				if (guest.joinable()) {
					guest.join();
				}

				SDL_DestroyTexture(tex);
				SDL_DestroyRenderer(ren);
				SDL_DestroyWindow(win);
				SDL_Quit();
			}

		} // namespace display
	} // namespace devices
} // namespace maize

#endif // MAIZE_DISPLAY
