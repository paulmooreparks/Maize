/* maize-264: Windows backend for the presenter transport (CreateFileMapping +
   MapViewOfFile + CreateProcess). Guarded on _WIN32 so it compiles to nothing on POSIX
   (mirroring the hostfs backend convention). The platform-independent protocol logic
   lives in presenter_transport_core.cpp.

   The mapping is pagefile-backed (CreateFileMapping on INVALID_HANDLE_VALUE) under a
   named object in the "Local\" namespace, so the session and its child presenter share
   it by name. A pagefile-backed section is destroyed once every handle to it closes, so
   Windows needs no explicit unlink step: teardown just CloseHandles. */

#include "presenter_transport.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <string>

namespace maize {
namespace presenter_transport {

namespace {
    std::string g_argv0;
    std::string g_obj_name;          // "Local\\mzpt-<id>"
    HANDLE      g_map_handle = nullptr;
    void*       g_view = nullptr;
    std::size_t g_size = 0;
    mapped_segment g_seg{};
    HANDLE      g_child_proc = nullptr;   // the session's OWN most-recently-spawned child (reaping only)
    bool        g_created = false;
    bool        g_torn_down = false;

    std::string obj_name_for(const std::string& session_id) {
        return std::string("Local\\mzpt-") + session_id;
    }

    std::size_t segment_size(std::uint32_t width, std::uint32_t height, std::uint32_t max_slots) {
        return kFramesOffset + static_cast<std::size_t>(max_slots)
             * static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    }

    /* Resolve maizeg.exe next to argv0; empty string means "let CreateProcess search". */
    std::string maizeg_beside_argv0() {
        if (g_argv0.empty()) { return std::string(); }
        std::string::size_type slash = g_argv0.find_last_of("/\\");
        if (slash == std::string::npos) { return std::string(); }
        std::string cand = g_argv0.substr(0, slash + 1) + "maizeg.exe";
        DWORD attrs = GetFileAttributesA(cand.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            return cand;
        }
        return std::string();
    }
}

std::uint32_t self_pid() {
    return static_cast<std::uint32_t>(GetCurrentProcessId());
}

void set_argv0(const char* argv0) {
    if (argv0) { g_argv0 = argv0; }
}

mapped_segment create_segment(const std::string& session_id, std::uint32_t width, std::uint32_t height,
                              std::uint32_t format, std::uint32_t max_slots) {
    mapped_segment seg{};
    std::string name = obj_name_for(session_id);
    std::size_t size = segment_size(width, height, max_slots);
    DWORD hi = static_cast<DWORD>((static_cast<std::uint64_t>(size) >> 32) & 0xFFFFFFFFu);
    DWORD lo = static_cast<DWORD>(size & 0xFFFFFFFFu);
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, hi, lo, name.c_str());
    if (h == nullptr) { return seg; }
    /* The random session token makes a pre-existing object a genuine error, not reuse. */
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        return seg;
    }
    void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (view == nullptr) {
        CloseHandle(h);
        return seg;
    }
    /* A fresh pagefile-backed section is zero-filled, so every atomic already reads 0. */
    control_block* c = reinterpret_cast<control_block*>(view);
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
    seg.frames_base = reinterpret_cast<std::uint32_t*>(static_cast<char*>(view) + kFramesOffset);

    g_obj_name = name;
    g_map_handle = h;
    g_view = view;
    g_size = size;
    g_seg = seg;
    g_created = true;
    g_torn_down = false;
    return seg;
}

mapped_segment open_segment(const std::string& session_id) {
    mapped_segment seg{};
    std::string name = obj_name_for(session_id);
    HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    if (h == nullptr) { return seg; }
    /* Map the whole section (size 0 = to end); the fixed page offset locates the frames. */
    void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (view == nullptr) {
        CloseHandle(h);
        return seg;
    }
    control_block* c = reinterpret_cast<control_block*>(view);
    if (c->magic != kMagic || c->version != kVersion) {
        UnmapViewOfFile(view);
        CloseHandle(h);
        return seg;
    }
    seg.ctl = c;
    seg.frames_base = reinterpret_cast<std::uint32_t*>(static_cast<char*>(view) + kFramesOffset);

    g_obj_name = name;
    g_map_handle = h;
    g_view = view;
    g_size = 0;
    g_seg = seg;
    g_created = false;
    g_torn_down = false;
    return seg;
}

bool spawn_presenter(const std::string& session_id, unsigned display_scale, unsigned refresh_hz,
                     std::string* error_out) {
    reap_spawned_child();

    std::string beside = maizeg_beside_argv0();
    std::string exe = beside.empty() ? std::string("maizeg.exe") : beside;
    /* Command line: quote the program path, then the flags. */
    std::string cmd = "\"" + exe + "\" --presenter " + session_id
        + " --display-scale " + std::to_string(display_scale)
        + " --refresh-hz " + std::to_string(refresh_hz);
    if (spawn_show_perf()) { cmd += " --show-perf"; }   /* maize-267 overlay forward */

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::string mutable_cmd = cmd;   // CreateProcessA may modify the command-line buffer
    BOOL ok = CreateProcessA(
        beside.empty() ? nullptr : exe.c_str(),
        &mutable_cmd[0],
        nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        if (error_out) { *error_out = "CreateProcess maizeg failed (" + std::to_string(GetLastError()) + ")"; }
        return false;
    }
    CloseHandle(pi.hThread);
    g_child_proc = pi.hProcess;
    return true;
}

void reap_spawned_child() {
    if (g_child_proc != nullptr) {
        if (WaitForSingleObject(g_child_proc, 0) == WAIT_OBJECT_0) {
            CloseHandle(g_child_proc);
            g_child_proc = nullptr;
        }
    }
}

void teardown(mapped_segment& seg) {
    if (g_torn_down) { return; }
    g_torn_down = true;
    if (seg.ctl) {
        seg.ctl->shutdown.store(1, std::memory_order_release);
    }
    if (g_child_proc != nullptr) {
        /* Grace-wait for the presenter to notice shutdown and exit, then force it. */
        if (WaitForSingleObject(g_child_proc, 500) != WAIT_OBJECT_0) {
            TerminateProcess(g_child_proc, 0);
            WaitForSingleObject(g_child_proc, 2000);
        }
        CloseHandle(g_child_proc);
        g_child_proc = nullptr;
    }
    if (g_view) {
        UnmapViewOfFile(g_view);
        g_view = nullptr;
    }
    if (g_map_handle) {
        CloseHandle(g_map_handle);   // last handle close destroys the pagefile section
        g_map_handle = nullptr;
    }
    seg.ctl = nullptr;
    seg.frames_base = nullptr;
    g_seg = mapped_segment{};
}

void teardown_if_active() {
    if (g_torn_down) { return; }
    if (!g_created && g_view == nullptr) { return; }
    g_torn_down = true;
    if (g_seg.ctl) {
        g_seg.ctl->shutdown.store(1, std::memory_order_release);
    }
    if (g_child_proc != nullptr) {
        TerminateProcess(g_child_proc, 0);
        CloseHandle(g_child_proc);
        g_child_proc = nullptr;
    }
    if (g_map_handle) {
        CloseHandle(g_map_handle);
        g_map_handle = nullptr;
    }
}

}  // namespace presenter_transport
}  // namespace maize

#endif  // _WIN32
