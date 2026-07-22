/* maize-264: the presenter role. See presenter_main.h. */

#include "presenter_main.h"
#include "presenter_transport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#ifdef MAIZE_DISPLAY
#include <SDL.h>
#include "sdl_scancodes.h"   // shared map_scancode() (maize::devices::display)
#include "font8x16.h"        // maize-267: glyphs for the --show-perf overlay (per-TU static)
#endif

#include <csignal>   // sig_atomic_t is needed unconditionally (used outside the
                    // #ifndef _WIN32 signal()/handler block below); matches
                    // host_tty.cpp's unconditional <csignal> include.

namespace maize {
namespace presenter_main {

namespace pt = presenter_transport;

namespace {
    using clock = std::chrono::steady_clock;

    std::int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now().time_since_epoch()).count();
    }

    /* FNV-1a over the active slot's width*height*4 bytes: a cheap, deterministic frame
       fingerprint the fixtures assert against. */
    std::uint32_t fnv1a(const std::uint8_t* data, std::size_t n) {
        std::uint32_t h = 0x811c9dc5u;
        for (std::size_t i = 0; i < n; ++i) {
            h ^= data[i];
            h *= 0x01000193u;
        }
        return h;
    }

    /* Test-only knobs are FILE-triggered (the file path comes from an env var, the file's
       existence/content is toggled by the fixture at runtime). This lets a fixture make the
       FIRST presenter healthy but a later respawn/stealer not, which env flags (inherited by
       every child) cannot express. A one-shot knob unlinks the file so it fires exactly once
       (the original stub, not the stealer that replaces it); a persistent knob leaves it. */
    bool knob_file_present(const char* env_name, int* value_out, bool unlink_after) {
        const char* path = std::getenv(env_name);
        if (!path || !*path) { return false; }
        std::FILE* f = std::fopen(path, "r");
        if (!f) { return false; }
        int v = 0;
        if (std::fscanf(f, "%d", &v) != 1) { v = 0; }
        std::fclose(f);
        if (value_out) { *value_out = v; }
        if (unlink_after) { std::remove(path); }
        return true;
    }

    /* Shared open + single-owner claim + test knobs. Returns false (with the exit code in
       *exit_code) when the presenter must not run (open/claim failure or the die knob). */
    bool attach_and_claim(const std::string& session_id, pt::mapped_segment& seg, int* exit_code) {
        seg = pt::open_segment(session_id);
        if (!seg.ctl) {
            std::cerr << "maizeg --presenter: no session segment '" << session_id
                      << "' (not found or version mismatch)" << std::endl;
            *exit_code = 2;
            return false;
        }
        if (!pt::claim_ownership(seg)) {
            std::cerr << "maizeg --presenter: a presenter is already attached to session "
                      << session_id << "; exiting" << std::endl;
            *exit_code = 3;
            return false;
        }
        /* D16 storm-guard knob (persistent file): claim ownership then exit at once, without
           ever marking ready, bumping a heartbeat, or releasing ownership (leaves a stale pid
           for the watcher to steal, and never satisfies wait_presenter_ready so each respawn
           attempt counts against the storm guard). Persistent: every respawn dies while the
           file exists, so the guard exhausts. */
        if (knob_file_present("MAIZE_PRESENTER_DIE_FILE", nullptr, false)) {
            *exit_code = 0;
            return false;
        }
        return true;
    }
}

#ifndef MAIZE_DISPLAY
namespace {
    volatile sig_atomic_t g_stub_term = 0;   // unqualified, matching host_tty.cpp:35 g_synth_byte
#ifndef _WIN32
    void stub_sigterm(int) { g_stub_term = 1; }   // async-signal-safe flag write
#endif
}

// ---- Headless checksum stub (the fixture-facing presenter) --------------------------
int run(const std::string& session_id, unsigned /*scale*/, unsigned /*hz*/, bool /*show_perf*/) {
    pt::mapped_segment seg{};
    int exit_code = 0;
    if (!attach_and_claim(session_id, seg, &exit_code)) { return exit_code; }

#ifndef _WIN32
    /* AC 9411 instant-detection leg: a SIGTERM'd stub releases ownership gracefully so the
       session's watcher detects the death immediately (via release_ownership_if_owner)
       rather than waiting out kStaleTimeoutMs. The handler only sets a flag; the loop below
       does the release + exit. */
    std::signal(SIGTERM, stub_sigterm);
#endif

    pt::mark_presenter_ready(seg);
    std::fprintf(stdout, "presenter-stub: ready session=%s\n", session_id.c_str());
    std::fflush(stdout);

    /* D15 stall knob: for the first stall_ms after becoming ready, keep rendering
       checksums but do NOT bump the heartbeat, so a second presenter steals ownership;
       once the stall ends, the first resumed bump_heartbeat detects the pid mismatch and
       exits (no release, per the single-owner protocol). */
    /* One-shot file knobs (unlinked on read) so the stealer/respawn that replaces this stub
       does NOT re-trigger them: the stall applies to the first presenter only (the stealer
       stays healthy), and the injected scancode fires exactly once. */
    int stall_ms = 0;
    knob_file_present("MAIZE_PRESENTER_STALL_FILE", &stall_ms, true);
    int inject_sc = -1;
    if (!knob_file_present("MAIZE_PRESENTER_INJECT_FILE", &inject_sc, true)) { inject_sc = -1; }
    clock::time_point ready = clock::now();
    clock::time_point stall_end = ready + std::chrono::milliseconds(stall_ms);
    bool in_stall = (stall_ms > 0);
    clock::time_point next_hb = ready;
    bool injected = false;

    std::uint64_t last_seq = 0;
    const std::size_t frame_bytes =
        static_cast<std::size_t>(seg.ctl->width) * seg.ctl->height * 4u;

    for (;;) {
        if (seg.ctl->shutdown.load(std::memory_order_acquire) != 0 || g_stub_term != 0) {
            pt::release_ownership_if_owner(seg);
            break;
        }
        clock::time_point now = clock::now();
        if (in_stall && now >= stall_end) { in_stall = false; next_hb = now; }
        if (!in_stall && now >= next_hb) {
            if (!pt::bump_heartbeat(seg)) { break; }   // D15: stolen -> exit, NO release
            next_hb = now + std::chrono::milliseconds(pt::kHeartbeatIntervalMs);
        }

        /* Input-ring test knob: push one synthetic scancode into the ring so the input
           round-trip (ring -> session keyboard_device -> guest) is exercised headlessly. */
        if (inject_sc >= 0 && !injected) {
            pt::push_input_scancode(seg, static_cast<std::uint8_t>(inject_sc & 0xFF));
            injected = true;
        }

        std::uint64_t seq = seg.ctl->present_sequence.load(std::memory_order_acquire);
        if (seq != last_seq) {
            last_seq = seq;
            int slot = seg.ctl->active_slot.load(std::memory_order_acquire);
            std::uint32_t* frame = pt::frame_ptr(seg, slot);
            if (slot >= 0 && frame) {
                std::uint32_t csum = fnv1a(reinterpret_cast<const std::uint8_t*>(frame), frame_bytes);
                std::fprintf(stdout, "presenter-stub: slot=%d seq=%llu checksum=%08x t=%lld\n",
                             slot, static_cast<unsigned long long>(seq), csum,
                             static_cast<long long>(now_ms()));
                std::fflush(stdout);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pt::kPresenterLinkTickMs));
    }
    return 0;
}

#else
namespace {
    /* maize-267: the same fixed-width glyph blit devices.cpp's overlay uses (that copy is
       file-static there; the font arrays are per-TU statics, so a local twin is the
       cheapest correct reuse). Draw color must be set by the caller. */
    void draw_text(SDL_Renderer* ren, int x, int y, int px, const std::string& s) {
        for (char ch : s) {
            int c = static_cast<unsigned char>(ch);
            if (c >= FONT_FIRST && c <= FONT_LAST) {
                const unsigned char* g = font8x16[c - FONT_FIRST];
                for (int row = 0; row < FONT_H; ++row) {
                    unsigned char bits = g[row];
                    for (int col = 0; col < FONT_W; ++col) {
                        if (bits & (1 << col)) {
                            SDL_Rect r { x + col * px, y + row * px, px, px };
                            SDL_RenderFillRect(ren, &r);
                        }
                    }
                }
            }
            x += FONT_W * px;
        }
    }
}

// ---- SDL windowed presenter (operator-verified, AC 9417; not self-verified by Build) --
int run(const std::string& session_id, unsigned display_scale, unsigned refresh_hz,
        bool show_perf) {
    pt::mapped_segment seg{};
    int exit_code = 0;
    if (!attach_and_claim(session_id, seg, &exit_code)) { return exit_code; }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "maizeg --presenter: SDL video unavailable: " << SDL_GetError() << std::endl;
        pt::release_ownership_if_owner(seg);
        return 4;
    }
    unsigned scale = display_scale < 1 ? 1 : display_scale;
    int w = static_cast<int>(seg.ctl->width);
    int h = static_cast<int>(seg.ctl->height);
    SDL_Window* win = SDL_CreateWindow("Maize presenter",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        w * static_cast<int>(scale), h * static_cast<int>(scale), SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(ren, w, h);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);   // maize-267: overlay box alpha
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, w, h);

    pt::mark_presenter_ready(seg);

    /* maize-267 --show-perf overlay state: MIPS from the session-published instruction
       counter's deltas (ctl->instr_count, written every link tick), FPS from the guest
       present_sequence deltas, sampled on the same 500 ms cadence as maizeg's native
       overlay. The session's exit report stays the authoritative summary. */
    std::int64_t perf_last_ms = 0;
    std::uint64_t perf_last_instr = 0;
    std::uint64_t perf_last_seq = 0;
    std::string perf_str;
    std::string last_drawn_perf;

    unsigned rhz = refresh_hz < 1 ? 1 : (refresh_hz > 1000 ? 1000 : refresh_hz);
    unsigned refresh_ms = 1000u / rhz;
    if (refresh_ms < 1) { refresh_ms = 1; }

    clock::time_point next_hb = clock::now();
    std::uint64_t last_seq = 0;
    int last_slot = -2;
    bool running = true;

    while (running) {
        if (seg.ctl->shutdown.load(std::memory_order_acquire) != 0) {
            pt::release_ownership_if_owner(seg);
            break;
        }
        clock::time_point now = clock::now();
        if (now >= next_hb) {
            if (!pt::bump_heartbeat(seg)) { running = false; break; }   // D15: stolen, no release
            next_hb = now + std::chrono::milliseconds(pt::kHeartbeatIntervalMs);
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                pt::release_ownership_if_owner(seg);
                running = false;
                break;
            }
            if (e.type == SDL_KEYDOWN) {
                SDL_Scancode sc_k = e.key.keysym.scancode;
                bool vt_mods = (e.key.keysym.mod & KMOD_LCTRL) && (e.key.keysym.mod & KMOD_LALT);
                if (vt_mods && sc_k >= SDL_SCANCODE_F1 && sc_k <= SDL_SCANCODE_F9) {
                    /* VT hotkey: write the target into pending_activate rather than calling
                       request_activate (this process does not own the framebuffer_device). */
                    if (sc_k == SDL_SCANCODE_F1) {
                        seg.ctl->pending_activate.store(pt::kConsoleSentinel, std::memory_order_release);
                    } else {
                        seg.ctl->pending_activate.store(static_cast<std::int32_t>(sc_k - SDL_SCANCODE_F2),
                                                        std::memory_order_release);
                    }
                    continue;   // consumed
                }
                if (e.key.repeat == 0) {
                    maize::u_byte sc = devices::display::map_scancode(sc_k);
                    if (sc) { pt::push_input_scancode(seg, sc); }
                }
            } else if (e.type == SDL_KEYUP) {
                SDL_Scancode sc_k = e.key.keysym.scancode;
                if ((e.key.keysym.mod & KMOD_LCTRL) && (e.key.keysym.mod & KMOD_LALT)
                    && sc_k >= SDL_SCANCODE_F1 && sc_k <= SDL_SCANCODE_F9) {
                    continue;   // swallow the hotkey release too
                }
                maize::u_byte sc = devices::display::map_scancode(sc_k);
                if (sc) { pt::push_input_scancode(seg, static_cast<maize::u_byte>(sc | 0x80)); }
            }
        }
        if (!running) { break; }

        int slot = seg.ctl->active_slot.load(std::memory_order_acquire);
        std::uint64_t seq = seg.ctl->present_sequence.load(std::memory_order_acquire);

        if (show_perf) {
            std::int64_t nowms = now_ms();
            std::uint64_t ic = seg.ctl->instr_count.load(std::memory_order_relaxed);
            if (perf_last_ms == 0) {
                perf_last_ms = nowms;
                perf_last_instr = ic;
                perf_last_seq = seq;
            } else if (nowms - perf_last_ms >= 500) {
                std::int64_t dt = nowms - perf_last_ms;
                int mips = static_cast<int>((ic - perf_last_instr)
                    / (static_cast<std::uint64_t>(dt) * 1000ull));
                int fps = static_cast<int>((seq - perf_last_seq) * 1000ull
                    / static_cast<std::uint64_t>(dt));
                perf_str = "M" + std::to_string(mips) + " F" + std::to_string(fps);
                perf_last_ms = nowms;
                perf_last_instr = ic;
                perf_last_seq = seq;
            }
        }

        bool frame_changed = (slot != last_slot || seq != last_seq);
        bool perf_changed = show_perf && !perf_str.empty() && perf_str != last_drawn_perf;
        if (frame_changed || perf_changed) {
            last_slot = slot;
            last_seq = seq;
            std::uint32_t* frame = pt::frame_ptr(seg, slot);
            if (slot >= 0 && frame) {
                if (frame_changed) {
                    SDL_UpdateTexture(tex, nullptr, frame, w * 4);
                }
                /* On a perf-only tick the texture still holds the last frame. */
                SDL_RenderClear(ren);
                SDL_RenderCopy(ren, tex, nullptr, nullptr);
            } else {
                SDL_RenderClear(ren);   // console active: blank frame
            }
            if (show_perf && !perf_str.empty()) {
                /* Same top-left translucent box + green text as the native overlay. */
                int px = 1;
                int boxw = static_cast<int>(perf_str.size()) * FONT_W * px + 2;
                int boxh = FONT_H * px + 2;
                SDL_Rect box { 1, 1, boxw, boxh };
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 180);
                SDL_RenderFillRect(ren, &box);
                SDL_SetRenderDrawColor(ren, 0, 255, 0, 255);
                draw_text(ren, 2, 2, px, perf_str);
                last_drawn_perf = perf_str;
            }
            SDL_RenderPresent(ren);
        }
        SDL_Delay(refresh_ms);
    }

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
#endif

}  // namespace presenter_main
}  // namespace maize
