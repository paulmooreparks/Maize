#!/bin/sh
# maize-473: the v1 asm/ harness that used to live here has been retired.
#
# It built and drove maize, maizeg, mazm, mzld and mzdis, none of which this branch
# defines any more (maize-450 dropped those targets when v1's build left the tree), so
# every run of it failed at the missing-executable step without testing anything.
#
# This stub stands at the old path on purpose. A stale bookmark, an old shell history
# entry or a document nobody swept still sends people here, and landing on an answer
# beats landing on "no such file or directory".
#
# Deliberately thin: it names no preset, no test count and no label set, because those
# move and a stub that repeats them would start lying the day they do. The build
# directory and the current suite are described in README.md.

cat >&2 <<'EOF'
scripts/run-tests.sh has been retired.

It drove the Maize v1 assembly corpus using binaries this branch no longer builds.

The test suite now runs under ctest against a configured build directory:

    ctest --test-dir build/<preset>

See README.md for configuring a preset and for selecting a subset of the suite.

The v1 harness is still runnable on the v1 branch, alongside the build that
defines the binaries it needs.
EOF

exit 2
