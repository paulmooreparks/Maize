#pragma once

/* Host-backed standard device models (device-plugin API). These are the compile-time,
   statically-linked device models attached in maize.cpp: the console, the keyboard, and
   the memory-backed framebuffer. Each is built on the `device` base's on_port_write /
   on_port_read hooks (a device that spans several ports registers one small proxy per
   port, exactly as the timer registers its three registers). The headless model needs no
   external dependency; the optional SDL2 window backend sits behind MAIZE_DISPLAY. */

#include "maize_cpu.h"
#include "console_io.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

namespace maize {
	namespace devices {

		/* A host device that owns one or more ports. The base device hook carries no port
		   id, so each port is a small proxy tagged with a role that forwards to the owner's
		   role-dispatched handlers. port_host is an abstract interface so device_port stays
		   a plain (non-template) class whose hook bodies are defined out-of-line, sidestepping
		   any question of when a class template's virtuals would be instantiated. */
		class port_host {
		public:
			virtual ~port_host() = default;
			virtual void           port_write(int role, cpu::reg_value const& value, cpu::subreg_enum value_subreg) = 0;
			virtual cpu::reg_value port_read(int role, cpu::subreg_enum dst_subreg) = 0;
		};

		/* A single port of a multi-port host device: forwards the two device hooks to the
		   owning port_host with a role tag. */
		struct device_port : public cpu::device {
			port_host* owner {nullptr};
			int role {0};
			void on_port_write(cpu::reg_value const& value, cpu::subreg_enum value_subreg) override;
			cpu::reg_value on_port_read(cpu::subreg_enum dst_subreg) override;
		};

		/* Console / terminal port device (0x00 data, 0x01 status). Bridges the abstract
		   console to host stdio: OUT 0x00 writes a byte to stdout, IN 0x00 reads a byte
		   from stdin, 0x01 reports output-ready / input-available. When selected as the
		   active input source it injects stdin bytes on the instruction tick and raises
		   IRQ 33. The native SYS-based terminal path is untouched and stays the default. */
		class console_device : public cpu::input_device, public port_host,
			public console::stdin_injector {
		public:
			enum role { ROLE_DATA, ROLE_STATUS };

			console_device();
			void attach();
			void on_input_tick() override;

			/* maize-238 (Branch A): stdin_injector -- the single-host-stdin-owner seam so a
			   bare-VM guest's SYS $00 read(0) and this device's eager on_input_tick pre-read
			   do not both consume host stdin. drain_stdin returns any latched byte first,
			   then blocks for one more. */
			bool stdin_is_active() const override { return active_injector_; }
			long drain_stdin(unsigned char* buf, unsigned long count) override;

			/* maize-94: mark this device as the EAGER active injector (set only under an
			   explicit --input=console, where on_input_tick pre-reads host stdin per
			   instruction and drives the park+IRQ multi-process model, doc 17). When it is
			   NOT the active injector (the default console-binary path), the status port
			   reports input-ready so the guest reads the data port on demand, which does a
			   blocking read of host stdin only when the guest actually asks for a byte: the
			   demand-driven model that makes an interactive quesOS shell work on the default
			   invocation without the eager pre-read stalling the guest's own output. */
			void set_active_injector(bool b) { active_injector_ = b; }

			void port_write(int role, cpu::reg_value const& value, cpu::subreg_enum value_subreg) override;
			cpu::reg_value port_read(int role, cpu::subreg_enum dst_subreg) override;

		private:
			device_port data_port_;
			device_port status_port_;
			u_word data_byte_ {0};
			bool available_ {false};
			bool exhausted_ {false};   // eager injector reached host-stdin EOF
			bool eof_ {false};         // maize-94: on-demand data-port read reached EOF
			bool active_injector_ {false};
			/* maize-238 (Branch A, OQ 9204): on_input_tick now runs on EVERY instruction tick
			   under the default-path injector, so an unconditional host-stdin poll per tick
			   would be a syscall per VM instruction (a real interpreter-hot-path cost that
			   would sink doom_bench). Throttle the poll to once per POLL_TICK_DIV ticks: the
			   amortized cost is negligible and the wake latency stays bounded (a few hundred
			   instructions, microseconds on the idle spin), meeting AC 9199's bounded wake. */
			static constexpr unsigned POLL_TICK_DIV {512};
			unsigned poll_tick_ctr_ {0};
		};

		/* Keyboard device (0x10 data, 0x11 status). Emits raw PC scancodes (Set-1 / XT
		   make + break codes). Headless mode sources scancodes from the injected stdin
		   byte stream (each stdin byte IS a scancode); the SDL2 windowed backend pushes
		   Set-1 scancodes through push_event instead. A key event makes a code readable at
		   0x10, sets 0x11 bit0, and raises IRQ 34; reading 0x10 clears key-available. */
		class keyboard_device : public cpu::input_device, public port_host {
		public:
			enum role { ROLE_DATA, ROLE_STATUS };

			keyboard_device();
			void attach();
			void on_input_tick() override;

			/* Windowed backend: switch the input source from stdin to the event queue,
			   and push a Set-1 scancode from the SDL thread (thread-safe). */
			void use_window_source();
			void push_event(u_byte scancode);

			void port_write(int role, cpu::reg_value const& value, cpu::subreg_enum value_subreg) override;
			cpu::reg_value port_read(int role, cpu::subreg_enum dst_subreg) override;

		private:
			/* Fill the one-scancode latch (scancode_/available_) from the window queue and
			   raise the keyboard IRQ, if a key is pending and the latch is free. Caller must
			   hold queue_mutex_. Called from push_event (SDL thread) and port_read/on_input_tick
			   (CPU thread): because it raises the IRQ from whichever thread supplies the key, a
			   CPU parked in HALT is woken by an SDL-thread key press, not only by on_input_tick. */
			void pump_latch();

			device_port data_port_;
			device_port status_port_;
			u_word scancode_ {0};
			/* available_ is read lock-free by the status-port poll and written under
			   queue_mutex_ by pump_latch; atomic so the cross-thread read is well-defined. */
			std::atomic<bool> available_ {false};
			bool exhausted_ {false};
			bool window_source_ {false};
			std::mutex queue_mutex_;
			std::deque<u_byte> queue_;
			/* Lock-free fast path for on_input_tick: mirrors queue_.size() (updated under the
			   lock) so the common no-key-pending tick returns without locking. */
			std::atomic<size_t> queue_size_ {0};
		};

		/* Memory-backed framebuffer (0x50-0x57 control ports). Pixels live in ordinary
		   guest RAM: the guest writes them with normal ST/CP stores at full speed, with no
		   per-pixel device dispatch and NO MMIO (the pixel memory has no device side
		   effects). The control plane is ports:

		     0x50 width      R   host config (pixels)
		     0x51 height     R   host config (pixels)
		     0x52 format     R   host config (1 = XRGB8888, 0x00RRGGBB)
		     0x53 base       R/W guest address of the SELECTED slot's pixel buffer
		     0x54 present    W   present the selected slot's frame; R bit0 = last present valid
		     0x55 status     R   bit0 vsync-pending, bit2 register-rejected; W bit1 vsync-IRQ-enable / ack
		     0x56 slot       R/W select the target slot (0..fb_max_slots-1) for 0x53/0x54/0x55
		     0x57 activate   R/W W switch the active surface; R the active slot / console sentinel

		   maize-236: the device holds a fixed-size REGISTRATION TABLE (fb_max_slots slots),
		   generalizing the maize-140 one-shot takeover latch. Each slot owns its own pixel
		   base + capture frame; N guest processes register N slots with zero new copies
		   (each app brings its own guest-RAM buffer). Ports 0x53/0x54/0x55 are slot-relative
		   on whichever slot 0x56 selects. A nonzero base write claims the selected slot; a
		   zero base write releases it. 0x57 chooses which claimed slot (or the text console)
		   is scanned out; a small activation history reverts scanout when the active slot is
		   released (VT-style switch-back). The device stays process-agnostic: it knows slots,
		   not PIDs; process ownership lives entirely in quesOS (workbench doc 18).

		   On a present the device reads [base, base + width*height*4) from guest memory (the
		   sparse array; every address is defined, so no host OOB) into the selected slot's
		   frame. The buffer size is bounded by the host-fixed resolution, never
		   guest-controlled, so there is no unbounded allocation. A base of 0 (unset) or a
		   base+size that would wrap the address space is an invalid present (defined,
		   non-crashing; last-present-valid reads 0). WIDTH/HEIGHT/FORMAT are global and apply
		   to every slot identically (Decision D2, no per-slot resolution). */
		class framebuffer_device : public port_host {
		public:
			enum role { ROLE_WIDTH, ROLE_HEIGHT, ROLE_FORMAT, ROLE_BASE, ROLE_PRESENT,
				ROLE_STATUS, ROLE_SLOT, ROLE_ACTIVATE };

			framebuffer_device(u_hword width, u_hword height);
			void attach();

			void port_write(int role, cpu::reg_value const& value, cpu::subreg_enum value_subreg) override;
			cpu::reg_value port_read(int role, cpu::subreg_enum dst_subreg) override;

			u_hword width() const { return width_; }
			u_hword height() const { return height_; }
			/* Host-side captured frame (XRGB8888), width*height entries, of the ACTIVE slot
			   (the scanned-out surface); the SDL2 backend blits from here. Empty when the
			   console is active (active_slot() < 0), which the caller checks before blitting. */
			const std::vector<std::uint32_t>& frame() const;
			bool last_present_valid() const;
			/* Monotonic count of valid presents (guest frames produced), summed across all
			   slots. The SDL backend samples the delta over time to show the guest frame rate;
			   for a single-app run (one slot) this is exactly that slot's present count. */
			std::uint64_t present_count() const { return present_count_.load(std::memory_order_relaxed); }

			/* maize-140/236: the raw-framebuffer takeover signal (text/graphics arbitration).
			   A guest graphics program opts into owning a surface by programming a nonzero
			   pixel-buffer base into port 0x53 (on the selected slot); that deliberate device
			   programming claims the slot. This reads true once ANY slot is claimed, so the
			   maize-221 console diagnostic call site (maize.cpp) needs no change. This is an
			   explicit guest action, NOT host-side auto-detection of the output byte stream. */
			bool graphics_claimed() const { return claimed_count_.load(std::memory_order_acquire) > 0; }

			/* maize-236: whether a guest ever ATTEMPTED to claim a slot (a nonzero base write
			   on an unclaimed slot), whether it succeeded or was rejected. The maize-221
			   console diagnostic keys on this: on a display-less console view a claim is
			   rejected (never claimed), so graphics_claimed() stays false, but the guest still
			   declared itself graphical and the operator still needs the use-the-graphical-
			   binary message. */
			bool graphics_claim_attempted() const { return claim_attempted_.load(std::memory_order_acquire); }

			/* maize-236: the currently scanned-out surface: a slot index (>=0), or -1 for the
			   text console. The display thread reads this each frame to pick which surface to
			   present and where to route input; the ACTIVATE port ($57) and the host VT hotkey
			   both drive it through request_activate(). */
			int active_slot() const { return active_slot_.load(std::memory_order_acquire); }

			/* maize-236: switch the active (scanned-out) surface. `target` is a slot index
			   (0..fb_max_slots-1) or fb_console_sentinel for the text console. A no-op when
			   the target is already active, or is a slot that is not currently claimed, or is
			   out of range (defined, non-crashing). Called by the ACTIVATE port write (guest
			   or quesOS) and by the SDL VT hotkey (Decision D9: one funnel for both). */
			void request_activate(u_word target);

			/* maize-221: a console-only VM (no display backend) has no window to show the
			   framebuffer. When set, a rejected claim (display unavailable) stops the VM
			   (cpu::power_off) so main can print a "run this with the graphical binary"
			   diagnostic instead of letting a graphical guest run invisibly. This is the bare
			   single-tasking path; a multi-process quesOS boot leaves it clear and takes the
			   per-exec rejection path (set_display_available) instead. */
			void set_stop_on_claim(bool on) { stop_on_claim_.store(on, std::memory_order_relaxed); }

			/* maize-236: whether a claim can be honored on this view. Default true (a window
			   is present, or a headless run where claims are pure bookkeeping). Set false for
			   a console-only view with no window: a claim is then rejected (slot stays
			   unclaimed, STATUS bit2 set) so the syscall layer can return -ENODEV to just that
			   caller, evolving maize-221 from whole-VM power-off to per-exec rejection. */
			void set_display_available(bool on) { display_available_.store(on, std::memory_order_relaxed); }

			/* Called by the display thread once per refresh (vblank). If the guest has enabled
			   the vsync IRQ (port $55 bit1), set vsync-pending and raise the framebuffer IRQ so
			   a guest parked in HALT wakes at the refresh rate. This is the periodic "monitor"
			   vblank, distinct from a guest present, and stays a single GLOBAL tick (not
			   per-slot); it is the backstop that makes HALT-idle race-free. Only the SDL
			   backend calls it, so the headless deterministic suite never fires it. */
			void on_display_refresh();

		private:
			/* One registration-table entry. Each slot owns its own pixel base and capture
			   frame, so switching to a backgrounded slot later shows its own current content. */
			struct slot_state {
				u_word base {0};
				bool claimed {false};
				bool present_valid {false};
				bool rejected {false};   // STATUS bit2: last claim on this slot was rejected
				std::uint64_t present_count {0};
				std::vector<std::uint32_t> frame;
			};

			bool present_into(int slot, bool bump);   // read the slot's guest buffer into its frame
			/* Switch-back / activation core. All three assume activate_mutex_ is held. */
			void activate_locked(int new_slot);
			void claim_locked(int slot, u_word base);
			void release_locked(int slot);

			device_port width_port_;
			device_port height_port_;
			device_port format_port_;
			device_port base_port_;
			device_port present_port_;
			device_port status_port_;
			device_port slot_port_;
			device_port activate_port_;

			u_hword width_;
			u_hword height_;
			u_word pixel_count_;

			std::vector<slot_state> slots_;   // fixed fb_max_slots entries, built in the ctor
			int selected_slot_ {0};           // $56: CPU-thread-only (port ops are supervisor)
			/* active_slot_ is read lock-free by the display thread every frame; the compound
			   activate / claim / release transitions that write it (and touch the history and
			   the claimed flags the hotkey reads) are serialized by activate_mutex_, since the
			   CPU thread (port writes) and the SDL thread (VT hotkey) both drive activation. */
			std::atomic<int> active_slot_ {-1};   // -1 = text console
			std::vector<int> activation_history_; // most-recent-last; -1 or a slot index
			std::mutex activate_mutex_;
			std::atomic<int> claimed_count_ {0};
			std::atomic<bool> claim_attempted_ {false};   // any nonzero-base write on an unclaimed slot

			/* control_/status_ are the GLOBAL vsync registers, touched by both the guest
			   thread (port_write/port_read) and the display thread (on_display_refresh), so
			   they are atomic. Vsync stays global (one monitor), not per-slot (Section 6). */
			std::atomic<u_word> control_ {0};   // bit1 = vsync-IRQ-enable
			std::atomic<u_word> status_ {0};    // bit0 = vsync-pending
			std::atomic<std::uint64_t> present_count_ {0};   // valid presents across all slots
			std::atomic<bool> stop_on_claim_ {false};        // maize-221: console build stops the VM on a rejected claim
			std::atomic<bool> display_available_ {true};     // maize-236: false rejects claims (per-exec)
			std::vector<std::uint32_t> empty_frame_;         // returned by frame() when the console is active
		};

		/* maize-140: host/VM text console (approach (i)). The window becomes a first-class
		   glass TTY: the guest writes BYTES on fd 1/2 and this device renders GLYPHS into a
		   host pixel buffer (an 80x25 cell grid over font8x16 at a 640x400 default, distinct
		   from the framebuffer's 320x200 DOOM default), interpreting the same VT/ANSI output
		   subset the maize-121 term_core engine honors (LF/CR/BS/HT, CUU/CUD/CUF/CUB, CUP,
		   ED, EL, SGR basic colors, right-margin wrap, bottom-of-screen scroll). Physical
		   keys arrive as Set-1 scancodes, run through a shift/ctrl-aware keymap (printable +
		   control bytes + arrow/nav VT INPUT escapes) and a full termios line discipline
		   (cooked line editing with echo + Backspace, or raw byte-at-a-time with no echo),
		   and are delivered on fd 0. A blocking fd-0 read with no input parks the CPU on the
		   run-bit substrate (windowed) or on host stdin (headless), never busy-spinning.

		   It is NOT a port device: it is bound to fd 0/1/2 through sys.cpp's console_io seam
		   (maize.cpp installs it when the window console is active), so ordinary stdio guest
		   programs work with zero special wiring. State is resident (survives across guest
		   program runs within one VM), unlike term_core's per-program static state. */
		class text_console : public console::console_io {
		public:
			/* The scancode source. STDIN is the headless deterministic path (each host
			   stdin byte is one Set-1 scancode, mirroring `maize --input=keyboard`); QUEUE
			   is the windowed path (the SDL thread feeds scancodes via push_scancode). */
			enum class input_mode { STDIN, QUEUE };

			text_console(unsigned width_px, unsigned height_px);

			void set_input_mode(input_mode m) { input_mode_ = m; }

			/* --- console_io (fd 0/1/2 + termios, called from sys.cpp on the CPU thread) --- */
			void write_out(const unsigned char* buf, unsigned long count) override;
			long read_in(unsigned char* buf, unsigned long count) override;
			void termios_get(unsigned char* image) override;
			void termios_set(const unsigned char* image) override;

			/* --- windowed backend hooks (called from the SDL thread) --- */
			/* Enqueue one Set-1 scancode (make or break) and wake a parked fd-0 read. */
			void push_scancode(u_byte scancode);
			/* Unblock any parked read and make further reads return EOF (window closing). */
			void stop();

			/* --- display blit + headless verification --- */
			unsigned width() const { return width_px_; }
			unsigned height() const { return height_px_; }
			const std::uint32_t* pixels() const { return pixels_.data(); }

			/* SYS $F6 ttysize: cell-grid geometry, distinct from width()/height() above
			   which are PIXEL dimensions for the blit. cols_/rows_ are clamped >=1 at
			   construction and bounded to 20..500 / 10..200 by --console-size, so the
			   narrowing to unsigned short is always in range. */
			void get_size(unsigned short* rows, unsigned short* cols) const override {
				*rows = static_cast<unsigned short>(rows_);
				*cols = static_cast<unsigned short>(cols_);
			}

			/* Cursor state for the present-time overlay (maize-140). The display thread
			   reads these once per frame to place the blinking block; the CPU thread mutates
			   row_/col_ through the VT engine. They are shared lock-free exactly like the
			   pixels_ buffer above (which SDL_UpdateTexture reads while write_out mutates
			   it): a torn read only misplaces the cosmetic cursor for a single blink frame
			   and self-corrects. cursor_visible_ tracks the guest's ESC[?25h/l show/hide. */
			int cursor_row() const { return row_; }
			int cursor_col() const { return col_; }
			bool cursor_visible() const { return cursor_visible_; }

			/* Monotonic count of console content renders (write_out flushes + echoed
			   keystrokes), the console-mode analog of framebuffer_device::present_count().
			   The --show-perf sampler reads it as the FPS source while the text console owns
			   the window, so typing / output shows a small non-zero rate and an idle prompt
			   shows 0. Atomic to match present_count_'s cross-thread discipline. */
			std::uint64_t render_count() const { return render_count_.load(std::memory_order_relaxed); }

			/* Dump the grid as text rows (trailing blanks trimmed) for the headless CI
			   self-check, which cannot read the host pixel buffer from guest C. */
			void dump_text(std::ostream& out) const;

		private:
			/* VT-output engine (ported from demos/terminal/term_core.h). */
			void render_cell(int row, int col);
			int cur_attr() const { return (bg_ << 4) | fg_; }
			void clear_cell(int r, int c);
			void scroll_up();
			void newline();
			void put_glyph(int ch);
			void csi_dispatch(int final_byte);
			void out_byte(int ch);   // one output byte through the VT parser

			/* keymap + line discipline (the new INPUT half). */
			void keymap(u_byte sc, std::string& out) const;   // scancode -> raw byte(s)
			void feed_scancode(u_byte sc);                     // keymap + line discipline + echo
			void deliver(unsigned char b) { delivered_.push_back(b); }
			bool canonical() const { return (lflag_ & console::TERMIOS_ICANON) != 0; }
			bool echo() const { return (lflag_ & console::TERMIOS_ECHO) != 0; }
			void reset_termios_defaults();

			/* Headless stdin scancode source: one blocking host-stdin byte, or -1 at EOF. */
			int next_stdin_scancode();
			/* Windowed scancode source: pop one queued scancode, parking the CPU while the
			   queue is empty; returns -1 if the window closed with nothing pending. */
			int next_queue_scancode();

			unsigned width_px_;
			unsigned height_px_;
			int cols_;
			int rows_;
			std::vector<std::uint32_t> pixels_;   // width_px_ * height_px_, XRGB8888
			std::vector<unsigned char> glyph_;    // cols_*rows_
			std::vector<unsigned char> attr_;     // cols_*rows_
			int row_ {0};
			int col_ {0};
			/* DEC deferred wrap (VT100 auto-margin, maize-172): writing a glyph to the
			   last column does NOT immediately advance the cursor; it arms wrap_pending_,
			   and the wrap happens only when the NEXT glyph is written. An editor
			   (kilo) that fills the whole last row (its status bar spans every column)
			   relies on this so a full-width bottom line does not scroll the screen. Any
			   explicit cursor move (CR, LF, BS, TAB, or a CUP/CU[UDFB] escape) cancels
			   the pending wrap. */
			bool wrap_pending_ {false};
			int fg_ {7};
			int bg_ {0};
			int pstate_ {0};
			int param_[8] {0};
			int pidx_ {0};
			int pseen_ {0};
			int priv_ {0};                 // CSI private-mode intro seen ('?'), e.g. ESC[?25h/l
			bool cursor_visible_ {true};   // guest ESC[?25l hides the overlay cursor; ESC[?25h shows

			/* Count of content renders (write_out flushes + echoed keystrokes); the FPS
			   source for --show-perf while the console owns the window. See render_count(). */
			std::atomic<std::uint64_t> render_count_ {0};

			/* keyboard modifier state (Set-1). */
			bool shift_ {false};
			bool ctrl_ {false};
			bool caps_ {false};            // Caps Lock latch (0x3A make toggles); affects letters only

			/* termios + line discipline. */
			unsigned iflag_ {0};
			unsigned oflag_ {0};
			unsigned cflag_ {0};
			unsigned lflag_ {0};
			unsigned char cc_[console::TERMIOS_NCCS] {0};
			std::string line_;                    // pending cooked line (raw typed bytes)
			std::deque<unsigned char> delivered_; // bytes ready for fd-0 read

			/* windowed scancode queue (SDL thread produces, CPU thread consumes). */
			input_mode input_mode_ {input_mode::STDIN};
			std::mutex q_mutex_;
			std::condition_variable q_cv_;
			std::deque<u_byte> scancode_q_;
			bool stopped_ {false};
			bool stdin_eof_ {false};
		};

#ifdef MAIZE_DISPLAY
		/* SDL2 window backend (opt-in build, MAIZE_DISPLAY=ON). Runs the guest on a
		   background thread while the SDL event loop pumps on the calling thread, blitting
		   the active surface (the text console by default, or the framebuffer once a guest
		   graphics program claims it) and mapping host keys to Set-1 scancodes. Never
		   compiled in the default/headless build. */
		namespace display {
			/* refresh_hz is the "monitor" refresh rate: the cadence at which the window is
			   re-presented and the vsync IRQ is raised (on_display_refresh). Lower it for
			   undemanding workloads (fewer wakeups), raise it for smoother pacing. con is the
			   default surface (text console); fb takes over for a raw-framebuffer program
			   (DOOM) once it claims the framebuffer, mutually exclusive per run. */
			void run(framebuffer_device& fb, keyboard_device& kbd, text_console& con,
				unsigned scale, bool show_perf, unsigned refresh_hz, bool pause_on_halt,
				bool vsync);

			/* maize-226: page a block of text (the --help/usage output) in the window like
			   more(1). The guest never runs on the --help path, so this opens its own window,
			   renders the text into `con` a page at a time (page size = console rows minus the
			   prompt row), and reads keys directly from SDL: Space/Enter/PageDown advance, q or
			   Esc quits, and the final page is held until a key or window-close (the auto
			   pause-on-halt the card asks for). Returns true if it displayed; false if SDL video
			   was unavailable, so the caller can fall back to a plain-text dump. */
			bool show_help_paged(text_console& con, const std::string& text,
				unsigned scale, bool vsync);
		}
#endif

	} // namespace devices
} // namespace maize
