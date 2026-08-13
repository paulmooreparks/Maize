# cmake/ToolchainRoot.cmake (maize-439): resolves the vendored llvm-mingw compiler the
# same way scripts/lib/ToolchainRoot.ps1 and scripts/lib/toolchain-root.sh do. Edit the
# pin at scripts/toolchain-pins/llvm-mingw.pin, not here.
#
# This is a CMAKE TOOLCHAIN FILE, wired in through CMakePresets.json's toolchainFile
# rather than through cacheVariables. A toolchain file is the one CMake mechanism that
# runs arbitrary logic (environment reads, EXISTS checks, conditionals) before
# CMAKE_C_COMPILER is fixed, which is what an external-then-fallback resolution order
# needs and a hardcoded cacheVariables entry cannot express.
#
# The order, identical in all three implementations:
#
#   1. MAIZE_TOOLCHAIN_ROOT, when set and non-empty, names the toolchains root.
#   2. Otherwise the per-user default root: %LOCALAPPDATA%/Maize/toolchains on
#      Windows, ${XDG_CACHE_HOME:-$HOME/.cache}/maize/toolchains elsewhere.
#   3. Under that root, <root>/llvm-mingw/<pinned-version>/ answers when it holds
#      the compiler.
#   4. Otherwise the in-repo fallback <repo-root>/.toolchains/llvm-mingw/, when IT
#      does. A checkout predating maize-439 keeps building with no migration step.
#   5. Otherwise FATAL_ERROR naming both checked paths and the bootstrap command.
#
# scripts/test-toolchain-resolution.sh holds all three implementations to the same
# answer by populating several candidates at once with distinguishable probe bytes. It
# runs on the Windows CI job (.github/workflows/ci.yml, "Toolchain resolver agreement"),
# which is where both of its prerequisites, PowerShell and cmake, are present. It is
# NOT registered with ctest.
#
# FORCE below is deliberate (Review finding 2, 2026-08-12; decision D-6): a toolchain
# file is RE-INCLUDED on every configure of an existing build directory, but
# set(... CACHE) WITHOUT FORCE is a no-op once CMAKE_C_COMPILER already carries a value
# from a prior configure, which every build directory configured under the OLD
# hardcoded-cacheVariables scheme already has. Without FORCE this file's own logic,
# including the FATAL_ERROR check below, would silently never run again on any of those
# pre-existing build directories.
#
# FORCE has its own cost (Review finding, 2026-08-13; decision D-8): it silently
# discards a hand-passed -DCMAKE_C_COMPILER=, which every dispatch brief on this board
# used to instruct agents to pass. The two message() calls below make that loud. Every
# configure states which directory it resolved, and a configure that is about to
# discard a DIFFERENT previously-cached value (whether that value came from a stale
# cache or from a command line; this file cannot tell those apart, and does not need
# to) emits a WARNING naming the discarded value and the MAIZE_TOOLCHAIN_ROOT
# alternative. A WARNING does not fail the configure; FATAL_ERROR is reserved for
# "nothing resolves at all".
#
# Do not remove FORCE without reading D-6 and D-8 on maize-439 first. Removing it
# reopens the stale-cache bug; removing the messages makes the override silent again.

# The pin format's one rule, kept identical in ToolchainRoot.ps1 and toolchain-root.sh:
# a line whose first character is '#' is a comment, blank lines are ignored, and of
# what remains line 1 is the version and line 2 is the sha256. The REGEX filters
# comments; file(STRINGS) already drops empty lines.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../scripts/toolchain-pins/llvm-mingw.pin"
     _maize_pin REGEX "^[^#]")
if(_maize_pin STREQUAL "")
  message(FATAL_ERROR
    "scripts/toolchain-pins/llvm-mingw.pin holds no version line. "
    "Expected the pinned version on the first non-comment line.")
endif()
list(GET _maize_pin 0 _maize_version)
string(STRIP "${_maize_version}" _maize_version)

if(DEFINED ENV{MAIZE_TOOLCHAIN_ROOT} AND NOT "$ENV{MAIZE_TOOLCHAIN_ROOT}" STREQUAL "")
  set(_maize_root "$ENV{MAIZE_TOOLCHAIN_ROOT}")
# CMAKE_HOST_WIN32, not WIN32. A toolchain file is included from
# CMakeDetermineSystem before the platform modules run, so WIN32 is not reliably set
# here yet; CMAKE_HOST_WIN32 is set at startup and is the documented way to ask about
# the host. The three resolvers all key on the host, since the per-user default is a
# property of the machine's home layout rather than of the build target.
elseif(CMAKE_HOST_WIN32)
  set(_maize_root "$ENV{LOCALAPPDATA}/Maize/toolchains")
elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
  set(_maize_root "$ENV{XDG_CACHE_HOME}/maize/toolchains")
else()
  set(_maize_root "$ENV{HOME}/.cache/maize/toolchains")
endif()
# LOCALAPPDATA and an operator-set MAIZE_TOOLCHAIN_ROOT both arrive in native Windows
# form; CMake wants forward slashes in a path it will hand to a compiler command line.
file(TO_CMAKE_PATH "${_maize_root}" _maize_root)

set(_maize_versioned "${_maize_root}/llvm-mingw/${_maize_version}")
# ABSOLUTE, so the compiler path this file caches, and the path its FATAL_ERROR
# prints, are both free of the "/cmake/.." segment CMAKE_CURRENT_LIST_DIR introduces.
get_filename_component(_maize_repo_fallback
  "${CMAKE_CURRENT_LIST_DIR}/../.toolchains/llvm-mingw" ABSOLUTE)

if(EXISTS "${_maize_versioned}/bin/x86_64-w64-mingw32-clang++.exe")
  set(_maize_dir "${_maize_versioned}")
elseif(EXISTS "${_maize_repo_fallback}/bin/x86_64-w64-mingw32-clang++.exe")
  set(_maize_dir "${_maize_repo_fallback}")
else()
  # This is maize-439's configure-time check. The failure that motivated the card
  # surfaced as "CreateProcess failed: The system cannot find the file specified" from
  # ninja, weeks after a successful configure, naming nothing. This names both paths
  # it looked in and the command that fixes it.
  message(FATAL_ERROR
    "Vendored llvm-mingw compiler not found. Checked:\n"
    "  ${_maize_versioned}\n"
    "  ${_maize_repo_fallback}\n"
    "Run: scripts\\bootstrap-toolchain.ps1  (or scripts/bootstrap-toolchain.sh under Git Bash)\n"
    "Set MAIZE_TOOLCHAIN_ROOT to install and resolve somewhere other than the per-user default.")
endif()

set(_maize_cc  "${_maize_dir}/bin/x86_64-w64-mingw32-clang.exe")
set(_maize_cxx "${_maize_dir}/bin/x86_64-w64-mingw32-clang++.exe")

message(STATUS "Maize toolchain: using ${_maize_dir}")

if(DEFINED CACHE{CMAKE_C_COMPILER} AND NOT "$CACHE{CMAKE_C_COMPILER}" STREQUAL ""
   AND NOT "$CACHE{CMAKE_C_COMPILER}" STREQUAL "${_maize_cc}")
  message(WARNING
    "Maize toolchain: overriding a different previously-set CMAKE_C_COMPILER "
    "($CACHE{CMAKE_C_COMPILER}) with ${_maize_cc}. If you passed -DCMAKE_C_COMPILER= "
    "on the command line, it was discarded: set MAIZE_TOOLCHAIN_ROOT instead to "
    "choose a different compiler.")
endif()

set(CMAKE_C_COMPILER   "${_maize_cc}"  CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_maize_cxx}" CACHE FILEPATH "" FORCE)

# Two readers, which is why this is CACHE INTERNAL rather than an ordinary variable.
#
# scripts/test-toolchain-resolution.sh includes this file with cmake -P and prints it,
# to compare this resolver's answer against the PowerShell and sh ones.
#
# CMakeLists.txt reads it as the marker that this file ran for THIS build directory,
# and fails when a mingw-clang build directory does not carry it. That guard exists
# because FORCE does not reach as far as it looks. A toolchain file is re-included on
# every configure only when the build directory was FIRST configured with one: CMake
# writes an include() of it into CMakeFiles/<ver>/CMakeSystem.cmake at that point, and
# a build directory configured under the old hardcoded-cacheVariables preset carries no
# such line and never gains one. Naming a toolchain file on a later configure of that
# directory is accepted, cached, and silently never acted on, so neither FORCE nor the
# FATAL_ERROR above can run there. Measured on maize-439: a build directory configured
# under the old preset then reconfigured with this one kept the old in-repo compiler
# and printed nothing at all. The marker is what turns that silence into a message.
set(MAIZE_RESOLVED_TOOLCHAIN_DIR "${_maize_dir}" CACHE INTERNAL "Directory cmake/ToolchainRoot.cmake resolved the pinned llvm-mingw toolchain to." FORCE)
