cmake_minimum_required(VERSION 3.21)
project(Maize)

# maize-439: refuse to configure when this project's own toolchain file was named for
# this build directory and did not run in it.
#
# cmake/ToolchainRoot.cmake stamps MAIZE_RESOLVED_TOOLCHAIN_DIR into the cache every
# time it runs, so the stamp's absence while the file is named is a contradiction: the
# resolver was asked for and did not answer, and nothing here resolved the compiler.
# The known cause is a build directory first configured under the OLD presets, which
# hardcoded the compiler into cacheVariables. CMake writes the include() of a toolchain
# file into CMakeFiles/<ver>/CMakeSystem.cmake at first configure and a directory
# without that line never gains one, so naming a toolchain file later is accepted,
# cached, and never acted on. Such a directory goes on building with the old in-repo
# compiler path, and fails with a bare "cannot find the file specified" the day that
# path goes away, which is the exact failure this card exists to remove.
#
# The condition tests the contradiction and the message states it. It does not narrate
# the cause, and that is deliberate (Convention counterexamples: a diagnostic that names
# a specific cause its own condition cannot distinguish, promoted from this card's own
# cycle-1 review). The earlier form fired on "WIN32 and a mingw-clang compiler and no
# stamp", which is equally true of a brand-new directory configured by hand with
# -DCMAKE_C_COMPILER=<the pinned clang> and no preset, and told that caller its correct
# compiler path "is whatever the old preset hardcoded". Keying on CMAKE_TOOLCHAIN_FILE
# instead excludes that caller by construction rather than by wording: a hand-rolled
# configure that names no toolchain file has asked for none, gets none, and is left
# alone. A hand-passed compiler that goes THROUGH a preset still reaches the toolchain
# file, where D-8's non-fatal WARNING handles it, so the two paths now agree about the
# same input instead of one warning and the other refusing.
#
# The name test keeps this off somebody else's toolchain file: a cross-compile file
# from outside this project would never write our stamp, and refusing it on every
# configure would be a guard firing on work it knows nothing about.
if(DEFINED CACHE{CMAKE_TOOLCHAIN_FILE}
   AND CMAKE_TOOLCHAIN_FILE MATCHES "ToolchainRoot\\.cmake$"
   AND NOT DEFINED CACHE{MAIZE_RESOLVED_TOOLCHAIN_DIR})
  message(FATAL_ERROR
    "cmake/ToolchainRoot.cmake is this build directory's toolchain file but did not "
    "run in it, so the compiler\n"
    "  ${CMAKE_C_COMPILER}\n"
    "was not resolved by the pinned-toolchain logic and this build would use whatever "
    "the cache already held. CMake only re-includes a toolchain file in a build "
    "directory that was FIRST configured with one, so this cannot be repaired in "
    "place. Either repair is equivalent and both cost one full reconfigure:\n"
    "  delete this build directory, then configure again:\n"
    "    ${CMAKE_BINARY_DIR}\n"
    "  or add --fresh to the same configure command (needs CMake 3.24 or newer; "
    "this project's floor is 3.21).")
endif()

set(CMAKE_CXX_STANDARD 20)

# maize-450: this build produces the Maize v2 machine and the v2 assembler, and nothing
# else. v1 is archived.
#
# Two operator rulings on 2026-08-13 set the shape. v1 gets no further development or
# testing, and quesOS along with several other v1 pieces will be PORTED to v2 rather than
# rewritten from nothing. So v1's sources, the quesOS tree and the .mazm corpus all stay
# exactly where they are, because they are what somebody reads while porting, and none of
# them is compiled, linked or run by this file any more. A file nobody builds costs
# nothing and answers questions later. A target everyone builds and a suite everyone runs
# cost minutes on every cycle and report failures nobody will act on.
#
# What left this file: the maize and maizeg VM binaries, the mazm assembler, mzld, mzdis,
# mzcc, the Windows-only console_probe_test and stdin_source_test, the source-set
# variables that fed them (hostfs, presenter transport, console probe, stdin source), the
# MAIZE_ARCH / interprocedural-optimization / MAIZE_PGO blocks that tuned v1's interpreter
# alone, and the include of cmake/MaizeCTest.cmake, which registered the 227-test v1
# guest-toolchain suite. Every one of those things is reachable in git history and builds
# today on the `v1` branch, which is where v1 is preserved and where it still has a test
# suite to be correct against.
#
# The v1 perf knobs are deliberately NOT re-pointed at mzvm. MAIZE_ARCH raised the ISA
# baseline to x86-64-v3, which is a portability decision for the v2 machine that nobody
# has made, and the committed PGO profiles were trained against v1's interpreter, so
# feeding them to a different binary would apply a profile that means nothing while
# looking like it means something. Tuning the v2 interpreter is its own card, with its
# own measurements.
#
# maize-422 decision D-1 gives mzld, mzdis and mzcc their v2 names in advance of their
# ports (maize-423 through 426). Today's builds of all three target the v1 machine, so
# they go with v1 and each comes back here as its port lands. mzcc in particular is what
# dragged the cproc and qbe cross-toolchain into every CI job, since its resolver requires
# both plus mazm, maize and mzld before it does any work at all.

# maize-418: the Maize v2 virtual machine.
#
# `mzvmg` is a build-target twin at this stage with no functional difference from `mzvm`:
# it exists so the naming lands once, and it has nothing to differ over until v2 grows a
# display device. The console device maize-451 landed is common to both, and the display
# device is maize-456. No SDL2 linkage is wired here, and none of v1's presenter machinery
# is ported into it speculatively.
set(MAIZE_V2_SOURCES "src/v2/decode_v2.cpp" "src/v2/interpreter_v2.cpp")
add_executable(mzvm  ${MAIZE_V2_SOURCES} "src/v2/mzvm_main.cpp")
add_executable(mzvmg ${MAIZE_V2_SOURCES} "src/v2/mzvm_main.cpp")
target_include_directories(mzvm  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/v2")
target_include_directories(mzvmg PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/v2")

# maize-456: each twin is told which one it is, because one source file compiled twice
# cannot work it out at runtime. argv[0] is not the answer: it carries whatever path the
# caller typed, it is empty when the caller says so, and a program that reports a name an
# operator can choose is reporting nothing. So the name is fixed here, per target, and
# every diagnostic either binary prints is built from it. Before this, `mzvmg --help`
# printed `usage: mzvm`, and each of the other fourteen things it can say named the twin.
target_compile_definitions(mzvm  PRIVATE MAIZE_V2_PROGRAM_NAME="mzvm")
target_compile_definitions(mzvmg PRIVATE MAIZE_V2_PROGRAM_NAME="mzvmg")

# maize-422: mzasm, the Maize v2 assembler. src/maize_obj.h is shared with v1's tools
# because the object format carries no instruction knowledge and D-3 extends it in place
# with a v2 version byte and one new relocation type.
set(MAIZE_MZASM_SOURCES
  "src/v2/mzasm_lexer.cpp"
  "src/v2/mzasm_assemble.cpp"
  "src/v2/mzasm_object.cpp")
add_executable(mzasm ${MAIZE_MZASM_SOURCES} "src/v2/mzasm_main.cpp")
target_include_directories(mzasm PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/v2")

# mzasm_lexer.cpp holds a thread_local, which needs the platform thread library on some
# toolchains even though nothing here starts a thread.
find_package(Threads REQUIRED)
target_link_libraries(mzasm PRIVATE Threads::Threads)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET mzvm PROPERTY CXX_STANDARD 20)
  set_property(TARGET mzvmg PROPERTY CXX_STANDARD 20)
  set_property(TARGET mzasm PROPERTY CXX_STANDARD 20)
endif()

# maize-81: opt-in AddressSanitizer + UndefinedBehaviorSanitizer build, so the suites run
# through instrumented binaries in CI. -fno-sanitize-recover=all is load-bearing: it makes
# a UBSan finding abort the process (nonzero exit) instead of logging and returning 0,
# which is what lets CI actually gate on UB. cmake/MaizeV2Fixtures.cmake reads
# _maize_san_flags to instrument the two test binaries the same way.
option(MAIZE_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

if (MAIZE_SANITIZE)
  set(_maize_san_flags
    -fsanitize=address,undefined
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer)
  foreach(_t mzvm mzvmg mzasm)
    target_compile_options(${_t} PRIVATE ${_maize_san_flags})
    target_link_options(${_t}    PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

# The SDL2 window backend. v1's maizeg carried it and no longer builds, and mzvmg has no
# display device to attach a window to yet (maize-456), so this option selects nothing. It
# stays declared rather than removed because both install scripts pass it explicitly on
# every configure (an unrecognized -D would become a warning about a manually-specified
# variable the project never used), and because mzvmg's port is where it comes back.
# find_package still runs when the option is on, so a caller asking for a display build
# on a machine without SDL2 is told at configure time rather than encouraged to believe a
# window is coming.
option(MAIZE_DISPLAY "Build the opt-in SDL2 host window backend (framebuffer + keyboard)" OFF)

if (MAIZE_DISPLAY)
  find_package(SDL2 REQUIRED)
  message(STATUS
    "maize-450: MAIZE_DISPLAY=ON and SDL2 was found, but no target consumes it yet. "
    "v1's maizeg is archived and mzvmg has no display device yet (maize-456), so this "
    "build links no SDL2 and opens no window.")
endif()

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  # maize-418 and maize-422: the v2 interpreter's fixtures and the mzasm suite, one CTest
  # entry per fixture. This is the whole registered suite, which is what makes a bare
  # `ctest` run v2 and only v2. cmake/MaizeCTest.cmake, which registered v1's
  # guest-toolchain fixtures, is no longer included; see the note at its head.
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MaizeV2Fixtures.cmake")
endif()

# maize-450: this file ends here because v1's declarations were taken out of it rather
# than commented out. A reader who greps this file for `maize`, `mazm` or `mzcc` and finds
# only prose has an answer: `git log -- CMakeLists.txt`, or the `v1` branch, where all of
# it still builds.

