cmake_minimum_required(VERSION 3.21)
project(Maize)

set(CMAKE_CXX_STANDARD 20)

# maize-114: the freestanding-C99 hostfs core plus both host backends. src/sys.cpp
# (compiled into BOTH maize and mazm) calls the core; maize.cpp additionally calls the
# backend acquisition hooks. Both backend TUs are guarded (#ifdef __linux__ / _WIN32)
# so the non-matching one compiles to nothing; both are listed on both targets so the
# build is platform-agnostic. mazm never installs a mount table, so its hostfs paths
# stay inert (empty table).
set(MAIZE_HOSTFS_SOURCES
  "src/hostfs/hostfs_core.c"
  "src/hostfs/hostfs_posix.c"
  "src/hostfs/hostfs_win32.c")

# src/devices.cpp carries the host-backed standard device models (console, keyboard, the
# memory-backed framebuffer). Linked into the VM binaries only; mazm never attaches devices.
#
# maize-217/maize-230: two VM binaries from the same sources. `maizeg` is the graphical build
# (the SDL window backend, MAIZE_DISPLAY below); on Windows, linking SDL2main makes it a
# GUI-subsystem binary whose output is the window. `maize` is the console build: no SDL, so it
# stays a console-subsystem binary whose stdout/stderr reach the invoking terminal. Console
# programs (hello, quesOS) run under `maize`; the screen (framebuffer, DOOM) runs under `maizeg`.
# maize-264: the cross-process presentation transport (workbench doc 18 session-host
# doctrine). Three TUs mirror the hostfs core/posix/win32 split: a platform-independent
# core plus two backends, each internally #ifdef-guarded so the non-matching one compiles
# to nothing. presenter_link is the session-side background thread (input drain +
# pending_activate + the D16 auto-respawn watcher). All are SDL-free, so they link into
# the console `maize` binary too; only the presenter role itself (presenter_main.cpp,
# below) is maizeg-only. `maize` never runs as a presenter.
set(MAIZE_PRESENTER_SOURCES
  "src/presenter_transport_core.cpp"
  "src/presenter_transport_posix.cpp"
  "src/presenter_transport_win32.cpp"
  "src/presenter_link.cpp")
# maize-345: the Windows console readiness probe and its pre-read byte buffer. Guarded
# on _WIN32 like the hostfs and presenter backends, so it compiles to nothing on POSIX,
# and listed on BOTH targets that compile src/sys.cpp (the VM binaries below and mazm)
# because sys.cpp now calls into it and would otherwise fail to link.
set(MAIZE_CONSOLE_PROBE_SOURCES "src/console_probe_win32.cpp")
# maize-313: the execution-independent host stdin source, which raises IRQ 33 from its own
# thread so a HALT-parked CPU wakes. Two backends behind one seam, each internally
# #ifdef-guarded so the non-matching one compiles to nothing, following the same convention
# as the hostfs, presenter-transport and console-probe splits. Listed on BOTH targets that
# compile src/sys.cpp (the VM binaries below and mazm), because the readiness seam in
# sys.cpp now publishes the arm/ack acknowledgement and would otherwise fail to link. mazm
# never starts a source, so its copy stays inert.
set(MAIZE_STDIN_SOURCE_SOURCES "src/stdin_source_posix.cpp" "src/stdin_source_win32.cpp")
set(MAIZE_VM_SOURCES "src/maize.cpp" "src/cpu.cpp" "src/sys.cpp" "src/devices.cpp" "src/perf.cpp" "src/host_tty.cpp" ${MAIZE_CONSOLE_PROBE_SOURCES} ${MAIZE_STDIN_SOURCE_SOURCES} ${MAIZE_HOSTFS_SOURCES} ${MAIZE_PRESENTER_SOURCES})
add_executable(maizeg ${MAIZE_VM_SOURCES})
add_executable(maize  ${MAIZE_VM_SOURCES})

# maize-264: the presenter role (SDL window under MAIZE_DISPLAY, headless checksum stub
# otherwise) is compiled into `maizeg` only; the console `maize` binary is never a
# presenter, so it does not carry this TU.
target_sources(maizeg PRIVATE "src/presenter_main.cpp")

# maize-264: shm_open / posix_spawn on Linux. Modern glibc carries these in libc, but
# link librt explicitly so an older CI toolchain still resolves them.
if (UNIX AND NOT APPLE)
  target_link_libraries(maizeg PRIVATE rt)
  target_link_libraries(maize  PRIVATE rt)
endif()

# maize-217/230: the console binary `maize` is the console-only VM. This define makes maize.cpp ignore the
# graphical-only launcher knobs (display, input=keyboard scancode injection) that live in
# the shared ~/.maize/config for `maizeg`, so `maize prog.mzb` in a terminal is always a
# plain console program (host stdio) and never binds a windowed console or a tty injector.
# It is distinct from MAIZE_DISPLAY: the headless CI `maizeg` (no MAIZE_DISPLAY) still honors
# those knobs for the scancode-injection tests; only the console `maize` opts out.
target_compile_definitions(maize PRIVATE MAIZE_CONSOLE_ONLY)
add_executable(mazm "src/mazm.cpp" "src/cpu.cpp" "src/sys.cpp" "src/host_tty.cpp" ${MAIZE_CONSOLE_PROBE_SOURCES} ${MAIZE_STDIN_SOURCE_SOURCES} ${MAIZE_HOSTFS_SOURCES})
add_executable(mzld "src/mzld.cpp")
add_executable(mzdis "src/mzdis.cpp")

# maize-278: mzcc, the compiled cross-platform C guest-build driver (replaces the
# ctest / one-shot compile path of scripts/cc-maize.sh). C (C11), not C++
# (decision DI10: cproc compiles C only, so mzcc-in-C is the prerequisite for
# maize-277 self-hosting the driver, and C links only libc). The
# core/posix/win32 process-spawn split mirrors the presenter_transport seam; both
# backend TUs are listed here and each is internally #ifdef-guarded so the
# non-matching one compiles to nothing. No new external dependency (decision D5):
# links only libc + Threads (the stdin-pump thread).
add_executable(mzcc
  "src/mzcc.c"
  "src/mzcc_proc_core.c"
  "src/mzcc_proc_posix.c"
  "src/mzcc_proc_win32.c"
  "src/mzcc_fs.c"
  "src/mzcc_userland.c"
  "src/mzcc_demos.c"
  "src/mzcc_quesos.c"
  # maize-274: the content-addressed object cache (SHA-256 key + atomic store)
  # and the parallel TU scheduler (a worker pool over the mzcc_proc thread
  # primitives). No new external dependency: still libc + Threads.
  "src/mzcc_sha256.c"
  "src/mzcc_cache.c"
  "src/mzcc_sched.c")
set_property(TARGET mzcc PROPERTY C_STANDARD 11)

# maize-345: the Windows console test binary. It owns a console of its own (FreeConsole +
# AllocConsole + CONIN$) and injects input records into it, which is the only way anything
# in this tree reaches the interactive-console branch of the readiness probe: the pty
# keystroke leg is Linux-only and the Windows CI suite feeds a pipe. It links the console
# TU plus src/host_tty.cpp, because AC-14 drives the console input-mode mask through
# host_tty's own init/termios_set/restore rather than through a copy of it.
#
# A test, not a deliverable: deliberately absent from scripts/install-mazm.ps1,
# scripts/install-mazm.sh, the Ctrl+Shift+B task and the MAIZE_SANITIZE list below (it
# exercises Win32 console API behaviour, not VM code).
if (WIN32)
  add_executable(console_probe_test "src/console_probe_test_win32.cpp" "src/host_tty.cpp" ${MAIZE_CONSOLE_PROBE_SOURCES})
  set_property(TARGET console_probe_test PROPERTY CXX_STANDARD 20)
endif()


find_package(Threads REQUIRED)
target_link_libraries(maizeg PRIVATE Threads::Threads)
target_link_libraries(maize  PRIVATE Threads::Threads)
target_link_libraries(mazm PRIVATE Threads::Threads)
target_link_libraries(mzcc PRIVATE Threads::Threads)

# maize-313: the Windows direct-drive test for the host stdin source. Windows has no quesOS
# fixture, so the Windows-only claims that need no guest (the pipe watcher's raise latency,
# the disarmed state issuing no peek, the forced raise under a frozen level indicator, and
# the unconditional join) are taken here rather than inferred from the POSIX legs. It links
# the source TU plus src/host_tty.cpp, because a console source requires host_tty::active().
#
# A test, not a deliverable: deliberately absent from scripts/install-mazm.ps1,
# scripts/install-mazm.sh, the Ctrl+Shift+B task and the MAIZE_SANITIZE list below, for the
# same reason console_probe_test is.
if (WIN32)
  add_executable(stdin_source_test "src/stdin_source_test_win32.cpp" "src/host_tty.cpp" ${MAIZE_STDIN_SOURCE_SOURCES})
  set_property(TARGET stdin_source_test PROPERTY CXX_STANDARD 20)
  target_link_libraries(stdin_source_test PRIVATE Threads::Threads)
endif()

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET maizeg PROPERTY CXX_STANDARD 20)
  set_property(TARGET maize PROPERTY CXX_STANDARD 20)
  set_property(TARGET mazm PROPERTY CXX_STANDARD 20)
  set_property(TARGET mzld PROPERTY CXX_STANDARD 20)
  set_property(TARGET mzdis PROPERTY CXX_STANDARD 20)
endif()

# maize-81: opt-in AddressSanitizer + UndefinedBehaviorSanitizer build.
# Applied uniformly to all four tools so the existing suites run through
# instrumented binaries in CI. -fno-sanitize-recover=all is load-bearing: it
# makes a UBSan finding abort the process (nonzero exit) instead of logging and
# returning 0, which is what lets CI actually gate on UB.
option(MAIZE_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

if (MAIZE_SANITIZE)
  set(_maize_san_flags
    -fsanitize=address,undefined
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer)
  foreach(_t maizeg maize mazm mzld mzdis mzcc)
    target_compile_options(${_t} PRIVATE ${_maize_san_flags})
    target_link_options(${_t}    PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

# Performance flags for the VM hot path (the `maize` interpreter). Release-only and
# skipped under sanitizers. LTO lets the compiler inline across translation units (device
# port I/O in devices.cpp, syscalls in sys.cpp) into the cpu.cpp instruction loop; the
# memory hot path is already same-TU. MAIZE_ARCH raises the ISA baseline from generic
# x86-64 (SSE2) to x86-64-v3 (AVX2/BMI2, Haswell+ 2013) for better ALU/bit-op codegen;
# set it to "native", "x86-64-v2", or empty (to disable) as portability requires. These
# apply to `maize` only (the perf-critical target); the assembler/linker/disassembler
# are untouched.
set(MAIZE_ARCH "x86-64-v3" CACHE STRING "Release -march for the maize VM (empty to disable)")

if (NOT MAIZE_SANITIZE)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _maize_ipo_ok OUTPUT _maize_ipo_msg)
  if (_maize_ipo_ok)
    set_property(TARGET maizeg PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET maize PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  endif()

  if (MAIZE_ARCH AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    target_compile_options(maizeg PRIVATE "$<$<CONFIG:Release>:-march=${MAIZE_ARCH}>")
    target_compile_options(maize  PRIVATE "$<$<CONFIG:Release>:-march=${MAIZE_ARCH}>")
  endif()

  # maize-259: opt-in Clang PGO (Profile-Guided Optimization) for the interpreter hot
  # path, two-phase like Clang's own IR-based PGO ("-fprofile-generate"/"-fprofile-use",
  # distinct from Clang's source-based "-fprofile-instr-generate"). MAIZE_PGO=generate
  # builds an instrumented maize/maizeg that write raw profile counters under
  # MAIZE_PGO_DIR on exit; MAIZE_PGO=use rebuilds against the merged default.profdata in
  # that same directory. scripts/build-pgo.ps1 drives both phases plus the
  # llvm-profdata merge; the default (empty) build is unchanged either way. Clang-only:
  # GCC's profile format and merge tooling differ and are not wired here. Applies to
  # `maize`/`maizeg` only (the same perf-critical targets MAIZE_ARCH targets),
  # Release-only.
  set(MAIZE_PGO "" CACHE STRING "Clang PGO phase for the maize VM: empty (off), generate, or use")
  set(MAIZE_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-data" CACHE PATH "Directory holding PGO raw profiles (generate) or the merged default.profdata (use)")

  if (MAIZE_PGO)
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      message(WARNING "MAIZE_PGO='${MAIZE_PGO}' requested but the compiler is not Clang; ignoring.")
    elseif (MAIZE_PGO STREQUAL "generate")
      foreach(_t maizeg maize)
        target_compile_options(${_t} PRIVATE "$<$<CONFIG:Release>:-fprofile-generate=${MAIZE_PGO_DIR}>")
        target_link_options(${_t}    PRIVATE "$<$<CONFIG:Release>:-fprofile-generate=${MAIZE_PGO_DIR}>")
      endforeach()
    elseif (MAIZE_PGO STREQUAL "use")
      foreach(_t maizeg maize)
        target_compile_options(${_t} PRIVATE
          "$<$<CONFIG:Release>:-fprofile-use=${MAIZE_PGO_DIR}>"
          "$<$<CONFIG:Release>:-Wno-profile-instr-out-of-date>"
          "$<$<CONFIG:Release>:-Wno-profile-instr-unprofiled>")
        target_link_options(${_t} PRIVATE "$<$<CONFIG:Release>:-fprofile-use=${MAIZE_PGO_DIR}>")
      endforeach()
    else()
      message(FATAL_ERROR "MAIZE_PGO must be empty, 'generate', or 'use' (got '${MAIZE_PGO}')")
    endif()
  endif()
endif()

# Opt-in SDL2 window backend for the framebuffer + keyboard (device set). OFF by default:
# the default build, linux-debug, the Windows presets, and MAIZE_SANITIZE all configure,
# build, and link with NO new mandatory external dependency and run headless. SDL2 is
# found and linked ONLY when MAIZE_DISPLAY=ON, and only src/devices.cpp's guarded window
# backend references it; the headless device model has zero SDL dependency.
option(MAIZE_DISPLAY "Build the opt-in SDL2 host window backend (framebuffer + keyboard)" OFF)

if (MAIZE_DISPLAY)
  find_package(SDL2 REQUIRED)
  target_compile_definitions(maizeg PRIVATE MAIZE_DISPLAY)
  target_link_libraries(maizeg PRIVATE SDL2::SDL2)
endif()

# maize-376: scripts/run-ctest.sh's fixtures registered as real CTest tests, so `ctest -R`
# / `-L` select a subset, `ctest -jN` runs them in parallel, and each one carries its own
# TIMEOUT. Only registered for a top-level configure; a super-build embedding this project
# should not inherit the guest-toolchain suite.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MaizeCTest.cmake")
endif()