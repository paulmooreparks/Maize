#pragma once

/* maize-140: the abstraction seam between the SYS read/write/termios dispatch
   (src/sys.cpp, compiled into BOTH maize and mazm) and the host text console
   device (src/devices.cpp, linked into maize only). sys.cpp holds a console_io*
   that is null in mazm and in any maize run without a bound console (host stdio
   then handles fd 0/1/2 exactly as before); maize.cpp installs the text_console
   when the window console is active. Keeping the interface here (not in devices.h)
   means sys.cpp never pulls in the device models, so mazm still links with no
   console code.

   The termios wire layout is frozen here so the host device and the guest libc
   (toolchain/rt/termios.h) agree byte for byte. It mirrors the Linux struct
   termios field order (four 32-bit flag words then the control array) so a later
   editor port (kilo, maize-172) that was written against Linux termios needs no
   field surgery. */

#include <cstddef>

namespace maize {
	namespace console {

		/* Frozen termios wire image: four little-endian 32-bit flag words followed
		   by NCCS control bytes. Total TERMIOS_SIZE bytes, copied verbatim between
		   guest memory and the host console's termios state by SYS $F1 / $F2. */
		constexpr std::size_t TERMIOS_NCCS = 20;
		constexpr std::size_t TERMIOS_SIZE = 16 + TERMIOS_NCCS;   // 4*4 flag words + cc[]

		/* Field byte offsets within the wire image. */
		constexpr std::size_t TERMIOS_OFF_IFLAG = 0;
		constexpr std::size_t TERMIOS_OFF_OFLAG = 4;
		constexpr std::size_t TERMIOS_OFF_CFLAG = 8;
		constexpr std::size_t TERMIOS_OFF_LFLAG = 12;
		constexpr std::size_t TERMIOS_OFF_CC = 16;

		/* c_cc indices (Linux values, the subset the console honors plus room to grow). */
		constexpr unsigned TERMIOS_VERASE = 2;   // erase char (default 0x7F)
		constexpr unsigned TERMIOS_VEOF = 4;     // end-of-file char (default 0x04)
		constexpr unsigned TERMIOS_VTIME = 5;    // read timeout, tenths (unused; VMIN-only)
		constexpr unsigned TERMIOS_VMIN = 6;     // min bytes for a raw read (default 1)

		/* c_lflag bits (Linux values). */
		constexpr unsigned TERMIOS_ISIG = 0x0001;
		constexpr unsigned TERMIOS_ICANON = 0x0002;
		constexpr unsigned TERMIOS_ECHO = 0x0008;

		/* c_iflag bit (Linux value): map input CR to NL (cooked default). */
		constexpr unsigned TERMIOS_ICRNL = 0x0100;

		/* c_oflag bits (Linux values): output post-processing / map NL to CR-NL. */
		constexpr unsigned TERMIOS_OPOST = 0x0001;
		constexpr unsigned TERMIOS_ONLCR = 0x0004;

		/* The console seen by sys.cpp. Buffers are host-side: sys.cpp marshals the
		   guest <-> host copy (mirroring the existing sys_read / sys_write shape), so
		   this interface never touches guest memory itself. */
		class console_io {
		public:
			virtual ~console_io() = default;

			/* fd 1 / fd 2: render a run of bytes through the VT-output engine. */
			virtual void write_out(const unsigned char* buf, unsigned long count) = 0;

			/* fd 0: block until the line discipline can deliver at least one byte
			   (cooked: a full line on Enter; raw: each byte as it arrives, VMIN=1),
			   copy up to count bytes
			   into buf, and return the number delivered. Returns 0 on end of input
			   (host stdin EOF in the headless source, or window close). Never busy-
			   spins: it parks the CPU (windowed) or blocks on host stdin (headless). */
			virtual long read_in(unsigned char* buf, unsigned long count) = 0;

			/* SYS $F1 tcgetattr: copy the current TERMIOS_SIZE-byte image out. */
			virtual void termios_get(unsigned char* image) = 0;
			/* SYS $F2 tcsetattr: adopt the TERMIOS_SIZE-byte image. */
			virtual void termios_set(const unsigned char* image) = 0;
		};

	} // namespace console
} // namespace maize
