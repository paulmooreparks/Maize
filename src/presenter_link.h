#pragma once

/* maize-264: the session-side glue between the shared-memory transport and the local
   framebuffer_device / keyboard_device. Two responsibilities:

   1. ensure_presenter(): the launch-or-attach hook the framebuffer's ROLE_BASE claim
      check calls (Decision D8, inside activate_mutex_). It consults the shared
      single-owner authority (presenter_owner_alive) and spawns a presenter only when
      none is live; a manually-reattached or auto-respawned presenter is recognized as
      alive and NOT double-spawned (OQ 9437 fix).

   2. A background thread (started once at segment creation) that, every
      kPresenterLinkTickMs (10 ms): drains the input FIFO into keyboard_device, applies
      the presenter's pending_activate requests through framebuffer_device, and (D16)
      watches presenter liveness, auto-respawning a dead presenter under a respawn-storm
      guard. This is the SAME thread and tick for all three jobs, no new thread. */

#include <string>

namespace maize {
namespace devices {
    class framebuffer_device;
    class keyboard_device;
}

namespace presenter_transport { struct mapped_segment; }

namespace presenter_link {

/* Launch-or-attach: returns true once a live presenter is confirmed (already alive, or
   freshly spawned and ready), false if that cannot be achieved within the budget. Safe
   to call repeatedly; a live presenter is a no-op. Runs on the CPU thread under the
   framebuffer's activate_mutex_. */
bool ensure_presenter(presenter_transport::mapped_segment& seg, const std::string& session_id,
                      unsigned display_scale, unsigned refresh_hz);

/* Start the background input-drain / pending_activate / liveness-watch thread. Records
   the devices + spawn parameters for the D16 auto-respawn path. Idempotent. */
void start(presenter_transport::mapped_segment& seg, devices::framebuffer_device& fb,
           devices::keyboard_device& kbd, const std::string& session_id,
           unsigned display_scale, unsigned refresh_hz);

/* Stop and join the background thread. Called at teardown before the segment is unmapped. */
void stop();

}  // namespace presenter_link
}  // namespace maize
