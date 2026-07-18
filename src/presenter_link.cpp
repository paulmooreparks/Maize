/* maize-264: session-side presenter_link. See presenter_link.h. */

#include "presenter_link.h"
#include "presenter_transport.h"
#include "devices.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

namespace maize {
namespace presenter_link {

namespace pt = presenter_transport;

namespace {
    /* One session per process, so a file-scope singleton holds the link state. */
    struct link_state {
        pt::mapped_segment* seg {nullptr};
        devices::framebuffer_device* fb {nullptr};
        devices::keyboard_device* kbd {nullptr};
        std::string session_id;
        unsigned scale {3};
        unsigned hz {60};

        std::thread thread;
        std::atomic<bool> running {false};
        std::atomic<bool> started {false};

        /* Watcher-local staleness state (NOT shared memory), per D16. */
        bool     have_hb_sample {false};
        std::uint64_t last_hb {0};
        std::chrono::steady_clock::time_point last_change {};

        /* Respawn-storm guard: a rolling list of attempt timestamps + a once-only hint. */
        std::deque<std::chrono::steady_clock::time_point> respawn_times;
        bool hint_printed {false};
    };
    link_state g_link;

    using clock = std::chrono::steady_clock;

    int ms_since(clock::time_point t) {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now() - t).count());
    }

    /* Apply one pending_activate request from the presenter (VT hotkey), consuming it. */
    void drain_pending_activate(pt::mapped_segment& seg, devices::framebuffer_device& fb) {
        std::int32_t target = seg.ctl->pending_activate.exchange(pt::kNoPendingActivate,
                                                                 std::memory_order_acq_rel);
        if (target == pt::kNoPendingActivate) { return; }
        if (target == pt::kConsoleSentinel) {
            fb.request_activate(cpu::fb_console_sentinel);
        } else if (target >= 0) {
            fb.request_activate(static_cast<maize::u_word>(target));
        }
    }

    /* Non-blocking, incremental staleness check (D16): a re-implementation of
       presenter_owner_alive's sample-twice logic that keeps last-seen heartbeat + its
       last-change time across ticks, so it never sleeps the drain thread. */
    bool presenter_alive_incremental(pt::mapped_segment& seg) {
        std::uint32_t pid = seg.ctl->presenter_pid.load(std::memory_order_acquire);
        std::uint64_t hb = seg.ctl->presenter_heartbeat.load(std::memory_order_acquire);
        clock::time_point now = clock::now();
        if (!g_link.have_hb_sample || hb != g_link.last_hb) {
            g_link.last_hb = hb;
            g_link.last_change = now;
            g_link.have_hb_sample = true;
        }
        /* A graceful release (pid==0) is dead instantly; otherwise dead once the
           heartbeat has not advanced for longer than kStaleTimeoutMs. */
        if (pid == 0) { return false; }
        return ms_since(g_link.last_change) <= pt::kStaleTimeoutMs;
    }

    void clear_storm_guard() {
        if (g_link.hint_printed || !g_link.respawn_times.empty()) {
            g_link.respawn_times.clear();
            g_link.hint_printed = false;
        }
    }

    /* Try one auto-respawn under the storm guard. Returns nothing; updates guard state. */
    void attempt_respawn(pt::mapped_segment& seg) {
        clock::time_point now = clock::now();
        /* Prune attempts older than the rolling window. */
        while (!g_link.respawn_times.empty()
               && std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - g_link.respawn_times.front()).count() > pt::kRespawnWindowMs) {
            g_link.respawn_times.pop_front();
        }
        if (static_cast<int>(g_link.respawn_times.size()) >= pt::kRespawnMaxAttempts) {
            if (!g_link.hint_printed) {
                std::cerr << "maize: presenter keeps exiting; auto-respawn paused. "
                             "Run `maizeg --presenter " << g_link.session_id
                          << "` to reattach manually." << std::endl;
                g_link.hint_printed = true;
            }
            return;
        }
        g_link.respawn_times.push_back(now);
        /* Reset readiness so wait_presenter_ready waits for the NEW presenter, not a
           stale flag from the dead one. */
        seg.ctl->presenter_present.store(0, std::memory_order_release);
        std::string err;
        if (pt::spawn_presenter(g_link.session_id, g_link.scale, g_link.hz, &err)) {
            if (pt::wait_presenter_ready(seg, pt::kRegisterTimeoutMs)) {
                clear_storm_guard();
                g_link.have_hb_sample = false;   // re-baseline against the new heartbeat
            }
        }
    }

    void watch_tick(pt::mapped_segment& seg, devices::framebuffer_device& fb) {
        /* Watching is only meaningful while a slot is claimed (a presenter SHOULD exist). */
        if (!fb.graphics_claimed()) {
            g_link.have_hb_sample = false;
            return;
        }
        if (presenter_alive_incremental(seg)) {
            /* Healthy: a presenter this session did not necessarily spawn (a manual
               reattach) is alive, so clear any tripped storm guard (D16 reset rule b). */
            clear_storm_guard();
        } else {
            attempt_respawn(seg);
        }
    }

    void link_loop() {
        pt::mapped_segment& seg = *g_link.seg;
        devices::framebuffer_device& fb = *g_link.fb;
        devices::keyboard_device& kbd = *g_link.kbd;
        while (g_link.running.load(std::memory_order_acquire)) {
            if (seg.ctl) {
                /* 1. Drain the input FIFO into the keyboard device. */
                std::uint8_t sc;
                while (pt::pop_input_scancode(seg, &sc)) {
                    kbd.push_event(sc);
                }
                /* 2. Apply the presenter's pending VT-activation request. */
                drain_pending_activate(seg, fb);
                /* 3. (D16) Auto-respawn watch. */
                watch_tick(seg, fb);
                /* Reap a dead child promptly so respawns do not leak a zombie. */
                pt::reap_spawned_child();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(pt::kPresenterLinkTickMs));
        }
    }
}  // namespace

bool ensure_presenter(pt::mapped_segment& seg, const std::string& session_id,
                      unsigned display_scale, unsigned refresh_hz) {
    if (!seg.ctl) { return false; }
    /* Consult the shared single-owner authority first (OQ 9437 fix). It short-circuits
       to false immediately on a cold presenter_pid==0 (D15 secondary fix), so the common
       first-registration path does not burn the stale-sample window. */
    if (pt::presenter_owner_alive(seg, pt::kStaleTimeoutMs)) {
        return true;   // already live (spawned by us, manually reattached, or auto-respawned)
    }
    /* No live presenter: spawn one and wait for it to claim ownership + become ready. */
    seg.ctl->presenter_present.store(0, std::memory_order_release);
    std::string err;
    if (!pt::spawn_presenter(session_id, display_scale, refresh_hz, &err)) {
        std::cerr << "maize: could not launch presenter: " << err << std::endl;
        return false;
    }
    return pt::wait_presenter_ready(seg, pt::kRegisterTimeoutMs);
}

void start(pt::mapped_segment& seg, devices::framebuffer_device& fb,
           devices::keyboard_device& kbd, const std::string& session_id,
           unsigned display_scale, unsigned refresh_hz) {
    if (g_link.started.exchange(true)) { return; }
    /* The presenter (its input ring) is the session's keyboard source now, so drain the
       ring through the same push_event/queue path the SDL window backend uses. */
    kbd.use_window_source();
    g_link.seg = &seg;
    g_link.fb = &fb;
    g_link.kbd = &kbd;
    g_link.session_id = session_id;
    g_link.scale = display_scale;
    g_link.hz = refresh_hz;
    g_link.running.store(true, std::memory_order_release);
    g_link.thread = std::thread(link_loop);
}

void stop() {
    if (!g_link.started.load()) { return; }
    g_link.running.store(false, std::memory_order_release);
    if (g_link.thread.joinable()) { g_link.thread.join(); }
    g_link.started.store(false);
}

}  // namespace presenter_link
}  // namespace maize
