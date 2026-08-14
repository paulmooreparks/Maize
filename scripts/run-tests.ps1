<#
.SYNOPSIS
    Retired. The Maize v1 assembly-corpus harness that used to live here is gone.

.DESCRIPTION
    This script drove the v1 asm/ corpus using binaries this branch no longer
    builds, so it could not run a test even once. It now prints where the suite
    went and exits 2.

    The test suite runs under ctest against a configured build directory:

        ctest --test-dir build/<preset>

    See README.md for configuring a preset and for selecting a subset of the
    suite. The v1 harness is still runnable on the v1 branch, alongside the
    build that defines the binaries it needs.
#>

# maize-473: the v1 asm/ harness that used to live here has been retired.
#
# The help block above is not decoration. Get-Help parses this file without running
# it (maize-450 entry 57), so the exit below cannot reach a reader who asks that way,
# and the notice has to appear in both places to reach both readers.
#
# It built and drove maize, maizeg, mazm, mzld and mzdis, none of which this branch
# defines any more (maize-450 dropped those targets when v1's build left the tree), so
# every run of it failed at the missing-executable step without testing anything.
#
# This stub stands at the old path on purpose. A stale bookmark, an old shell history
# entry or a document nobody swept still sends people here, and landing on an answer
# beats landing on a missing file.
#
# Deliberately thin: it names no preset, no test count and no label set, because those
# move and a stub that repeats them would start lying the day they do. The build
# directory and the current suite are described in README.md.
#
# Kept in step with scripts/run-tests.sh, which says the same thing to the same reader.

[Console]::Error.WriteLine(@'
scripts\run-tests.ps1 has been retired.

It drove the Maize v1 assembly corpus using binaries this branch no longer builds.

The test suite now runs under ctest against a configured build directory:

    ctest --test-dir build/<preset>

See README.md for configuring a preset and for selecting a subset of the suite.

The v1 harness is still runnable on the v1 branch, alongside the build that
defines the binaries it needs.
'@)

exit 2
