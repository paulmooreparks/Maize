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

add_executable(maize "src/maize.cpp" "src/cpu.cpp" "src/sys.cpp" ${MAIZE_HOSTFS_SOURCES})
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