#pragma once

/* Host-backed standard device models (device-plugin API). These are the compile-time,
   statically-linked device models attached in maize.cpp: the console, the keyboard, and
   the memory-backed framebuffer. Each is built on the `device` base's on_port_write /
   on_port_read hooks (a device that spans several ports registers one small proxy per
   port, exactly as the timer registers its three registers). The headless model needs no
   external dependency; the optional SDL2 window backend sits behind MAIZE_DISPLAY. */

#include "maize_cpu.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
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
		class console_device : public cpu::input_device, public port_host {
		public:
			enum role { ROLE_DATA, ROLE_STATUS };

			console_device();
			void attach();
			void on_input_tick() override;

			void port_write(int role, cpu::reg_value const& value, cpu::subreg_enum value_subreg) override;
			cpu::reg_value port_read(int role, cpu::subreg_enum dst_subreg) override;

		private:
			device_port data_port_;
			device_port status_port_;
			u_word data_byte_ {0};
			bool available_ {false};
			bool exhausted_ {false};
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
			device_port data_port_;
			device_port status_port_;
			u_word scancode_ {0};
			bool available_ {false};
			bool exhausted_ {false};
			bool window_source_ {false};
			std::mutex queue_mutex_;
			std::deque<u_byte> queue_;
			/* Lock-free fast path for on_input_tick, which runs once per executed guest
			   instruction: taking queue_mutex_ every instruction just to test emptiness
			   dominates the windowed hot loop. This mirrors queue_.size() (updated under
			   the lock) so the common no-key-pending tick returns without locking. */
			std::atomic<size_t> queue_size_ {0};
		};

		/* Memory-backed framebuffer (0x50-0x55 control ports). Pixels live in ordinary
		   guest RAM: the guest writes them with normal ST/CP stores at full speed, with no
		   per-pixel device dispatch and NO MMIO (the pixel memory has no device side
		   effects). The control plane is ports:

		     0x50 width      R   host config (pixels)
		     0x51 height     R   host config (pixels)
		     0x52 format     R   host config (1 = XRGB8888, 0x00RRGGBB)
		     0x53 base       R/W guest address of the pixel buffer
		     0x54 present    W   present a frame; R bit0 = last present valid
		     0x55 status     R   bit0 vsync-pending; W bit1 vsync-IRQ-enable / ack

		   On a present write the device reads [base, base + width*height*4) from guest
		   memory (the sparse array; every address is defined, so no host OOB) and presents
		   it (headless: capture only; SDL2 windowed: blit). The buffer size is bounded by
		   the host-fixed resolution, never guest-controlled, so there is no unbounded
		   allocation. A base of 0 (unset) or a base+size that would wrap the address space
		   is an invalid present (defined, non-crashing; last-present-valid reads 0). */
		class framebuffer_device : public port_host {
		public:
			enum role { ROLE_WIDTH, ROLE_HEIGHT, ROLE_FORMAT, ROLE_BASE, ROLE_PRESENT, ROLE_STATUS };

			framebuffer_device(u_hword width, u_hword height);
			void attach();

			void port_write(int role, cpu::reg_value const& value, cpu::subreg_enum value_subreg) override;
			cpu::reg_value port_read(int role, cpu::subreg_enum dst_subreg) override;

			u_hword width() const { return width_; }
			u_hword height() const { return height_; }
			/* Host-side captured frame (XRGB8888), width*height entries. Filled on a valid
			   present; the SDL2 backend blits from here. */
			const std::vector<std::uint32_t>& frame() const { return frame_; }
			bool last_present_valid() const { return present_valid_; }
			/* Monotonic count of valid presents (guest frames produced). The SDL backend
			   samples the delta over time to show the guest frame rate. */
			std::uint64_t present_count() const { return present_count_.load(std::memory_order_relaxed); }

		private:
			bool present_frame();   // read the guest buffer into frame_; returns validity

			device_port width_port_;
			device_port height_port_;
			device_port format_port_;
			device_port base_port_;
			device_port present_port_;
			device_port status_port_;

			u_hword width_;
			u_hword height_;
			u_word pixel_count_;
			u_word base_ {0};
			u_word control_ {0};   // bit1 = vsync-IRQ-enable
			u_word status_ {0};    // bit0 = vsync-pending
			bool present_valid_ {false};
			std::atomic<std::uint64_t> present_count_ {0};   // valid presents (guest frames)
			std::vector<std::uint32_t> frame_;
		};

#ifdef MAIZE_DISPLAY
		/* SDL2 window backend (opt-in build, MAIZE_DISPLAY=ON). Runs the guest on a
		   background thread while the SDL event loop pumps on the calling thread, blitting
		   the framebuffer's captured frame and mapping host keys to Set-1 scancodes pushed
		   into the keyboard. Never compiled in the default/headless build. */
		namespace display {
			void run(framebuffer_device& fb, keyboard_device& kbd, unsigned scale, bool show_fps);
		}
#endif

	} // namespace devices
} // namespace maize
