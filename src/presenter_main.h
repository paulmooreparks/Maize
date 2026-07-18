#pragma once

/* maize-264: the presenter role. `maizeg --presenter <session-id>` branches main() here,
   skipping all VM/image-load setup. Under MAIZE_DISPLAY it opens an SDL window and renders
   the session's active framebuffer slot; without MAIZE_DISPLAY (the headless CI maizeg) it
   is a checksum stub that drives every transport fixture. Compiled into `maizeg` only;
   `maize` is never a presenter. */

#include <string>

namespace maize {
namespace presenter_main {

/* Attach to the session's shared segment, claim single ownership, and run the present
   loop (SDL window or headless checksum stub). Returns a process exit code: 0 on a clean
   shutdown, nonzero if the segment could not be opened or ownership could not be claimed
   (a live presenter already holds it). */
int run(const std::string& session_id, unsigned display_scale, unsigned refresh_hz);

}  // namespace presenter_main
}  // namespace maize
