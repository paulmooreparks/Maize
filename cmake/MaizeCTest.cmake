# MaizeCTest.cmake (maize-376): scripts/run-ctest.sh's fixtures as real CTest tests.
#
# Before this file the C-toolchain suite was one 4500-line shell script that always ran
# every fixture, serially, with no per-fixture timeout and no way to select a subset.
# Every mechanism that fixes that already exists in CTest, so the harness is wired to it
# rather than reimplemented: `ctest -R` / `-L` select, `-jN` parallelises, the TIMEOUT
# property bounds a hang to one test instead of the whole run, and ctest reports per-test
# durations itself.
#
# Shape. One FIXTURES_SETUP test (ctest_guest_env) runs the harness preamble exactly once
# per ctest invocation via `run-ctest.sh --ctest-setup <file>`: the WSL native mirror sync,
# the toolchain build-if-absent check, the MAZM/MAIZE/MAIZEG/MZLD/MZCC resolution, and the
# three exec-wrapper scripts. It writes <file> as a sourceable snapshot. Every fixture test
# then runs `run-ctest.sh --ctest-env <file> --only <label>`, which sources that snapshot,
# skips the preamble entirely, and runs exactly one dispatch site. The setup test is the
# only writer of the wrapper scripts, which is what makes -jN safe: those three scripts sit
# at fixed shared paths and are written by truncate-then-write redirection, and CTest
# guarantees a FIXTURES_SETUP test completes before any FIXTURES_REQUIRED test starts.
#
# Concurrency classification. Every fixture below carries an explicit category:
#   - no lock: the fixture writes only fixture-name-prefixed paths under the per-preset
#     WORK_DIR, or mkdtemp-based scratch, and never builds a guest image. Note that a
#     fixture-name prefix is NOT what keeps the C-fixture compiles apart, because several
#     fixtures compile the same source (hello.c is compiled by four separate tests). The
#     harness routes every compile_c artifact into a per-label $CC_WORK_DIR under --only
#     instead; see the comment on that variable in scripts/run-ctest.sh.
#   - MAIZE_QUESOS_LOCK / MAIZE_USERLAND_LOCK: the fixture drives a full quesOS or
#     userland guest build.
#   - PROCESSORS: a load reservation for the one fixture that drives real ptys.
# The locks are WINDOWS-ONLY (operator decision, maize-376 comment 3422). Their rationale
# is the maize-304 MSYS2 dofork fork-exhaustion history recorded in scripts/lib/
# harness-env.sh, which is platform-specific; the object cache (src/mzcc_cache.c, atomic
# temp-plus-rename) and the mkdtemp-based staging roots are race-free on every platform.
# On Linux the locks are omitted, which is what collapses the parallel floor from the SUM
# of the locked fixtures' durations toward the longest single test.
#
# Timeouts are sized off a measured fixture-timings.log baseline (linux-debug, warm caches,
# 32-core WSL host: 147s serial, longest single fixture 25s, 68 of 80 fixtures at 0-1s).
# They are hang bounds, not speed gates, so the headroom is deliberately wide, and the
# bundling fixtures' values sit ABOVE the harness's own internal `timeout` ceilings so the
# harness always gets to report the specific diagnosis before ctest kills the test.

get_filename_component(_maize_binary_dir_name "${CMAKE_BINARY_DIR}" NAME)
set(MAIZE_CTEST_PRESET "${_maize_binary_dir_name}" CACHE STRING
    "Preset name run-ctest.sh resolves its build directory from (defaults to the binary directory's own name, which is the preset name for every CMakePresets.json preset)")

set(MAIZE_CTEST_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(MAIZE_CTEST_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-ctest.sh")
set(MAIZE_CTEST_ENV_FILE "${CMAKE_BINARY_DIR}/ctest-run/ctest-env.sh")

# run-ctest.sh is POSIX sh. On Linux and macOS /bin/sh is right there; on Windows the
# harness has always run under Git Bash (ci.yml's windows leg already invokes it through
# `shell: bash`), so look for Git's own sh before giving up. ctest execs the test command
# directly rather than through a shell, so this has to be a real resolved path.
find_program(MAIZE_SH_EXECUTABLE
    NAMES sh bash
    HINTS
        "$ENV{ProgramFiles}/Git/usr/bin"
        "$ENV{ProgramFiles}/Git/bin"
        "C:/Program Files/Git/usr/bin"
        "C:/Program Files/Git/bin")

# This stays a WARNING rather than a FATAL_ERROR, and the reasoning is a ruling, not an
# oversight. Configuring Maize does not otherwise require a POSIX shell: a Windows
# operator with MSVC or llvm-mingw and CMake can build maize.exe, mazm and the rest from
# a fresh clone today, and promoting this to FATAL_ERROR would make Git Bash a hard
# CONFIGURE dependency of the VM itself. That runs directly against the build-dependency
# minimalism ruling (a fresh clone must build on a typical machine with minimum
# dependencies, and bash-on-Windows is the interim arrangement maize-266 exists to remove),
# so the cost lands on exactly the people that ruling protects.
#
# The false-green risk that motivates the question is real, and it is closed on the side
# where it actually bites. CI runs `ctest --no-tests=error` on all three legs (ci.yml), so
# a leg whose configure registered nothing fails instead of passing vacuously. A developer
# who sees this warning on their own machine still gets a working build, and the harness
# remains runnable standalone through scripts/run-ctest.sh. Neither path is silently
# permissive: the local one warns, and the CI one is red.
if (NOT MAIZE_SH_EXECUTABLE)
    message(WARNING
        "maize-376: no POSIX sh found, so the C-toolchain ctest suite is not registered. "
        "The rest of the build is unaffected. Install Git Bash (Windows) or a POSIX shell "
        "and re-configure to get the suite; scripts/run-ctest.sh still runs standalone. "
        "CI runs ctest with --no-tests=error, so this state cannot pass there silently.")
    return()
endif()

# The setup test resolves the whole environment once. It deliberately does NOT pass
# --skip-build (resolving open_question 10259): today a bare `scripts/run-ctest.sh` builds
# the C toolchain when it is absent, and that fresh-clone convenience is worth keeping on
# the ctest path too. The TIMEOUT below is sized to cover a from-scratch toolchain build,
# not just the resolution. CI's own legs build first anyway, so they never pay for it.
add_test(NAME ctest_guest_env
         COMMAND "${MAIZE_SH_EXECUTABLE}" "${MAIZE_CTEST_SCRIPT}"
                 --preset "${MAIZE_CTEST_PRESET}"
                 --ctest-setup "${MAIZE_CTEST_ENV_FILE}"
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(ctest_guest_env PROPERTIES
    FIXTURES_SETUP maize_guest_env
    TIMEOUT 3600
    LABELS "setup")

# No FIXTURES_CLEANUP test (resolving open_question 10260): the harness has always left
# build/<preset>/ctest-run in place after a run, every scratch path in it is overwritten
# by the next run rather than accumulating, and the operator debugs failures out of it.
# Deleting it would trade a real debugging affordance for nothing.

# maize_ctest_fixture(<label> LABELS <l>... [TIMEOUT <s>] [PROCESSORS <n>]
#                     [QUESOS_LOCK] [USERLAND_LOCK])
#   Register one run-ctest.sh dispatch site as a CTest test named after its own
#   `_mz_want "<label>"` guard, so the ctest name and the harness label can never drift.
function(maize_ctest_fixture label)
    cmake_parse_arguments(MZF "QUESOS_LOCK;USERLAND_LOCK" "TIMEOUT;PROCESSORS" "LABELS" ${ARGN})
    if (NOT MZF_LABELS)
        message(FATAL_ERROR "maize_ctest_fixture(${label}): LABELS is required")
    endif()
    if (NOT MZF_TIMEOUT)
        message(FATAL_ERROR "maize_ctest_fixture(${label}): TIMEOUT is required")
    endif()

    add_test(NAME "${label}"
             COMMAND "${MAIZE_SH_EXECUTABLE}" "${MAIZE_CTEST_SCRIPT}"
                     --ctest-env "${MAIZE_CTEST_ENV_FILE}" --only "${label}"
             WORKING_DIRECTORY "${MAIZE_CTEST_SOURCE_DIR}")
    set_tests_properties("${label}" PROPERTIES
        FIXTURES_REQUIRED maize_guest_env
        TIMEOUT "${MZF_TIMEOUT}"
        LABELS "${MZF_LABELS}")

    # Windows-only, per the operator decision on maize-376 comment 3422.
    if (WIN32)
        set(_locks "")
        if (MZF_QUESOS_LOCK)
            list(APPEND _locks quesos_build)
        endif()
        if (MZF_USERLAND_LOCK)
            list(APPEND _locks userland_build)
        endif()
        if (_locks)
            set_tests_properties("${label}" PROPERTIES RESOURCE_LOCK "${_locks}")
        endif()
    endif()

    if (MZF_PROCESSORS)
        set_tests_properties("${label}" PROPERTIES PROCESSORS "${MZF_PROCESSORS}")
    endif()

    set_property(GLOBAL APPEND PROPERTY MAIZE_CTEST_DECLARED_FIXTURES "${label}")
endfunction()

# --- toolchain: the C end-to-end corpus ------------------------------------------------
# Concurrency: no lock. Each compiles ctest/<name>.c through the driver and runs it, into
# the per-label ${WORK_DIR}/ctest-scratch/<label>/ that --only selects, so two tests that
# compile the same source (kilo_xalloc_die and kilo_xalloc_die_exit, for instance) write
# separate images rather than racing on one.
# Measured 0-1s each. TIMEOUT 300 bounds a wedged compile (the harness's own per-compile
# ceiling is 180s, so it still diagnoses first).
foreach(_t
        hello capstone globals ptrdata ldzfold voidcall freelist addrlocalphi spill
        caddroff fp syscall_raw syscall_write syscall_errno syscall_close str bulkmem
        ctype sbrk malloc
        stdint minmax_signedness rthdrs2 packed atexit strtol clock palette_blit_selfcheck
        rw_bounds_selfcheck
        varargs printf libcgaps libcgaps3 exitcode abort noreturn kilo_next_cap
        kilo_xalloc_die kilo_xalloc_die_exit kilo_hl_tab_comment kilo_hl_space_comment
        run_qbe_flag run_args_test run_image_resolution run_wx_reject_test
        run_default_produce_test run_driver_run_mode_test multifile
        run_multi_link_reject_test multifile_no_out multifile_emit_reject)
    maize_ctest_fixture(${_t} LABELS "toolchain" TIMEOUT 300)
endforeach()

# --- hostfs: the --mount acceptance family ---------------------------------------------
# Concurrency: no lock. Each prepares its own ${WORK_DIR}/<name> host tree.
foreach(_t
        run_hostfs_cat run_hostfs_ls run_hostfs_stat run_hostfs_escape run_hostfs_rofs
        run_hostfs_stdio run_hostfs_savefs run_hostfs_savefs_neg run_hostfs_truncate
        run_hostfs_root_merge)
    maize_ctest_fixture(${_t} LABELS "hostfs" TIMEOUT 300)
endforeach()

# --- terminal / console device self-checks ---------------------------------------------
# Concurrency: no lock. Headless --console-dump over a pipe, no shared console device.
maize_ctest_fixture(run_terminal_selfcheck LABELS "terminal" TIMEOUT 300)
maize_ctest_fixture(run_console_selfcheck  LABELS "terminal" TIMEOUT 300)

# --- doom: the bare-VM demo gates ------------------------------------------------------
# Concurrency: no lock. The three generator fixtures write distinct ${WORK_DIR} images
# (doom_render.mzx / doom_transition.mzx / doom_selfcheck.mzx) and distinct IWADs.
# Measured 9-14s each, the heaviest non-guest-build fixtures in the suite.
maize_ctest_fixture(run_doom_link       LABELS "doom" TIMEOUT 900)
maize_ctest_fixture(run_doom_selfcheck  LABELS "doom" TIMEOUT 900)
maize_ctest_fixture(run_doom_render     LABELS "doom" TIMEOUT 900)
maize_ctest_fixture(run_doom_transition LABELS "doom" TIMEOUT 900)
maize_ctest_fixture(run_doom_input      LABELS "doom" TIMEOUT 900)

# --- launcher: ~/.maize config resolution ----------------------------------------------
# Concurrency: no lock. Each sets HOME command-scoped to its own fake_home under WORK_DIR
# (never exported), and none of the three drives a guest build.
maize_ctest_fixture(run_launcher_defaults     LABELS "launcher" TIMEOUT 300)
maize_ctest_fixture(run_launcher_config_mount LABELS "launcher" TIMEOUT 600)
maize_ctest_fixture(run_launcher_per_binary   LABELS "launcher" TIMEOUT 600)

# --- jit: interpreter-vs-JIT equivalence ------------------------------------------------
# Concurrency: no lock. Reads BARE_MAIZE (written by the setup test) and never rewrites it.
maize_ctest_fixture(run_timer_cadence_equiv LABELS "toolchain;jit" TIMEOUT 300)

# --- quesos: fixtures that drive a full quesOS guest build ------------------------------
# Concurrency: RESOURCE_LOCK quesos_build on Windows only. Measured 1s (selfcheck,
# argcheck, default_init, quiet_boot), 4s (quesos94), 25s (ac_fixtures, the single longest
# fixture in the suite and therefore the parallel floor on Linux).
maize_ctest_fixture(run_quesos_selfcheck    LABELS "quesos" TIMEOUT 900  QUESOS_LOCK)
maize_ctest_fixture(run_quesos_argcheck     LABELS "quesos" TIMEOUT 900  QUESOS_LOCK)
maize_ctest_fixture(run_quesos_default_init LABELS "quesos" TIMEOUT 900  QUESOS_LOCK)
maize_ctest_fixture(run_quesos_quiet_boot   LABELS "quesos" TIMEOUT 900  QUESOS_LOCK)
# ac_fixtures compiles roughly 34 sources through cc_maize_compile_bounded's 180s-per-file
# ceiling, so the test bound has to sit above that batch's own worst case.
maize_ctest_fixture(run_quesos_ac_fixtures  LABELS "quesos" TIMEOUT 3600 QUESOS_LOCK)
maize_ctest_fixture(run_quesos94_fixtures   LABELS "quesos" TIMEOUT 1800 QUESOS_LOCK)
# maize-313: the stdin-wake group. Several of its legs are deliberately SLOW, because the
# thing under test is what happens while the guest sits on its idle path: the 200-byte leg
# spends 4 seconds feeding, the relatch leg waits out two 1000 ms deadlines, and the
# select-timeout leg holds an open-but-silent stdin. Sized for the compile burst plus that.
maize_ctest_fixture(run_stdin_wake_fixtures LABELS "quesos" TIMEOUT 1800 QUESOS_LOCK)
# doom_quesos carries an internal `timeout 480` render step (sized for the ASan leg) plus a
# 240s pty presenter check, so 1800 leaves it room to diagnose itself first.
maize_ctest_fixture(run_doom_quesos LABELS "doom;quesos" TIMEOUT 1800 QUESOS_LOCK)

# --- userland: quesOS plus a cross-compiled /bin set ------------------------------------
# run_userland94_fixtures builds quesOS and the 11-tool wave-1 set, then runs roughly 22
# named sub-checks, four of them over real ptys (pty_oksh_check.py, pty_oksh_kilo_check.py).
# Measured 23s warm, of which the kilo largefile load is 5.2s (LOAD_MS=5223).
#
# PROCESSORS 4 is a load RESERVATION, not an exclusivity lock, and the distinction is
# deliberate. The real-pty sub-checks poll with fixed-second windows tuned for an unloaded
# host, so heavy co-scheduled load is the hazard. Full exclusivity (PROCESSORS at or above
# the -j width) would make CTest run this 23s test with the whole machine idle, costing
# more wall-clock than the flakiness it hedges, since the suite's floor is already set by
# the 25s run_quesos_ac_fixtures. Reserving 4 slots keeps concurrent load off it cheaply.
# The gate that actually settles this is the repeated-run comparison: if a -jN repeat ever
# diverges from serial here, raise PROCESSORS to full isolation rather than retrying.
#
# TIMEOUT 5400: the harness's own UBUILD_TIMEOUT for this fixture is 11 tools * 180s =
# 1980s, followed by roughly 12 more bounded compiles at up to 180s each, so a genuinely
# cold, uncached build can legitimately run past an hour's worth of internal ceilings. This
# has to sit above them so the harness reports the specific cause.
maize_ctest_fixture(run_userland94_fixtures
    LABELS "userland;quesos;oksh" TIMEOUT 5400 PROCESSORS 4 QUESOS_LOCK USERLAND_LOCK)

# wave2 builds the full 43-program default set with no internal timeout wrap at all, so its
# bound is the only one it has.
maize_ctest_fixture(run_userland_wave2_fixtures
    LABELS "userland;quesos" TIMEOUT 5400 QUESOS_LOCK USERLAND_LOCK)

# --- the POSIX .sh builders' own smoke gate ---------------------------------------------
# Takes quesos_build (one full quesOS link via os/quesos/build-quesos.sh) but not
# userland_build: its userland half is one cheap tool by design, not the full set.
maize_ctest_fixture(run_sh_builder_smoke LABELS "quesos;userland" TIMEOUT 1800 QUESOS_LOCK)

# --- drift guard -----------------------------------------------------------------------
# The whole point of the conversion is that no fixture is silently dropped, and the only
# durable way to hold that is to check it mechanically at configure time rather than once
# by hand. Every dispatch site in run-ctest.sh is a top-level `_mz_want "<label>" &&`
# statement, so read them straight out of the script and require the two sets to match
# exactly. Adding a fixture to the script without registering it here (or vice versa) is
# then a configure error instead of a test that quietly stops running.
file(STRINGS "${MAIZE_CTEST_SCRIPT}" _mz_want_lines REGEX "^_mz_want \"[^\"]+\" &&")
# Two of the dispatch statements are shell line-continuations, so their matched line ends
# in a backslash. file(STRINGS) builds a CMake list by joining with ";", and a trailing
# backslash escapes that separator, silently MERGING those entries with the ones after
# them (which is how this guard first reported two perfectly present fixtures as missing).
# Un-escape the separator so every matched line is its own list element again.
string(REPLACE "\\;" ";" _mz_want_lines "${_mz_want_lines}")
set(_script_labels "")
foreach(_line IN LISTS _mz_want_lines)
    string(REGEX REPLACE "^_mz_want \"([^\"]+)\".*$" "\\1" _lbl "${_line}")
    list(APPEND _script_labels "${_lbl}")
endforeach()
get_property(_declared_labels GLOBAL PROPERTY MAIZE_CTEST_DECLARED_FIXTURES)

set(_missing_here "${_script_labels}")
list(REMOVE_ITEM _missing_here ${_declared_labels})
set(_missing_there "${_declared_labels}")
list(REMOVE_ITEM _missing_there ${_script_labels})

# Set difference alone would pass a label that appears TWICE on one side and once on the
# other, because list(REMOVE_ITEM) removes every matching element rather than one, which
# empties both difference sets. The invariant the guard claims is one dispatch site to
# exactly one add_test entry, so it has to test the "exactly one" half too. A plain
# list(LENGTH) comparison against a deduplicated copy would catch that, but naming the
# offending label costs one small loop and saves the next reader a hunt.
function(_maize_repeated_labels out_var)
    set(_seen "")
    set(_repeats "")
    foreach(_l IN LISTS ARGN)
        if (_l IN_LIST _seen)
            list(APPEND _repeats "${_l}")
        else()
            list(APPEND _seen "${_l}")
        endif()
    endforeach()
    set(${out_var} "${_repeats}" PARENT_SCOPE)
endfunction()

_maize_repeated_labels(_dupes_script ${_script_labels})
_maize_repeated_labels(_dupes_declared ${_declared_labels})

if (_missing_here OR _missing_there OR _dupes_script OR _dupes_declared)
    message(FATAL_ERROR
        "maize-376: cmake/MaizeCTest.cmake and scripts/run-ctest.sh have drifted apart.\n"
        "  in run-ctest.sh but not registered here: ${_missing_here}\n"
        "  registered here but not in run-ctest.sh: ${_missing_there}\n"
        "  dispatched more than once in run-ctest.sh: ${_dupes_script}\n"
        "  registered more than once here: ${_dupes_declared}\n"
        "Every fixture dispatch site must have exactly one add_test entry.")
endif()

list(LENGTH _script_labels _mz_fixture_count)
message(STATUS "maize-376: registered ${_mz_fixture_count} run-ctest.sh fixtures as ctest tests (preset ${MAIZE_CTEST_PRESET})")
