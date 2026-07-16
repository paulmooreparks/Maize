/* Host-backed standard device models (device-plugin API): console, keyboard, and the
   memory-backed framebuffer. Compiled into the `maize` target only; the headless model
   has no external dependency, and the optional SDL2 window backend sits behind
   MAIZE_DISPLAY. See devices.h for the pinout and the memory-backed framebuffer model. */

#include "maize.h"
#include "devices.h"
#include "font8x16.h"
#include "perf.h"
#include <cstring>

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

		void keyboard_device::pump_latch() {
			/* Caller holds queue_mutex_. Move one queued scancode into the latch and raise the
			   keyboard IRQ, if a key is pending and the latch is free. Because this runs from
			   whichever thread supplies the key, a CPU parked in HALT is woken by an SDL-thread
			   key press (raise_irq notifies the run()-loop park), not only by on_input_tick. */
			if (!available_.load(std::memory_order_relaxed) && !queue_.empty()) {
				scancode_ = queue_.front();
				queue_.pop_front();
				queue_size_.store(queue_.size(), std::memory_order_release);
				available_.store(true, std::memory_order_release);
				cpu::raise_irq(cpu::keyboard_irq_vector);
			}
		}

		void keyboard_device::push_event(u_byte scancode) {
			std::lock_guard<std::mutex> lk(queue_mutex_);
			queue_.push_back(scancode);
			queue_size_.store(queue_.size(), std::memory_order_release);
			pump_latch();   // latch + raise IRQ now (wakes a HALT-parked CPU)
		}

		void keyboard_device::port_write(int /*role*/, reg_value const& /*value*/, subreg_enum /*value_subreg*/) {
			/* The keyboard has no guest-writable data/status register in v1; a write is a
			   defined no-op (the guest reads scancodes and polls the status bit). */
		}

		reg_value keyboard_device::port_read(int role, subreg_enum /*dst_subreg*/) {
			reg_value out;
			if (role == ROLE_DATA) {
				unsigned char b = 0;
				if (window_source_) {
					/* Consume the latch and immediately refill it from the queue (raising the
					   IRQ for the next key), all under the lock so scancode_ stays consistent
					   with the SDL thread's push_event. */
					std::lock_guard<std::mutex> lk(queue_mutex_);
					if (available_.load(std::memory_order_relaxed)) {
						b = static_cast<unsigned char>(scancode_);
						available_.store(false, std::memory_order_release);
						pump_latch();
					}
				}
				else if (available_.load(std::memory_order_relaxed)) {
					b = static_cast<unsigned char>(scancode_);
					available_.store(false, std::memory_order_release);   // reading data clears key-available
				}
				out.w0 = b;
			}
			else {
				out.w0 = available_.load(std::memory_order_acquire) ? 0x1 : 0x0;   // bit0 key-available
			}
			return out;
		}

		void keyboard_device::on_input_tick() {
			if (available_.load(std::memory_order_acquire)) {
				return;   // a scancode is already latched, waiting to be read
			}
			if (window_source_) {
				/* Backstop only. Windowed input is driven by push_event (latch + IRQ on the SDL
				   thread) and port_read (drain the queue on consume), so active_input is not set
				   for the window source and this path is normally never reached. Kept correct for
				   safety: drain one queued key if one slipped in while the latch was full. */
				if (queue_size_.load(std::memory_order_acquire) == 0) {
					return;
				}
				std::lock_guard<std::mutex> lk(queue_mutex_);
				pump_latch();
				return;
			}
			/* Headless deterministic injection: read one stdin byte as the Set-1 scancode. */
			if (exhausted_) {
				return;
			}
			unsigned char b = 0;
			u_word n = maize::syscall::read(0, &b, 1);
			if (n != 1) {
				exhausted_ = true;   // EOF or error: no more injected scancodes
				return;
			}
			scancode_ = b;
			available_.store(true, std::memory_order_release);
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
			/* The guest framebuffer is XRGB8888 little-endian, byte-identical to frame_'s
			   uint32 layout on a little-endian host, so one bulk copy into frame_ needs no
			   intermediate vector and no per-pixel repack (was: a 256 KB alloc + byte-loop
			   read + a second repack pass, every present). */
			cpu::mm.read_into(base_, reinterpret_cast<u_byte*>(frame_.data()),
				static_cast<size_t>(size));
			present_valid_ = true;
			present_count_.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		void framebuffer_device::port_write(int role, reg_value const& value, subreg_enum value_subreg) {
			u_word bits = cpu::port_value_bits(value, value_subreg);
			switch (role) {
				case ROLE_BASE:
					base_ = bits;
					/* maize-140: a nonzero base is the explicit raw-framebuffer takeover
					   signal (D3). Programming the framebuffer's pixel-buffer address is a
					   deliberate guest action a graphics program (DOOM's DG_Init) takes and
					   a text-console program never does, so it latches graphics mode for the
					   display's text/graphics arbitration. */
					if (bits != 0) {
						claimed_.store(true, std::memory_order_release);
					}
					break;
				case ROLE_PRESENT:
					/* Pure frame copy. The vsync IRQ is driven by the display refresh
					   (on_display_refresh), not by the guest's present, so a guest parked in
					   HALT still wakes at the refresh rate. */
					present_frame();
					break;
				case ROLE_STATUS:
					/* bit1 = vsync-IRQ-enable (guest opt-in; default off, so the deterministic
					   headless suite, which has no display thread, never fires it). Writing the
					   register also acks: clear vsync-pending. */
					control_.store(bits & 0x2, std::memory_order_release);
					status_.fetch_and(~static_cast<u_word>(0x1), std::memory_order_acq_rel);
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
				case ROLE_STATUS:  out.w0 = status_.load(std::memory_order_acquire) & 0x1; break;
				default:           out.w0 = 0; break;
			}
			return out;
		}

		void framebuffer_device::on_display_refresh() {
			if (control_.load(std::memory_order_acquire) & 0x2) {
				status_.fetch_or(0x1, std::memory_order_acq_rel);   // vsync-pending
				cpu::raise_irq(cpu::fb_irq_vector);
			}
		}

		// ---- Host text console (maize-140) -----------------------------------------------

		/* 8 basic ANSI colors as XRGB8888, identical to term_core's term_palette so the
		   host console renders the SGR 30-47 colors the same way the maize-121 engine does. */
		static const std::uint32_t con_palette[8] = {
			0x000000u, 0xAA0000u, 0x00AA00u, 0xAA5500u,
			0x0000AAu, 0xAA00AAu, 0x00AAAAu, 0xAAAAAAu
		};

		/* Set-1 (XT) make-code -> ASCII (US layout), the exact tables term_core.h uses.
		   Index by make code 0..0x3F; 0 = no glyph. The upper table is selected while shift
		   is held. Enter (0x1C), Backspace (0x0E), Tab (0x0F) and the nav keys are handled
		   specially in keymap(), so their slots here are unused for those codes. */
		static const unsigned char con_sc_lower[0x40] = {
			/* 0x00 */ 0,    0,   '1', '2', '3', '4', '5', '6',
			/* 0x08 */ '7',  '8', '9', '0', '-', '=', 0x08, 0x09,
			/* 0x10 */ 'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',
			/* 0x18 */ 'o',  'p', '[', ']', 0x0D, 0,  'a', 's',
			/* 0x20 */ 'd',  'f', 'g', 'h', 'j', 'k', 'l', ';',
			/* 0x28 */ '\'', '`', 0,   '\\','z', 'x', 'c', 'v',
			/* 0x30 */ 'b',  'n', 'm', ',', '.', '/', 0,   '*',
			/* 0x38 */ 0,    ' ', 0,   0,   0,   0,   0,   0
		};
		static const unsigned char con_sc_upper[0x40] = {
			/* 0x00 */ 0,    0,   '!', '@', '#', '$', '%', '^',
			/* 0x08 */ '&',  '*', '(', ')', '_', '+', 0x08, 0x09,
			/* 0x10 */ 'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',
			/* 0x18 */ 'O',  'P', '{', '}', 0x0D, 0,  'A', 'S',
			/* 0x20 */ 'D',  'F', 'G', 'H', 'J', 'K', 'L', ':',
			/* 0x28 */ '"',  '~', 0,   '|', 'Z', 'X', 'C', 'V',
			/* 0x30 */ 'B',  'N', 'M', '<', '>', '?', 0,   '*',
			/* 0x38 */ 0,    ' ', 0,   0,   0,   0,   0,   0
		};

		text_console::text_console(unsigned width_px, unsigned height_px)
			: width_px_(width_px), height_px_(height_px),
			  cols_(static_cast<int>(width_px / FONT_W)),
			  rows_(static_cast<int>(height_px / FONT_H)),
			  pixels_(static_cast<size_t>(width_px) * height_px, 0u) {
			if (cols_ < 1) { cols_ = 1; }
			if (rows_ < 1) { rows_ = 1; }
			glyph_.assign(static_cast<size_t>(cols_) * rows_, 0x20);
			attr_.assign(static_cast<size_t>(cols_) * rows_, static_cast<unsigned char>(cur_attr()));
			reset_termios_defaults();
			for (int r = 0; r < rows_; ++r) {
				for (int c = 0; c < cols_; ++c) { render_cell(r, c); }
			}
		}

		void text_console::reset_termios_defaults() {
			/* Cooked TTY defaults (ICANON+ECHO+ISIG), a real glass-TTY line discipline. */
			iflag_ = console::TERMIOS_ICRNL;
			oflag_ = console::TERMIOS_OPOST | console::TERMIOS_ONLCR;
			cflag_ = 0;
			lflag_ = console::TERMIOS_ICANON | console::TERMIOS_ECHO | console::TERMIOS_ISIG;
			std::memset(cc_, 0, sizeof(cc_));
			cc_[console::TERMIOS_VERASE] = 0x7F;   // Backspace erases (DEL)
			cc_[console::TERMIOS_VEOF] = 0x04;     // Ctrl-D
			cc_[console::TERMIOS_VMIN] = 1;        // raw read returns after >=1 byte
			cc_[console::TERMIOS_VTIME] = 0;
		}

		void text_console::render_cell(int row, int col) {
			int ch = glyph_[static_cast<size_t>(row) * cols_ + col];
			int a = attr_[static_cast<size_t>(row) * cols_ + col];
			std::uint32_t fg = con_palette[a & 7];
			std::uint32_t bg = con_palette[(a >> 4) & 7];
			if (ch < FONT_FIRST || ch > FONT_LAST) { ch = 0x20; }
			int idx = ch - FONT_FIRST;
			for (int gy = 0; gy < FONT_H; ++gy) {
				int bits = font8x16[idx][gy];
				size_t base = (static_cast<size_t>(row) * FONT_H + gy) * width_px_ + static_cast<size_t>(col) * FONT_W;
				for (int gx = 0; gx < FONT_W; ++gx) {
					pixels_[base + gx] = ((bits >> gx) & 1) ? fg : bg;
				}
			}
		}

		void text_console::clear_cell(int r, int c) {
			glyph_[static_cast<size_t>(r) * cols_ + c] = 0x20;
			attr_[static_cast<size_t>(r) * cols_ + c] = static_cast<unsigned char>(cur_attr());
			render_cell(r, c);
		}

		void text_console::scroll_up() {
			for (int r = 0; r < rows_ - 1; ++r) {
				for (int c = 0; c < cols_; ++c) {
					glyph_[static_cast<size_t>(r) * cols_ + c] = glyph_[static_cast<size_t>(r + 1) * cols_ + c];
					attr_[static_cast<size_t>(r) * cols_ + c] = attr_[static_cast<size_t>(r + 1) * cols_ + c];
				}
			}
			for (int c = 0; c < cols_; ++c) {
				glyph_[static_cast<size_t>(rows_ - 1) * cols_ + c] = 0x20;
				attr_[static_cast<size_t>(rows_ - 1) * cols_ + c] = static_cast<unsigned char>(cur_attr());
			}
			for (int r = 0; r < rows_; ++r) {
				for (int c = 0; c < cols_; ++c) { render_cell(r, c); }
			}
		}

		void text_console::newline() {
			wrap_pending_ = false;
			col_ = 0;
			row_++;
			if (row_ >= rows_) { row_ = rows_ - 1; scroll_up(); }
		}

		void text_console::put_glyph(int ch) {
			/* Honor a deferred right-margin wrap from the previous glyph first: only now
			   (a real glyph is arriving) does the cursor drop to the next line. */
			if (wrap_pending_) {
				col_ = 0;
				row_++;
				if (row_ >= rows_) { row_ = rows_ - 1; scroll_up(); }
				wrap_pending_ = false;
			}
			glyph_[static_cast<size_t>(row_) * cols_ + col_] = static_cast<unsigned char>(ch);
			attr_[static_cast<size_t>(row_) * cols_ + col_] = static_cast<unsigned char>(cur_attr());
			render_cell(row_, col_);
			if (col_ >= cols_ - 1) {
				wrap_pending_ = true;   // last column: arm the wrap, do NOT advance yet
			} else {
				col_++;
			}
		}

		void text_console::csi_dispatch(int final_byte) {
			int np = pidx_ + 1;
			int n = param_[0];
			switch (final_byte) {
			case 'A':
				wrap_pending_ = false;
				if (!pseen_ || n == 0) { n = 1; }
				row_ -= n; if (row_ < 0) { row_ = 0; }
				break;
			case 'B':
				wrap_pending_ = false;
				if (!pseen_ || n == 0) { n = 1; }
				row_ += n; if (row_ > rows_ - 1) { row_ = rows_ - 1; }
				break;
			case 'C':
				wrap_pending_ = false;
				if (!pseen_ || n == 0) { n = 1; }
				col_ += n; if (col_ > cols_ - 1) { col_ = cols_ - 1; }
				break;
			case 'D':
				wrap_pending_ = false;
				if (!pseen_ || n == 0) { n = 1; }
				col_ -= n; if (col_ < 0) { col_ = 0; }
				break;
			case 'H':
			case 'f': {
				wrap_pending_ = false;
				int rr = pseen_ ? param_[0] : 1;
				int cc = (np >= 2) ? param_[1] : 1;
				if (rr < 1) { rr = 1; }
				if (cc < 1) { cc = 1; }
				row_ = rr - 1; col_ = cc - 1;
				if (row_ > rows_ - 1) { row_ = rows_ - 1; }
				if (col_ > cols_ - 1) { col_ = cols_ - 1; }
				break;
			}
			case 'J': {
				int m = pseen_ ? param_[0] : 0;
				if (m == 2) {
					for (int r = 0; r < rows_; ++r) { for (int c = 0; c < cols_; ++c) { clear_cell(r, c); } }
				} else if (m == 0) {
					for (int c = col_; c < cols_; ++c) { clear_cell(row_, c); }
					for (int r = row_ + 1; r < rows_; ++r) { for (int c = 0; c < cols_; ++c) { clear_cell(r, c); } }
				} else if (m == 1) {
					for (int r = 0; r < row_; ++r) { for (int c = 0; c < cols_; ++c) { clear_cell(r, c); } }
					for (int c = 0; c <= col_; ++c) { clear_cell(row_, c); }
				}
				break;
			}
			case 'K': {
				int m = pseen_ ? param_[0] : 0;
				if (m == 0) {
					for (int c = col_; c < cols_; ++c) { clear_cell(row_, c); }
				} else if (m == 1) {
					for (int c = 0; c <= col_; ++c) { clear_cell(row_, c); }
				} else if (m == 2) {
					for (int c = 0; c < cols_; ++c) { clear_cell(row_, c); }
				}
				break;
			}
			case 'h':
				/* DECSET: ESC[?25h shows the (host-drawn) cursor. Only the private
				   cursor-visibility mode is honored; other modes are consumed and ignored. */
				if (priv_ && n == 25) { cursor_visible_ = true; }
				break;
			case 'l':
				/* DECRST: ESC[?25l hides the cursor overlay. */
				if (priv_ && n == 25) { cursor_visible_ = false; }
				break;
			case 'm':
				if (!pseen_) {
					fg_ = 7; bg_ = 0;
				} else {
					for (int i = 0; i < np; ++i) {
						int p = param_[i];
						if (p == 0) { fg_ = 7; bg_ = 0; }
						else if (p >= 30 && p <= 37) { fg_ = p - 30; }
						else if (p >= 40 && p <= 47) { bg_ = p - 40; }
					}
				}
				break;
			case 'n':
				/* DSR (Device Status Report). ESC[6n queries the cursor position; the
				   console replies on the INPUT stream with ESC[<row>;<col>R, 1-based, the
				   standard VT100 handshake. This is what lets an editor discover the
				   window size when there is no ioctl(TIOCGWINSZ): it homes the cursor to
				   the bottom-right (ESC[999C ESC[999B, clamped by the CUF/CUD cases above)
				   and reads back the clamped position. maize-172 (kilo) drives exactly this
				   path. The reply is injected straight into delivered_ (drained ahead of
				   any scancode input by read_in), so it works in cooked or raw mode. Only
				   the non-private DSR 6 is answered; other reports are ignored. */
				if (!priv_ && n == 6) {
					auto push_dec = [this](int v) {
						char tmp[8];
						int k = 0;
						if (v < 0) { v = 0; }
						do { tmp[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && k < 8);
						while (k > 0) { deliver(static_cast<unsigned char>(tmp[--k])); }
					};
					deliver(0x1B);
					deliver('[');
					push_dec(row_ + 1);
					deliver(';');
					push_dec(col_ + 1);
					deliver('R');
				}
				break;
			default:
				break;   /* unknown final byte: consumed and ignored */
			}
		}

		void text_console::out_byte(int chi) {
			unsigned char ch = static_cast<unsigned char>(chi);
			if (pstate_ == 0) {
				if (ch == 0x1B) { pstate_ = 1; return; }
				if (ch == 0x0D) { wrap_pending_ = false; col_ = 0; return; }
				if (ch == 0x0A) { newline(); return; }
				if (ch == 0x08) { wrap_pending_ = false; if (col_ > 0) { col_--; } return; }
				if (ch == 0x09) {
					wrap_pending_ = false;
					int nc = (col_ & ~7) + 8;
					if (nc > cols_ - 1) { nc = cols_ - 1; }
					col_ = nc;
					return;
				}
				if (ch >= 0x20 && ch <= 0x7E) { put_glyph(ch); return; }
				return;   /* other control bytes ignored */
			}
			if (pstate_ == 1) {
				if (ch == '[') {
					pstate_ = 2; pidx_ = 0; param_[0] = 0; pseen_ = 0; priv_ = 0;
					return;
				}
				pstate_ = 0;   /* unsupported / incomplete ESC sequence: consumed defensively */
				return;
			}
			/* pstate_ == 2: CSI, collecting parameters. */
			if (ch == '?') { priv_ = 1; return; }   /* private-mode intro, e.g. ESC[?25h/l */
			if (ch >= '0' && ch <= '9') {
				param_[pidx_] = param_[pidx_] * 10 + (ch - '0');
				pseen_ = 1;
				return;
			}
			if (ch == ';') {
				if (pidx_ < 7) { pidx_++; param_[pidx_] = 0; }
				return;
			}
			csi_dispatch(ch);
			pstate_ = 0;
		}

		void text_console::write_out(const unsigned char* buf, unsigned long count) {
			for (unsigned long i = 0; i < count; ++i) { out_byte(buf[i]); }
			/* One content render per flush (a guest write() syscall), so --show-perf reports
			   a non-zero rate while output is flowing and 0 when the console is idle. */
			if (count > 0) { render_count_.fetch_add(1, std::memory_order_relaxed); }
		}

		/* Translate one Set-1 scancode into the raw byte(s) it produces (before line
		   discipline). Enter -> CR (cooked ICRNL later maps it to NL for the delivered
		   line; raw delivers CR verbatim). Backspace -> DEL (the VERASE default). Arrow /
		   Home / End / PageUp / PageDown / Delete / Insert -> VT INPUT escape sequences
		   (the encoding is DEFINED here; maize-172 kilo proves it against a real editor).
		   Printable keys index the shift-aware table; Ctrl-<key> folds to a control byte. */
		void text_console::keymap(u_byte sc, std::string& out) const {
			out.clear();
			if (sc == 0x1C) { out.push_back('\r'); return; }          // Enter
			if (sc == 0x0E) { out.push_back(0x7F); return; }          // Backspace -> DEL
			if (sc == 0x0F) { out.push_back('\t'); return; }          // Tab
			switch (sc) {                                             // extended nav keys
			case 0x48: out = "\x1b[A"; return;   // Up
			case 0x50: out = "\x1b[B"; return;   // Down
			case 0x4D: out = "\x1b[C"; return;   // Right
			case 0x4B: out = "\x1b[D"; return;   // Left
			case 0x47: out = "\x1b[H"; return;   // Home
			case 0x4F: out = "\x1b[F"; return;   // End
			case 0x49: out = "\x1b[5~"; return;  // PageUp
			case 0x51: out = "\x1b[6~"; return;  // PageDown
			case 0x53: out = "\x1b[3~"; return;  // Delete
			case 0x52: out = "\x1b[2~"; return;  // Insert
			default: break;
			}
			if (sc >= 0x40) { return; }                               // outside the make table
			/* Caps Lock affects ALPHABETIC keys only: the effective shift for a letter is
			   (shift_ XOR caps_), so Shift and Caps Lock cancel on letters, while digits and
			   symbols always follow shift_ alone. */
			bool is_alpha = (con_sc_lower[sc] >= 'a' && con_sc_lower[sc] <= 'z');
			bool eff_shift = is_alpha ? (shift_ != caps_) : shift_;
			unsigned char a = eff_shift ? con_sc_upper[sc] : con_sc_lower[sc];
			if (a == 0) { return; }
			if (ctrl_ && a >= 0x40 && a <= 0x7F) {
				a = static_cast<unsigned char>(a & 0x1F);            // Ctrl-A..Z -> 0x01..0x1A, etc.
			}
			out.push_back(static_cast<char>(a));
		}

		void text_console::feed_scancode(u_byte sc) {
			/* Modifier make/break first (shift + ctrl), so the shift/ctrl state is set
			   before the key it modifies is decoded. */
			if (sc == 0x2A || sc == 0x36) { shift_ = true; return; }
			if (sc == 0xAA || sc == 0xB6) { shift_ = false; return; }
			if (sc == 0x1D) { ctrl_ = true; return; }
			if (sc == 0x9D) { ctrl_ = false; return; }
			if (sc == 0x3A) { caps_ = !caps_; return; }   // Caps Lock make toggles; break ignored
			if (sc & 0x80) { return; }   // any other break code: ignored

			std::string raw;
			keymap(sc, raw);
			if (raw.empty()) { return; }

			if (canonical()) {
				for (unsigned char b : raw) {
					if (b == cc_[console::TERMIOS_VERASE]) {
						/* Backspace edits the pending line and erases the echoed cell. */
						if (!line_.empty()) {
							line_.pop_back();
							if (echo()) { out_byte(0x08); out_byte(' '); out_byte(0x08); }
						}
					} else if (b == '\r') {
						/* Enter: ICRNL delivers the line terminated with NL; echo a newline. */
						line_.push_back('\n');
						if (echo()) { out_byte('\r'); out_byte('\n'); }
						for (unsigned char lb : line_) { deliver(lb); }
						line_.clear();
					} else {
						line_.push_back(static_cast<char>(b));
						if (echo()) { out_byte(b); }
					}
				}
			} else {
				/* Raw / non-canonical: deliver each byte as it arrives, no line editing. */
				for (unsigned char b : raw) {
					deliver(b);
					if (echo()) { out_byte(b); }
				}
			}

			/* A locally-echoed keystroke repainted the grid, so count it toward the console
			   FPS (a raw-mode read with ECHO off relies on the guest's own write_out instead). */
			if (echo()) { render_count_.fetch_add(1, std::memory_order_relaxed); }
		}

		int text_console::next_stdin_scancode() {
			if (stdin_eof_) { return -1; }
			cpu::set_running(false);   // park: MIPS idle while blocked on host stdin
			unsigned char b = 0;
			u_word nr = maize::syscall::read(0, &b, 1);
			cpu::set_running(true);
			if (nr != 1) { stdin_eof_ = true; return -1; }
			return b;
		}

		int text_console::next_queue_scancode() {
			std::unique_lock<std::mutex> lk(q_mutex_);
			while (scancode_q_.empty()) {
				if (stopped_) { return -1; }
				cpu::set_running(false);   // park on the run-bit substrate: no busy-spin
				q_cv_.wait(lk);
				cpu::set_running(true);
			}
			u_byte sc = scancode_q_.front();
			scancode_q_.pop_front();
			return sc;
		}

		long text_console::read_in(unsigned char* buf, unsigned long count) {
			if (count == 0) { return 0; }
			while (delivered_.empty()) {
				int sc = (input_mode_ == input_mode::STDIN)
					? next_stdin_scancode() : next_queue_scancode();
				if (sc < 0) { break; }   // end of input (host EOF or window closed)
				feed_scancode(static_cast<u_byte>(sc));
			}
			unsigned long n = 0;
			while (n < count && !delivered_.empty()) {
				buf[n++] = delivered_.front();
				delivered_.pop_front();
			}
			return static_cast<long>(n);
		}

		void text_console::push_scancode(u_byte scancode) {
			{
				std::lock_guard<std::mutex> lk(q_mutex_);
				scancode_q_.push_back(scancode);
			}
			q_cv_.notify_one();
		}

		void text_console::stop() {
			{
				std::lock_guard<std::mutex> lk(q_mutex_);
				stopped_ = true;
			}
			q_cv_.notify_all();
		}

		/* termios wire image <-> host flag state (little-endian 32-bit flag words + cc[]). */
		void text_console::termios_get(unsigned char* image) {
			auto put32 = [](unsigned char* p, unsigned v) {
				p[0] = static_cast<unsigned char>(v & 0xFF);
				p[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
				p[2] = static_cast<unsigned char>((v >> 16) & 0xFF);
				p[3] = static_cast<unsigned char>((v >> 24) & 0xFF);
			};
			put32(image + console::TERMIOS_OFF_IFLAG, iflag_);
			put32(image + console::TERMIOS_OFF_OFLAG, oflag_);
			put32(image + console::TERMIOS_OFF_CFLAG, cflag_);
			put32(image + console::TERMIOS_OFF_LFLAG, lflag_);
			std::memcpy(image + console::TERMIOS_OFF_CC, cc_, console::TERMIOS_NCCS);
		}

		void text_console::termios_set(const unsigned char* image) {
			auto get32 = [](const unsigned char* p) -> unsigned {
				return static_cast<unsigned>(p[0])
					| (static_cast<unsigned>(p[1]) << 8)
					| (static_cast<unsigned>(p[2]) << 16)
					| (static_cast<unsigned>(p[3]) << 24);
			};
			iflag_ = get32(image + console::TERMIOS_OFF_IFLAG);
			oflag_ = get32(image + console::TERMIOS_OFF_OFLAG);
			cflag_ = get32(image + console::TERMIOS_OFF_CFLAG);
			lflag_ = get32(image + console::TERMIOS_OFF_LFLAG);
			std::memcpy(cc_, image + console::TERMIOS_OFF_CC, console::TERMIOS_NCCS);
		}

		void text_console::dump_text(std::ostream& out) const {
			for (int r = 0; r < rows_; ++r) {
				std::string line;
				for (int c = 0; c < cols_; ++c) {
					unsigned char ch = glyph_[static_cast<size_t>(r) * cols_ + c];
					line.push_back((ch >= 0x20 && ch <= 0x7E) ? static_cast<char>(ch) : ' ');
				}
				while (!line.empty() && line.back() == ' ') { line.pop_back(); }
				out << line << "\n";
			}
		}

	} // namespace devices
} // namespace maize

#ifdef MAIZE_DISPLAY

#include <SDL.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <sstream>
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
					case SDL_SCANCODE_TAB: return 0x0F;         // automap
					case SDL_SCANCODE_MINUS: return 0x0C;       // reduce view / zoom out
					case SDL_SCANCODE_EQUALS: return 0x0D;      // increase view / zoom in
					case SDL_SCANCODE_BACKSPACE: return 0x0E;
					case SDL_SCANCODE_RCTRL: return 0x1D;       // right ctrl also fires
					case SDL_SCANCODE_RSHIFT: return 0x36;      // right shift also runs
					case SDL_SCANCODE_RALT: return 0x38;        // right alt also strafes
					case SDL_SCANCODE_PAUSE: return 0x45;
					case SDL_SCANCODE_F1: return 0x3B;
					case SDL_SCANCODE_F2: return 0x3C;
					case SDL_SCANCODE_F3: return 0x3D;
					case SDL_SCANCODE_F4: return 0x3E;
					case SDL_SCANCODE_F5: return 0x3F;
					case SDL_SCANCODE_F6: return 0x40;
					case SDL_SCANCODE_F7: return 0x41;
					case SDL_SCANCODE_F8: return 0x42;
					case SDL_SCANCODE_F9: return 0x43;
					case SDL_SCANCODE_F10: return 0x44;
					case SDL_SCANCODE_F11: return 0x57;
					case SDL_SCANCODE_F12: return 0x58;
					/* maize-140: punctuation + navigation keys the text console needs for a
					   usable typing experience (the DOOM key path ignores the extras). */
					case SDL_SCANCODE_SEMICOLON: return 0x27;
					case SDL_SCANCODE_APOSTROPHE: return 0x28;
					case SDL_SCANCODE_GRAVE: return 0x29;
					case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
					case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
					case SDL_SCANCODE_BACKSLASH: return 0x2B;
					case SDL_SCANCODE_SLASH: return 0x35;
					case SDL_SCANCODE_HOME: return 0x47;
					case SDL_SCANCODE_END: return 0x4F;
					case SDL_SCANCODE_PAGEUP: return 0x49;
					case SDL_SCANCODE_PAGEDOWN: return 0x51;
					case SDL_SCANCODE_DELETE: return 0x53;
					case SDL_SCANCODE_INSERT: return 0x52;
					default: return 0;
				}
			}

			/* maize-222: the --show-perf overlay renders with the console font (font8x16, Model 30)
			   so the live M<mips> F<fps> readout matches the console face. Any printable ASCII
			   glyph renders; the earlier digit-only 3x5 micro-font is gone. */
			static void draw_text(SDL_Renderer* ren, int x, int y, int px, const std::string& s) {
				for (char ch : s) {
					int c = static_cast<unsigned char>(ch);
					if (c >= FONT_FIRST && c <= FONT_LAST) {
						const unsigned char* g = font8x16[c - FONT_FIRST];
						for (int row = 0; row < FONT_H; ++row) {
							unsigned char bits = g[row];
							for (int col = 0; col < FONT_W; ++col) {
								if (bits & (1 << col)) {   // LSB-first: bit col == pixel column col
									SDL_Rect r { x + col * px, y + row * px, px, px };
									SDL_RenderFillRect(ren, &r);
								}
							}
						}
					}
					x += FONT_W * px;   // fixed-width glyphs carry their own side bearing
				}
			}

			void run(framebuffer_device& fb, keyboard_device& kbd, text_console& con,
				unsigned scale, bool show_perf, unsigned refresh_hz, bool pause_on_halt,
				bool vsync) {
				kbd.use_window_source();

				/* Refresh period from the requested rate (the "monitor" cadence). Clamp to a sane
				   range; per-frame sleep is the integer-ms period. */
				if (refresh_hz < 1) { refresh_hz = 1; }
				if (refresh_hz > 1000) { refresh_hz = 1000; }
				unsigned refresh_ms = 1000u / refresh_hz;
				if (refresh_ms < 1) { refresh_ms = 1; }

				if (SDL_Init(SDL_INIT_VIDEO) != 0) {
					/* No display available: fall back to headless execution. */
					cpu::run();
					return;
				}

				if (scale < 1) { scale = 1; }

				/* maize-140: text/graphics arbitration (D3). The text console owns the window
				   by default, so the window opens at the CONSOLE resolution (cw x ch, e.g.
				   640x400 -> 80x50 cells). When a guest graphics program (DOOM) explicitly
				   claims the framebuffer, we switch to the framebuffer surface and resize the
				   window to the framebuffer resolution (fbw x fbh) so DOOM's window geometry is
				   exactly what it was before this card. The two surfaces have their own
				   streaming textures; only one is presented per frame. */
				int cw = static_cast<int>(con.width());
				int ch = static_cast<int>(con.height());
				int fbw = static_cast<int>(fb.width());
				int fbh = static_cast<int>(fb.height());

				/* maize-218: clamp the initial window to the monitor work area so a large
				   --display-scale never pushes the (centered) window's title bar off-screen. */
				{
					SDL_Rect usable;
					if (SDL_GetDisplayUsableBounds(0, &usable) == 0) {
						while (scale > 1 &&
							(cw * static_cast<int>(scale) > usable.w ||
							 ch * static_cast<int>(scale) > usable.h)) {
							--scale;
						}
					}
				}

				SDL_Window* win = SDL_CreateWindow("Maize",
					SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
					cw * static_cast<int>(scale), ch * static_cast<int>(scale),
					SDL_WINDOW_RESIZABLE);
				SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
				/* maize-227: align presents to the monitor's vblank. Without this the
				   graphics present is paced only by the software refresh_ms timer and
				   SDL_RenderPresent returns immediately, so presents free-run against the
				   physical monitor / compositor and slowly beat with it -- the rare "freeze
				   a frame or two, then catch up" hitch. With vsync, SDL_RenderPresent blocks
				   until vblank; the next_vsync deadline goes overdue during that block, so
				   the following wait shrinks and present throughput is not capped below the
				   guest's frame rate. refresh_hz still drives the guest vsync-IRQ cadence
				   (fb.on_display_refresh), so the guest contract is unchanged. Fail-soft: a
				   driver that cannot vsync logs once and presents unsynced (prior behavior). */
				if (vsync && ren) {
					if (SDL_RenderSetVSync(ren, 1) != 0) {
						std::cerr << "maize: display vsync unavailable (" << SDL_GetError()
							<< "); presenting unsynced\n";
					}
				}
				SDL_RenderSetLogicalSize(ren, cw, ch);
				SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);   // alpha for the fps box
				SDL_Texture* con_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
					SDL_TEXTUREACCESS_STREAMING, cw, ch);
				SDL_Texture* fb_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
					SDL_TEXTUREACCESS_STREAMING, fbw, fbh);
				bool graphics_mode = false;   // false = console surface; true = framebuffer

				/* maize-218: the framebuffer presents into the FIXED console canvas (cw x ch),
				   aspect-preserving and integer-scaled when it divides evenly (DOOM 320x200 ->
				   2x -> 640x400, filling the console footprint), centered with black letterbox
				   bars otherwise. Computed once: the fb and console geometry are fixed for the
				   run. The window is NEVER resized on graphics takeover, so there is no mid-run
				   resize or reposition. */
				SDL_Rect fb_dst;
				{
					int si = (cw / fbw < ch / fbh) ? cw / fbw : ch / fbh;   // max integer scale
					if (si >= 1) {
						fb_dst.w = fbw * si;
						fb_dst.h = fbh * si;
					} else {
						/* framebuffer larger than the canvas: aspect-preserving downscale to fit */
						double az = (double)cw / fbw < (double)ch / fbh
							? (double)cw / fbw : (double)ch / fbh;
						fb_dst.w = static_cast<int>(fbw * az);
						fb_dst.h = static_cast<int>(fbh * az);
					}
					fb_dst.x = (cw - fb_dst.w) / 2;
					fb_dst.y = (ch - fb_dst.h) / 2;
				}

				/* --show-perf overlay state: every ~500 ms, sample the FPS source -> FPS and the
				   guest instruction counter -> MIPS (VM work rate), track the peak of each for
				   the on-exit report, and render both as one line "M<mips> F<fps>". The FPS
				   source depends on the active surface: the framebuffer present counter in
				   graphics mode (guest frames produced), the console render counter otherwise
				   (a console program never presents a framebuffer, so it would read a flat 0).
				   FPS is distinct from the ~60 Hz window refresh below. Init from the console
				   counter since the text console owns the window until a graphics claim. */
				std::uint64_t perf_last_present = con.render_count();
				std::uint64_t perf_last_insn = cpu::instruction_count();
				std::uint32_t perf_last_ticks = SDL_GetTicks();
				std::string perf_str = "M0 F0";

				/* maize-222: register the per-device perf sources for this windowed run. The CPU
				   source is always active; the display source (frames/FPS) is registered because a
				   window is attached here. The sampler below writes peaks into these; perf::emit()
				   at halt prints one section per source. */
				perf::cpu_source cpu_perf;
				perf::display_source disp_perf;
				disp_perf.frames = [&fb]() -> std::uint64_t { return fb.present_count(); };
				if (show_perf) {
					perf::reset();
					perf::add(&cpu_perf);
					perf::add(&disp_perf);
				}

				/* guest_done latches true when cpu::run() returns (the guest halted, e.g.
				   DOOM calling exit()). The window loop exits on either the window closing
				   (SDL_QUIT) OR the guest finishing; without the latter a guest exit would
				   leave this loop spinning on the last frame forever, freezing the window.
				   The latch (not a live power-state poll) avoids a startup race with run()
				   setting power on. Joined below before guest_done goes out of scope. */
				std::atomic<bool> guest_done {false};
				std::thread guest([&guest_done]() { cpu::run(); guest_done = true; });

				/* maize-140 dirty-present state: present (texture upload + clear + copy +
				   overlays + RenderPresent) only when something visibly changed since the last
				   present, so an idle console with a solid cursor stops re-uploading and
				   re-presenting the same frame every refresh (the blink-driven repaint was the
				   palpable typing slowdown). The refresh sleep and fb.on_display_refresh() below
				   stay UNCONDITIONAL regardless of this gate. last_input_tick is the SDL tick of
				   the most recent keystroke pushed to the console; within 500 ms of it the cursor
				   is drawn solid (steady while typing), after which it resumes blinking. Init it
				   ~1 s in the past (modular subtraction) so the console starts idle/blinking. */
				std::uint64_t last_render_count = 0;
				std::uint64_t last_present_count = 0;
				std::string last_perf_str;
				int last_cursor_phase = -1;
				bool force_present = true;   // first frame and post-surface-swap force a present
				std::uint32_t last_input_tick = SDL_GetTicks() - 1000u;

				/* maize-176 wake/present scheduling. Two INDEPENDENT schedules replace the old
				   poll-once-then-sleep(refresh_ms) that coupled input AND echo latency to the
				   refresh rate:
				     - next_vsync is an absolute-deadline schedule at the refresh_ms ("monitor")
				       cadence: fb.on_display_refresh() fires when the deadline passes (the vsync
				       IRQ / HALT-wake backstop) and the deadline advances by refresh_ms, so an
				       early wake on input neither drifts nor double-fires it. It also paces the
				       graphics-mode present at refresh_hz.
				     - last_console_present drives the console (text) surface, which presents on
				       demand no more than once per console_min_gap (~60Hz), so echo appears within
				       ~16ms of the guest writing it at ANY refresh_hz, not once per refresh period.
				   The loop blocks in SDL_WaitEventTimeout until the soonest of these (or the idle
				   cursor blink) is due, so a keystroke wakes it immediately while an idle console
				   never busy-spins. */
				const std::uint32_t console_min_gap = 16u;   // ~60Hz console present cap
				std::uint32_t next_vsync = SDL_GetTicks() + refresh_ms;
				std::uint32_t last_console_present = SDL_GetTicks() - 1000u;
				bool console_present_pending = false;   // dirty console content held off by the gap

				bool running = true;
				while (running && !guest_done.load(std::memory_order_acquire)) {
					/* Switch to the framebuffer surface the first time a graphics program claims
					   it (DOOM's DG_Init, essentially at startup). maize-218: the window and the
					   render logical size stay at the console canvas; the framebuffer is fit into
					   that canvas at present time (fb_dst), so there is no mid-run window resize. */
					if (!graphics_mode && fb.graphics_claimed()) {
						graphics_mode = true;
						/* The FPS source switches from the console counter to the framebuffer
						   present counter here; rebaseline so the swap does not register a
						   bogus one-sample spike from the counters' differing magnitudes. */
						perf_last_present = fb.present_count();
						perf_last_ticks = SDL_GetTicks();
						/* The presented surface just changed; force one present so the swap is
						   not swallowed by the dirty gate. */
						force_present = true;
					}

					/* maize-176: block until an event arrives OR the soonest scheduled tick is
					   due, so input latency is independent of refresh_hz. The timeout is the ms
					   until the nearest of: the next vsync tick, the next cursor-blink toggle (only
					   when the cursor is idle/blinking), and a pending console present held off by
					   the ~16ms gap. Clamp to >=1ms (never wait 0) and <= the vsync interval (so the
					   on_display_refresh backstop still fires each period). */
					std::uint32_t wake_now = SDL_GetTicks();
					std::uint32_t timeout = refresh_ms;
					{
						std::uint32_t until_vsync = (std::int32_t)(next_vsync - wake_now) > 0
							? (next_vsync - wake_now) : 0u;
						if (until_vsync < timeout) { timeout = until_vsync; }
					}
					bool wake_cursor_solid = (wake_now - last_input_tick) < 500u;
					if (!graphics_mode && con.cursor_visible() && !wake_cursor_solid) {
						std::uint32_t until_blink = 500u - (wake_now % 500u);
						if (until_blink < timeout) { timeout = until_blink; }
					}
					if (console_present_pending) {
						std::uint32_t due = last_console_present + console_min_gap;
						std::uint32_t until_present = (std::int32_t)(due - wake_now) > 0
							? (due - wake_now) : 0u;
						if (until_present < timeout) { timeout = until_present; }
					}
					if (timeout < 1u) { timeout = 1u; }
					if (timeout > refresh_ms) { timeout = refresh_ms; }

					/* Wait for the first event (or the timeout), then drain the rest with
					   SDL_PollEvent so a burst is handled in one wake. */
					SDL_Event e;
					if (SDL_WaitEventTimeout(&e, static_cast<int>(timeout))) {
						do {
							if (e.type == SDL_QUIT) {
								running = false;
							}
							else if (e.type == SDL_KEYDOWN) {
								if (graphics_mode) {
									/* Graphics (DOOM): one make per physical press. DOOM tracks held
									   keys via make/break, so injected key-repeat makes would confuse
									   it; keep the repeat==0 filter on this path. */
									if (e.key.repeat == 0) {
										u_byte sc = map_scancode(e.key.keysym.scancode);
										if (sc) { kbd.push_event(sc); }
									}
								}
								else {
									/* Console: process KEYDOWN INCLUDING repeats so a held key
									   autorepeats at the OS repeat rate; refresh last_input_tick on
									   repeats too so the cursor stays solid while a key is held (SDL
									   time stays on the SDL side, so the console needs no timestamp
									   of its own). */
									u_byte sc = map_scancode(e.key.keysym.scancode);
									if (sc) {
										con.push_scancode(sc);
										last_input_tick = SDL_GetTicks();
									}
								}
							}
							else if (e.type == SDL_KEYUP) {
								u_byte sc = map_scancode(e.key.keysym.scancode);
								if (sc) {
									u_byte brk = static_cast<u_byte>(sc | 0x80);   // break code
									if (graphics_mode) { kbd.push_event(brk); }
									else { con.push_scancode(brk); }
								}
							}
						} while (SDL_PollEvent(&e));
					}

					if (show_perf) {
						std::uint32_t now = SDL_GetTicks();
						std::uint32_t dt = now - perf_last_ticks;
						if (dt >= 500) {
							std::uint64_t p = graphics_mode ? fb.present_count() : con.render_count();
							std::uint64_t ic = cpu::instruction_count();
							int fps = static_cast<int>((p - perf_last_present) * 1000ull / dt);
							/* MIPS = delta-instructions / (dt_ms * 1000). */
							int mips = static_cast<int>((ic - perf_last_insn)
								/ (static_cast<std::uint64_t>(dt) * 1000ull));
							perf_str = "M" + std::to_string(mips) + " F" + std::to_string(fps);
							if (fps > disp_perf.peak_fps) { disp_perf.peak_fps = fps; }   // maize-222
							if (mips > cpu_perf.peak_mips) { cpu_perf.peak_mips = mips; }
							perf_last_present = p;
							perf_last_insn = ic;
							perf_last_ticks = now;
						}
					}

					/* maize-176 vsync tick (absolute-deadline schedule): fire the once-per-refresh
					   vblank IRQ / HALT-wake backstop when its deadline passes, then advance the
					   deadline by refresh_ms so an early wake on input neither drifts nor double-
					   fires it. If we fell a full period or more behind (e.g. a debugger pause),
					   resync to now so we do not fire a catch-up burst. This tick also paces the
					   graphics-mode present at refresh_hz (see present_due below). */
					bool vsync_ticked = false;
					if ((std::int32_t)(SDL_GetTicks() - next_vsync) >= 0) {
						fb.on_display_refresh();
						vsync_ticked = true;
						next_vsync += refresh_ms;
						if ((std::int32_t)(SDL_GetTicks() - next_vsync) >= 0) {
							next_vsync = SDL_GetTicks() + refresh_ms;
						}
					}

					/* maize-140 dirty gate: decide whether anything visible changed since the last
					   present. Only this present block is skipped when nothing moved; the vsync tick
					   above still fires each refresh period regardless. Cursor timing: within 500 ms
					   of the last keystroke the cursor is SOLID (it holds steady while the operator
					   types, so echoes are the only presents), and after that idle window it BLINKS
					   on the ~500 ms frame-clock phase. */
					std::uint32_t now_ticks = SDL_GetTicks();
					bool cursor_solid = (now_ticks - last_input_tick) < 500u;
					int cursor_phase = static_cast<int>((now_ticks / 500u) & 1u);
					bool blink_on = (cursor_phase == 0);
					bool cursor_on = !graphics_mode && con.cursor_visible()
						&& (cursor_solid || blink_on);

					std::uint64_t render_count = con.render_count();
					std::uint64_t present_count = fb.present_count();
					bool dirty = force_present;
					if (graphics_mode) {
						if (present_count != last_present_count) { dirty = true; }
					} else {
						if (render_count != last_render_count) { dirty = true; }
						/* A blinking (not solid) cursor toggles ~2x/sec: present on each phase
						   flip so it visibly blinks; a solid cursor needs no timer-driven present. */
						if (!cursor_solid && con.cursor_visible()
							&& cursor_phase != last_cursor_phase) {
							dirty = true;
						}
						if (show_perf && perf_str != last_perf_str) { dirty = true; }
					}

					/* maize-176 present scheduling (two INDEPENDENT schedules): the console (text)
					   surface presents on demand at up to ~60Hz (>= console_min_gap between
					   presents), so echo latency is bounded by ~16ms at ANY refresh_hz; the
					   framebuffer (graphics) surface presents on the vsync tick, i.e. at refresh_hz.
					   A forced present (first frame / surface swap) bypasses the gate. */
					bool present_due;
					if (graphics_mode) {
						present_due = vsync_ticked;
					} else {
						present_due = (std::uint32_t)(now_ticks - last_console_present) >= console_min_gap;
					}

					if (dirty && (force_present || present_due)) {
						bool presented = false;
						if (graphics_mode) {
							const std::vector<std::uint32_t>& frame = fb.frame();
							if (!frame.empty()) {
								SDL_UpdateTexture(fb_tex, nullptr, frame.data(),
									fbw * static_cast<int>(sizeof(std::uint32_t)));
								/* maize-218: black letterbox bars, then the framebuffer fit into the
								   fixed console canvas (fb_dst) instead of stretched to fill. */
								SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
								SDL_RenderClear(ren);
								SDL_RenderCopy(ren, fb_tex, nullptr, &fb_dst);
								presented = true;
							}
						} else {
							/* Console surface: blit the host grid pixel buffer. */
							SDL_UpdateTexture(con_tex, nullptr, con.pixels(),
								cw * static_cast<int>(sizeof(std::uint32_t)));
							SDL_RenderClear(ren);
							SDL_RenderCopy(ren, con_tex, nullptr, nullptr);
							presented = true;
						}

						if (presented) {
							/* Block cursor: a present-time overlay (drawn like the perf box),
							   never written into the console pixel buffer, so it needs no
							   cell-restore bookkeeping and cannot leave artifacts on move/scroll.
							   Console surface only; honor the guest's ESC[?25l hide. Drawn when
							   the cursor is solid (recent typing) or the blink phase is on. The
							   cell is a FONT_W x FONT_H block at (col*FONT_W, row*FONT_H) in logical space, which
							   equals the console pixel space (RenderSetLogicalSize == console
							   resolution), so SDL scales it by the same factor as the console
							   texture. A translucent light fill reads as a classic inverted block
							   while leaving the glyph legible. */
							if (cursor_on) {
								SDL_Rect cur { con.cursor_col() * FONT_W, con.cursor_row() * FONT_H, FONT_W, FONT_H };
								SDL_SetRenderDrawColor(ren, 0xC0, 0xC0, 0xC0, 0xA0);
								SDL_RenderFillRect(ren, &cur);
							}
							if (show_perf) {
								/* Top-left overlay in logical space, so SDL scales it with the
								   surface: a translucent box behind green "M<mips> F<fps>" text at
								   a small font (px == 1 logical pixel per font pixel). */
								int px = 1;
								int boxw = static_cast<int>(perf_str.size()) * FONT_W * px + 2;   // maize-222: font8x16
								int boxh = FONT_H * px + 2;
								SDL_Rect box { 1, 1, boxw, boxh };
								SDL_SetRenderDrawColor(ren, 0, 0, 0, 180);
								SDL_RenderFillRect(ren, &box);
								SDL_SetRenderDrawColor(ren, 0, 255, 0, 255);
								draw_text(ren, 2, 2, px, perf_str);
							}
							SDL_RenderPresent(ren);

							/* Latch what we just presented so the next iteration can tell whether
							   anything moved. Only updated on an actual present, so a graphics-mode
							   empty frame leaves force_present set until real content arrives. */
							last_render_count = render_count;
							last_present_count = present_count;
							last_cursor_phase = cursor_phase;
							last_perf_str = perf_str;
							force_present = false;
							/* Console surface just presented: reset its ~60Hz gap and clear any
							   deferred-present flag. The graphics surface is paced by the vsync
							   tick, so it does not touch the console schedule. */
							if (!graphics_mode) {
								last_console_present = now_ticks;
								console_present_pending = false;
							}
						}
					} else if (dirty && !graphics_mode) {
						/* Dirty console content held off by the ~16ms present cap: remember it so
						   the wait at the top of the loop schedules a wake when the gap elapses. */
						console_present_pending = true;
					}
				}

				/* Distinguish a genuine guest halt from a user window-close: --pause-on-halt
				   only holds for a halt. If the loop exited on SDL_QUIT the guest is usually
				   still running (guest_done false) and power_off below stops it. */
				bool guest_halted = guest_done.load(std::memory_order_acquire);

				/* Unblock a console fd-0 read parked on the scancode queue, then stop the VM,
				   so a guest blocked in read() at window-close does not wedge the join below. */
				con.stop();
				cpu::power_off();
				if (guest.joinable()) {
					guest.join();
				}

				/* maize-217: on-exit performance report. Replaces the old modal message box
				   (SDL_ShowSimpleMessageBox), which was intrusive and blocked teardown. The
				   report goes to stderr (visible on console runs / maizec / logs) AND is written
				   into the text-console grid so a windowed run can read it when the window is
				   held open with --pause-on-halt. */
				if (show_perf) {
					std::ostringstream oss;
					oss.put(13); oss.put(10);   // maize-222: leading blank line
					perf::emit(oss);
					std::string report = oss.str();
					std::cerr << report << std::flush;
					con.write_out(reinterpret_cast<const unsigned char*>(report.data()),
						static_cast<unsigned long>(report.size()));
					/* Show the console (carrying the report) for the pause; reset the logical
					   size in case a graphics guest had switched it to the framebuffer geometry. */
					graphics_mode = false;
					SDL_RenderSetLogicalSize(ren, cw, ch);
				}

				/* maize-217: --pause-on-halt. On a genuine guest halt (not a user window-close),
				   hold the window open on its final frame (plus any perf report just written to
				   the console) until the operator presses a key or closes the window, then tear
				   down. Default off: a plain run closes immediately, as before. */
				if (pause_on_halt && guest_halted) {
					SDL_SetWindowTitle(win, "Maize - halted (press a key or close the window)");
					/* Visible prompt: written to the text console so it shows in the window for
					   a text program (and after any perf report just written above); graphics
					   programs (DOOM) keep their final frame, with the prompt in the title bar. */
					std::string prompt = "\r\n-- Maize halted: press any key or close the window to exit --\r\n";
					con.write_out(reinterpret_cast<const unsigned char*>(prompt.data()),
						static_cast<unsigned long>(prompt.size()));
					SDL_RenderClear(ren);
					if (graphics_mode) {
						const std::vector<std::uint32_t>& frame = fb.frame();
						if (!frame.empty()) {
							SDL_UpdateTexture(fb_tex, nullptr, frame.data(),
								fbw * static_cast<int>(sizeof(std::uint32_t)));
							SDL_RenderCopy(ren, fb_tex, nullptr, &fb_dst);   // maize-218: fit into the fixed canvas, not stretch
						}
					} else {
						SDL_UpdateTexture(con_tex, nullptr, con.pixels(),
							cw * static_cast<int>(sizeof(std::uint32_t)));
						SDL_RenderCopy(ren, con_tex, nullptr, nullptr);
					}
					SDL_RenderPresent(ren);
					bool held = true;
					SDL_Event pe;
					while (held && SDL_WaitEvent(&pe)) {
						if (pe.type == SDL_QUIT || pe.type == SDL_KEYDOWN) { held = false; }
					}
				}

				SDL_DestroyTexture(con_tex);
				SDL_DestroyTexture(fb_tex);
				SDL_DestroyRenderer(ren);
				SDL_DestroyWindow(win);
				SDL_Quit();
			}

		} // namespace display
	} // namespace devices
} // namespace maize

#endif // MAIZE_DISPLAY
