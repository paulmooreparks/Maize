/* demos/doom/doom_bench_quesos.c -- doom_bench for a quesOS worklist spawn.
 *
 * The identical timedemo to doom_bench.c, with the DOOM arguments baked into
 * the binary, because a program spawned from the quesOS exec worklist receives
 * argv = { path } and nothing more, so -iwad cannot arrive on a command line.
 * doom_bench.c is included verbatim rather than copied so the two benches can
 * never drift; only main() differs. Earlier characterization rounds used
 * untracked scratch copies of this file, which is how it got lost twice; it is
 * tracked now so scripts/jit-characterize.sh works from a fresh clone.
 *
 * Run hosted and headless, with the IWAD dir mounted read-only at /ro:
 *   maize --no-root --fb-no-display --mount <dir>=/ro:ro quesos.mzx /ro/doomq.mzx
 */
#define main doom_bench_main
#include "doom_bench.c"
#undef main

int main(void)
{
    static char *baked_argv[] = {
        "doom_bench_quesos", "-iwad", "/ro/min.wad",
        "-warp", "1", "1", "-nomonsters", 0
    };
    return doom_bench_main(7, baked_argv);
}
