/* maize-264: POSIX backend for the presenter transport (shm_open + mmap + posix_spawn).
   Guarded on __linux__ so it compiles to nothing on Windows (mirroring the hostfs
   backend convention). The platform-independent protocol logic lives in
   presenter_transport_core.cpp. */

#include "presenter_transport.h"

#ifdef __linux__

#include <atomic>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

extern char** environ;

namespace maize {
namespace presenter_transport {

namespace {
    std::string g_argv0;
    std::string g_shm_name;          // POSIX shm name ("/mzpt-<id>")
    int         g_fd = -1;
    void*       g_map = nullptr;
    std::size_t g_size = 0;
    mapped_segment g_seg{};
    pid_t       g_child_pid = -1;    // the session's OWN most-recently-spawned child (reaping only)
    bool        g_created = false;   // this process created (owns) the segment
    bool        g_torn_down = false;

    std::string shm_name_for(const std::string& session_id) {
        return std::string("/mzpt-") + session_id;
    }

    void sleep_ms(int ms) {
        struct timespec ts;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
        nanosleep(&ts, nullptr);
    }

    std::size_t segment_size(std::uint32_t width, std::uint32_t height, std::uint32_t max_slots) {
        return kFramesOffset + static_cast<std::size_t>(max_slots)
             * static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    }

    /* Resolve maizeg next to argv0 (same install dir); returns an empty string to signal
       "fall back to PATH via posix_spawnp". */
    std::string maizeg_beside_argv0() {
        if (g_argv0.empty()) { return std::string(); }
        std::string::size_type slash = g_argv0.find_last_of('/');
        if (slash == std::string::npos) { return std::string(); }
        std::string cand = g_argv0.substr(0, slash + 1) + "maizeg";
        if (access(cand.c_str(), X_OK) == 0) { return cand; }
        return std::string();
    }
}

std::uint32_t self_pid() {
    return static_cast<std::uint32_t>(getpid());
}

void set_argv0(const char* argv0) {
    if (argv0) { g_argv0 = argv0; }
}

mapped_segment create_segment(const std::string& session_id, std::uint32_t width, std::uint32_t height,
                              std::uint32_t format, std::uint32_t max_slots) {
    mapped_segment seg{};
    std::string name = shm_name_for(session_id);
    /* O_EXCL: the random session token makes a collision a genuine error, not stale reuse. */
    int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) { return seg; }
    std::size_t size = segment_size(width, height, max_slots);
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        close(fd);
        shm_unlink(name.c_str());
        return seg;
    }
    void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        shm_unlink(name.c_str());
        return seg;
    }
    /* Fresh mapping is zero-filled (ftruncate), so every atomic already reads 0. Set the
       header + the non-zero initial states (active_slot=-1, pending_activate sentinel). */
    control_block* c = reinterpret_cast<control_block*>(map);
    c->magic = kMagic;
    c->version = kVersion;
    c->width = width;
    c->height = height;
    c->format = format;
    c->max_slots = max_slots;
    c->active_slot.store(kConsoleSentinel, std::memory_order_relaxed);
    c->present_sequence.store(0, std::memory_order_relaxed);
    c->pending_activate.store(kNoPendingActivate, std::memory_order_relaxed);
    c->presenter_present.store(0, std::memory_order_relaxed);
    c->shutdown.store(0, std::memory_order_relaxed);
    c->presenter_pid.store(0, std::memory_order_relaxed);
    c->presenter_heartbeat.store(0, std::memory_order_relaxed);
    c->input_head.store(0, std::memory_order_relaxed);
    c->input_tail.store(0, std::memory_order_release);

    seg.ctl = c;
    seg.frames_base = reinterpret_cast<std::uint32_t*>(static_cast<char*>(map) + kFramesOffset);

    g_shm_name = name;
    g_fd = fd;
    g_map = map;
    g_size = size;
    g_seg = seg;
    g_created = true;
    g_torn_down = false;
    return seg;
}

mapped_segment open_segment(const std::string& session_id) {
    mapped_segment seg{};
    std::string name = shm_name_for(session_id);
    int fd = shm_open(name.c_str(), O_RDWR, 0600);
    if (fd < 0) { return seg; }
    struct stat st;
    if (fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) < kFramesOffset) {
        close(fd);
        return seg;
    }
    std::size_t size = static_cast<std::size_t>(st.st_size);
    void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return seg;
    }
    control_block* c = reinterpret_cast<control_block*>(map);
    if (c->magic != kMagic || c->version != kVersion) {
        munmap(map, size);
        close(fd);
        return seg;
    }
    seg.ctl = c;
    seg.frames_base = reinterpret_cast<std::uint32_t*>(static_cast<char*>(map) + kFramesOffset);
    /* The presenter tracks its own mapping so a later munmap can be honored, but never
       unlinks (only the session that created the segment unlinks it). */
    g_shm_name.clear();
    g_fd = fd;
    g_map = map;
    g_size = size;
    g_seg = seg;
    g_created = false;
    g_torn_down = false;
    return seg;
}

bool spawn_presenter(const std::string& session_id, unsigned display_scale, unsigned refresh_hz,
                     std::string* error_out) {
    /* Reap a previous tracked child first so a respawn does not leak a zombie. */
    reap_spawned_child();

    std::string beside = maizeg_beside_argv0();
    std::string scale_str = std::to_string(display_scale);
    std::string hz_str = std::to_string(refresh_hz);
    const char* prog = beside.empty() ? "maizeg" : beside.c_str();

    // posix_spawn takes a mutable argv; build char* copies.
    std::string a_presenter = "--presenter";
    std::string a_scale = "--display-scale";
    std::string a_hz = "--refresh-hz";
    char* argv[] = {
        const_cast<char*>(prog),
        const_cast<char*>(a_presenter.c_str()),
        const_cast<char*>(session_id.c_str()),
        const_cast<char*>(a_scale.c_str()),
        const_cast<char*>(scale_str.c_str()),
        const_cast<char*>(a_hz.c_str()),
        const_cast<char*>(hz_str.c_str()),
        nullptr
    };

    pid_t pid = -1;
    int rc;
    if (beside.empty()) {
        rc = posix_spawnp(&pid, prog, nullptr, nullptr, argv, environ);
    } else {
        rc = posix_spawn(&pid, prog, nullptr, nullptr, argv, environ);
    }
    if (rc != 0) {
        if (error_out) { *error_out = std::string("posix_spawn maizeg failed: ") + std::strerror(rc); }
        return false;
    }
    g_child_pid = pid;
    return true;
}

void reap_spawned_child() {
    if (g_child_pid > 0) {
        int status = 0;
        pid_t w = waitpid(g_child_pid, &status, WNOHANG);
        if (w == g_child_pid || w < 0) { g_child_pid = -1; }
    }
}

void teardown(mapped_segment& seg) {
    if (g_torn_down) { return; }
    g_torn_down = true;
    if (seg.ctl) {
        seg.ctl->shutdown.store(1, std::memory_order_release);
    }
    /* Grace-wait for the presenter's poll loop to notice shutdown and exit on its own. */
    if (g_child_pid > 0) {
        for (int waited = 0; waited < 500; waited += kPresenterLinkTickMs) {
            int status = 0;
            pid_t w = waitpid(g_child_pid, &status, WNOHANG);
            if (w == g_child_pid) { g_child_pid = -1; break; }
            sleep_ms(kPresenterLinkTickMs);
        }
        if (g_child_pid > 0) {
            kill(g_child_pid, SIGTERM);
            int status = 0;
            waitpid(g_child_pid, &status, 0);
            g_child_pid = -1;
        }
    }
    if (g_map) {
        munmap(g_map, g_size);
        g_map = nullptr;
    }
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    if (g_created && !g_shm_name.empty()) {
        shm_unlink(g_shm_name.c_str());
        g_shm_name.clear();
    }
    seg.ctl = nullptr;
    seg.frames_base = nullptr;
    g_seg = mapped_segment{};
}

void teardown_if_active() {
    /* Crash/signal path (host_tty restore hook). No-op if no segment was ever created,
       or if a clean teardown already ran. Kept minimal: signal the presenter, kill the
       tracked child, unlink the name so no /dev/shm entry or orphan process survives. */
    if (g_torn_down) { return; }
    if (!g_created && g_map == nullptr) { return; }
    g_torn_down = true;
    if (g_seg.ctl) {
        g_seg.ctl->shutdown.store(1, std::memory_order_release);
    }
    if (g_child_pid > 0) {
        kill(g_child_pid, SIGTERM);
        g_child_pid = -1;
    }
    if (g_created && !g_shm_name.empty()) {
        shm_unlink(g_shm_name.c_str());
        g_shm_name.clear();
    }
    /* Deliberately do NOT munmap here: a signal handler running mid-fault should not
       risk touching the mapping again; the process is ending and the kernel reclaims it. */
}

}  // namespace presenter_transport
}  // namespace maize

#endif  // __linux__
