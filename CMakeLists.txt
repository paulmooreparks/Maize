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
# memory-backed framebuffer). It is linked into `maize` only; mazm never attaches devices.
add_executable(maize "src/maize.cpp" "src/cpu.cpp" "src/sys.cpp" "src/devices.cpp" ${MAIZE_HOSTFS_SOURCES})
add_executable(mazm "src/mazm.cpp" "src/cpu.cpp" "src/sys.cpp" ${MAIZE_HOSTFS_SOURCES})
add_executable(mzld "src/mzld.cpp")
add_executable(mzdis "src/mzdis.cpp")

find_package(Threads REQUIRED)
target_link_libraries(maize PRIVATE Threads::Threads)
target_link_libraries(mazm PRIVATE Threads::Threads)

if (CMAKE_VERSION VERSION_GREATER 3.12)
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
  foreach(_t maize mazm mzld mzdis)
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
    set_property(TARGET maize PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  endif()

  if (MAIZE_ARCH AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    target_compile_options(maize PRIVATE "$<$<CONFIG:Release>:-march=${MAIZE_ARCH}>")
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
  target_compile_definitions(maize PRIVATE MAIZE_DISPLAY)
  target_link_libraries(maize PRIVATE SDL2::SDL2)
endif()