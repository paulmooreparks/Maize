# Vendored C toolchain

This directory vendors the two upstream tools that the Maize C toolchain is built
on. They are pinned as git submodules. The Maize code-generation target lives
in-repo under `toolchain/qbe-maize/` and is overlaid onto the qbe checkout at build
time (`scripts/apply-maize-qbe-target.sh`), so the submodules stay pristine
upstream.

## Pins

| Tool  | Path             | Submodule URL                              | Pinned commit                              | License |
|-------|------------------|--------------------------------------------|--------------------------------------------|---------|
| cproc | `toolchain/cproc`| https://github.com/michaelforney/cproc     | `d1c53ddf56571573a7025324c8dd5c6d547a4d1f` | ISC     |
| qbe   | `toolchain/qbe`  | https://github.com/michaelforney/qbe       | `4420727667b915042050b9bfa6eb381ce7a97ba5` | MIT     |

Both licenses (ISC, MIT) are permissive and compatible with this repo's Apache-2.0,
so there is no license-driven reason to prefer an in-tree copy over a submodule.

## Canonical upstreams

- cproc canonical home: https://sr.ht/~mcf/cproc/ (author Michael Forney).
  `github.com/michaelforney/cproc` is the author's own GitHub mirror and is the pin
  source here.
- qbe canonical home: https://c9x.me/compile/ (author Quentin Carbonneaux), git at
  `https://c9x.me/git/qbe.git`. `github.com/michaelforney/qbe` is a faithful mirror
  (upstream commits by Quentin Carbonneaux plus merge commits) maintained by the
  cproc author; it is the pin source here.

## Why GitHub mirrors rather than the canonical sr.ht / c9x.me hosts

The submodule mechanism requires a clean recursive checkout so that a
fresh clone plus one command builds both tools. The canonical hosts introduce checkout
friction that undermines that goal:

- `https://c9x.me/git/qbe.git` returns `HTTP/0.9 when not allowed` under git's default
  HTTP negotiation; a plain `git clone` (and therefore `git clone --recurse-submodules`)
  fails unless the caller sets `http.version=HTTP/1.1`. It is also a single personal
  server, i.e. a CI-availability single point of failure.
- The `github.com/8l/qbe` mirror was rejected: its master tip carries non-upstream
  "USFO" commits (author `Linux User <obarun@localhost>`) layered on top of the real
  history, so it is not a faithful pin source.

`github.com/michaelforney/{cproc,qbe}` are GitHub-hosted, faithful, and give
friction-free `actions/checkout` recursive submodule checkout with the workflow token.
This keeps the primary mechanism (submodules) intact rather than falling back to
in-tree vendored copies.

The qbe pin (`4420727`, master) is a stable snapshot; bump it when the Maize target
needs a newer qbe.

## SDL2 (Windows `--display` window backend)

The maize VM's opt-in `--display` window backend (`MAIZE_DISPLAY=ON`, `src/devices.cpp`)
links SDL2. On Linux/macOS SDL2 comes from the system package manager
(`libsdl2-dev` / `sdl2-config`). On Windows it is a pinned, downloaded dependency,
NOT a submodule and NOT committed, fetched into the gitignored `.toolchains/SDL2/`
by `scripts/bootstrap-sdl2.ps1` (the SDL2 counterpart of
`scripts/bootstrap-toolchain.ps1` for llvm-mingw).

| Dep  | Path (Windows)                          | Source                                   | Pinned version | SHA256 (asset) | License |
|------|-----------------------------------------|------------------------------------------|----------------|----------------|---------|
| SDL2 | `.toolchains/SDL2/x86_64-w64-mingw32/`  | github.com/libsdl-org/SDL (mingw devel)  | `2.32.8`       | `2f0a74c2…7249e2` (`SDL2-devel-2.32.8-mingw.zip`) | zlib |

`scripts/install-mazm.ps1` auto-invokes `bootstrap-sdl2.ps1` when the SDL2 dir is
missing and refuses to silently degrade to a headless maize (pass `-Headless` to opt
out). This closes the recurring "install suddenly breaks" trap: previously SDL2 was a
manually-placed, unpinned, undocumented directory with no fetch script, so when it
was cleaned or lost, `find_package(SDL2 REQUIRED)` hard-failed the configure and the
whole tool install died. To bump the pin: change `$Version` + `$Sha256` in
`bootstrap-sdl2.ps1` together (recompute the hash with `Get-FileHash` on the asset).
2.32.x is the final SDL2 (2.x) series.

## Fresh-clone / build

```
git clone --recurse-submodules <repo>
# or, after a plain clone:
git submodule update --init --recursive

scripts/build-toolchain.sh        # Linux / macOS / MSYS2 (POSIX)
```

CI builds both tools via `scripts/build-toolchain.sh` as a step kept separate from the
asm/ PASS/FAIL harness (`scripts/run-tests.{sh,ps1}`).

## Build environments

- Linux (`ubuntu-latest`): system GCC/Clang + POSIX `make`, already present.
- Windows (`windows-latest`): built under MSYS2's POSIX environment (the
  `msys2/setup-msys2` action, `MSYS` msystem, `gcc` + `make`). cproc's `driver.c`
  depends on POSIX process APIs (`<spawn.h>`, `posix_spawn`, `<sys/wait.h>`,
  `<unistd.h>`) and cproc's `configure` only recognizes POSIX target triples, so a
  native llvm-mingw (Windows PE) build of cproc is not possible without patching
  vendored upstream. MSYS2's POSIX layer supplies those APIs; qbe builds there too, so
  a single environment covers both tools. The repo's llvm-mingw toolchain stays
  dedicated to Maize's own CMake/ninja build and is not used for the C toolchain.
