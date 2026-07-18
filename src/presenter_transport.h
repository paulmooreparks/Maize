#pragma once

/* maize-264: cross-process presentation transport (workbench doc 18, session-host
   doctrine). This header is the SINGLE source of truth for the shared-memory layout
   that the session process (the terminal-primary `maize` binary) and the presenter
   process (`maizeg --presenter`) both map. It is host-only plumbing: no guest-visible
   port, syscall, opcode, or ABI is touched.

   The session (client) side CREATES one named segment per session and relocates the
   framebuffer_device's per-slot capture buffers plus a control block into it. The
   presenter (server) side OPENS the same segment by name, renders the active slot's
   frames, and pushes input scancodes + VT-hotkey activation requests back through the
   control block. A polled present_sequence counter is the doorbell (latest-frame-wins,
   Decision D4); a presenter_pid + presenter_heartbeat pair is the single-owner liveness
   authority (D14/D15); the input ring is a true FIFO (D5).

   The struct is mapped by both processes, which are the same binary family built by the
   same compiler on the same architecture, so the std::atomic members have identical
   layout on both sides (a trusted same-user child, not an isolation boundary, D11). */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace maize {
namespace presenter_transport {

constexpr std::uint32_t kMagic = 0x4d5a5054;       // 'MZPT', sanity check on attach
constexpr std::uint32_t kVersion = 2;              // presenter_pid/presenter_heartbeat added (cycle 2/3)
constexpr std::size_t   kInputRingCapacity = 256;  // Set-1 scancodes, presenter producer / session consumer
constexpr std::int32_t  kConsoleSentinel = -1;     // matches cpu::fb_console_sentinel's meaning
constexpr std::int32_t  kNoPendingActivate = -2;   // distinct from a legal slot index or kConsoleSentinel
constexpr int kHeartbeatIntervalMs = 250;          // owning presenter bumps presenter_heartbeat this often; also the read-check-write tick (D15)
constexpr int kStaleTimeoutMs = 1000;              // no advance across this window => owner presumed dead
constexpr int kRegisterTimeoutMs = 3000;           // overall registration-time launch/attach budget (D8)
constexpr int kPresenterLinkTickMs = 10;           // presenter_link poll cadence; the D16 watcher rides this same tick
constexpr int kRespawnMaxAttempts = 3;             // storm guard (D16): at most this many respawn attempts per window
constexpr int kRespawnWindowMs = 30000;            // storm guard (D16): the rolling window kRespawnMaxAttempts is measured over

/* Frame regions start one page after the segment base. sizeof(control_block) is a few
   hundred bytes (well under a page), so a fixed page offset keeps both sides' frame
   addressing deterministic without either re-deriving a rounded control size. */
constexpr std::size_t kFramesOffset = 4096;

struct control_block {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t format;                            // mirrors cpu::fb_format_xrgb8888
    std::uint32_t max_slots;
    std::atomic<std::int32_t>  active_slot;          // -1 = console; session writes, presenter reads
    std::atomic<std::uint64_t> present_sequence;     // mirrors framebuffer_device::present_count(); presenter polls
    std::atomic<std::int32_t>  pending_activate;     // presenter writes a hotkey-requested target; session consumes -> kNoPendingActivate
    std::atomic<std::uint8_t>  presenter_present;    // set by whichever presenter WINS ownership, once ready; readiness only, not liveness
    std::atomic<std::uint8_t>  shutdown;             // session sets 1 at teardown; presenter's poll loop notices and exits
    std::atomic<std::uint32_t> presenter_pid;        // 0 = unclaimed; else the owning presenter's pid (single-owner CAS)
    std::atomic<std::uint64_t> presenter_heartbeat;  // monotonic counter the OWNING presenter bumps every kHeartbeatIntervalMs
    std::atomic<std::uint32_t> input_head;           // ring producer index (presenter-owned)
    std::atomic<std::uint32_t> input_tail;           // ring consumer index (session-owned)
    std::uint8_t input_ring[kInputRingCapacity];     // Set-1 scancodes, same bytes push_event consumes today
};

static_assert(sizeof(control_block) <= kFramesOffset,
              "control_block must fit before the frame regions");

struct mapped_segment {
    control_block* ctl {nullptr};
    std::uint32_t* frames_base {nullptr};   // frames_base[slot*(width*height) + i]; nullptr on open failure
};

/* Generate a random 8-hex-digit session token (not a PID; PIDs recycle). Seeded per
   process at first use so O_CREAT|O_EXCL never legitimately collides. */
std::string new_session_id();

/* Record argv[0] so spawn_presenter can find `maizeg` next to the running binary. */
void set_argv0(const char* argv0);

// --- session (client) side ---
mapped_segment create_segment(const std::string& session_id, std::uint32_t width, std::uint32_t height,
                              std::uint32_t format, std::uint32_t max_slots);
bool spawn_presenter(const std::string& session_id, unsigned display_scale, unsigned refresh_hz,
                     std::string* error_out);
bool wait_presenter_ready(mapped_segment& seg, int timeout_ms);       // polls ctl->presenter_present
bool presenter_owner_alive(mapped_segment& seg, int sample_window_ms); // BLOCKING liveness check (registration time only)
void teardown(mapped_segment& seg);        // sets shutdown=1, grace-waits, force-kills the tracked child, unmaps + unlinks
void teardown_if_active();                 // crash/signal path: no-op if no segment was ever created (host_tty hook)
void reap_spawned_child();                 // non-blocking reap of the session's own dead child (avoid a POSIX zombie)

// --- presenter (server) side ---
mapped_segment open_segment(const std::string& session_id);   // ctl==nullptr on magic/version mismatch or not-found
bool claim_ownership(mapped_segment& seg);   // CAS presenter_pid 0 -> own pid; one stale-steal attempt on a dead owner
void mark_presenter_ready(mapped_segment& seg);
bool bump_heartbeat(mapped_segment& seg);    // read-check-write (D15): false (no bump) the instant presenter_pid stops matching
void release_ownership_if_owner(mapped_segment& seg);   // graceful-exit CAS own-pid -> 0; NEVER on the D15 steal-detected path
void push_input_scancode(mapped_segment& seg, std::uint8_t sc);   // producer; drops the oldest entry on ring overflow
bool pop_input_scancode(mapped_segment& seg, std::uint8_t* out);  // consumer

/* Shared frame addressing: the first pixel of `slot` in the mapped frame regions. */
inline std::uint32_t* frame_ptr(const mapped_segment& seg, int slot) {
    if (!seg.ctl || !seg.frames_base || slot < 0
        || static_cast<std::uint32_t>(slot) >= seg.ctl->max_slots) {
        return nullptr;
    }
    std::size_t pixels = static_cast<std::size_t>(seg.ctl->width) * seg.ctl->height;
    return seg.frames_base + static_cast<std::size_t>(slot) * pixels;
}

/* Cross-platform current-process id (uint32), for the single-owner CAS. */
std::uint32_t self_pid();

}  // namespace presenter_transport
}  // namespace maize
