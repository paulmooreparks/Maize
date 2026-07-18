/* maize-264: platform-independent presenter-transport logic. The OS-specific segment
   creation/mapping/spawn/teardown lives in presenter_transport_posix.cpp /
   presenter_transport_win32.cpp (each internally guarded so the non-matching one
   compiles to nothing), mirroring the hostfs_core.c / hostfs_posix.c / hostfs_win32.c
   split. Everything here operates purely on an already-mapped control_block, so it is
   identical on every platform: the single-owner protocol (D14/D15), the polled doorbell
   readiness wait, and the input FIFO ring (D5). */

#include "presenter_transport.h"

#include <chrono>
#include <random>
#include <thread>

namespace maize {
namespace presenter_transport {

namespace {
    void sleep_ms(int ms) {
        if (ms > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
    }
}

std::string new_session_id() {
    /* Per-process RNG seeded once. A random 8-hex token (not a PID; PIDs recycle) so the
       O_CREAT|O_EXCL create never legitimately collides and no stale-reuse logic is
       needed. */
    static std::mt19937 rng([] {
        std::random_device rd;
        std::uint64_t seed = static_cast<std::uint64_t>(rd())
            ^ (static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) << 1)
            ^ (static_cast<std::uint64_t>(self_pid()) << 32);
        return std::mt19937(static_cast<std::mt19937::result_type>(seed));
    }());
    std::uniform_int_distribution<std::uint32_t> dist;
    std::uint32_t v = dist(rng);
    char buf[9];
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        buf[7 - i] = hex[(v >> (i * 4)) & 0xF];
    }
    buf[8] = '\0';
    return std::string(buf);
}

bool wait_presenter_ready(mapped_segment& seg, int timeout_ms) {
    if (!seg.ctl) { return false; }
    int waited = 0;
    while (waited < timeout_ms) {
        if (seg.ctl->presenter_present.load(std::memory_order_acquire) != 0) { return true; }
        sleep_ms(kPresenterLinkTickMs);
        waited += kPresenterLinkTickMs;
    }
    return seg.ctl->presenter_present.load(std::memory_order_acquire) != 0;
}

bool presenter_owner_alive(mapped_segment& seg, int sample_window_ms) {
    if (!seg.ctl) { return false; }
    control_block* c = seg.ctl;
    /* D15 secondary fix: a cold start (presenter_pid==0) is definitively no-owner, so
       short-circuit instead of burning the whole sample window out of the caller's
       registration budget. */
    if (c->presenter_pid.load(std::memory_order_acquire) == 0) { return false; }
    std::uint64_t hb0 = c->presenter_heartbeat.load(std::memory_order_acquire);
    sleep_ms(sample_window_ms);
    std::uint32_t pid1 = c->presenter_pid.load(std::memory_order_acquire);
    std::uint64_t hb1 = c->presenter_heartbeat.load(std::memory_order_acquire);
    return pid1 != 0 && hb1 != hb0;
}

bool claim_ownership(mapped_segment& seg) {
    if (!seg.ctl) { return false; }
    control_block* c = seg.ctl;
    std::uint32_t me = self_pid();
    /* Two attempts: the second covers losing a steal race to a third presenter (or an
       owner that gracefully released to 0 during our stale-sample sleep). */
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::uint32_t expected = 0;
        if (c->presenter_pid.compare_exchange_strong(expected, me,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;   // won a cold/released segment, no stale wait
        }
        std::uint32_t owner = expected;   // the pid currently holding ownership
        std::uint64_t hb0 = c->presenter_heartbeat.load(std::memory_order_acquire);
        sleep_ms(kStaleTimeoutMs);
        std::uint64_t hb1 = c->presenter_heartbeat.load(std::memory_order_acquire);
        if (hb1 != hb0) {
            return false;   // a live owner advanced its heartbeat: already attached
        }
        /* Stale: the previous owner never cleared presenter_pid (a crash, not a clean
           exit). Attempt exactly one steal CAS from the observed owner to this pid. */
        std::uint32_t exp = owner;
        if (c->presenter_pid.compare_exchange_strong(exp, me,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;   // stole a dead owner's slot
        }
        // Lost the steal race to a third presenter; loop once to re-check.
    }
    return false;
}

void mark_presenter_ready(mapped_segment& seg) {
    if (!seg.ctl) { return; }
    seg.ctl->presenter_present.store(1, std::memory_order_release);
}

bool bump_heartbeat(mapped_segment& seg) {
    if (!seg.ctl) { return false; }
    control_block* c = seg.ctl;
    /* Read-check-write (D15): verify this process still owns the segment BEFORE bumping.
       The instant presenter_pid stops matching (a steal fired against us), return false
       without writing; the caller must self-terminate immediately. */
    if (c->presenter_pid.load(std::memory_order_acquire) != self_pid()) { return false; }
    c->presenter_heartbeat.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

void release_ownership_if_owner(mapped_segment& seg) {
    if (!seg.ctl) { return; }
    /* Best-effort graceful release: CAS own-pid -> 0. A no-op if it has already been
       stolen (the expected pid no longer matches). NEVER called on the D15
       steal-detected path (that presenter must not clear a pid it no longer owns). */
    std::uint32_t exp = self_pid();
    seg.ctl->presenter_pid.compare_exchange_strong(exp, 0u,
        std::memory_order_acq_rel, std::memory_order_acquire);
}

void push_input_scancode(mapped_segment& seg, std::uint8_t sc) {
    if (!seg.ctl) { return; }
    control_block* c = seg.ctl;
    std::uint32_t head = c->input_head.load(std::memory_order_relaxed);
    std::uint32_t tail = c->input_tail.load(std::memory_order_acquire);
    std::uint32_t next = (head + 1) % static_cast<std::uint32_t>(kInputRingCapacity);
    if (next == tail) {
        /* Ring full: drop the oldest entry (documented, matches a real keyboard buffer
           overrun; only on pathological overflow, since input is a true FIFO otherwise). */
        c->input_tail.store((tail + 1) % static_cast<std::uint32_t>(kInputRingCapacity),
                            std::memory_order_release);
    }
    c->input_ring[head] = sc;
    c->input_head.store(next, std::memory_order_release);
}

bool pop_input_scancode(mapped_segment& seg, std::uint8_t* out) {
    if (!seg.ctl || !out) { return false; }
    control_block* c = seg.ctl;
    std::uint32_t tail = c->input_tail.load(std::memory_order_relaxed);
    std::uint32_t head = c->input_head.load(std::memory_order_acquire);
    if (tail == head) { return false; }   // empty
    *out = c->input_ring[tail];
    c->input_tail.store((tail + 1) % static_cast<std::uint32_t>(kInputRingCapacity),
                        std::memory_order_release);
    return true;
}

}  // namespace presenter_transport
}  // namespace maize
