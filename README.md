# debris (native)

A native player for **fr-041: debris.**, the 177 kB demo by Farbrausch (Breakpoint 2007), written in C99 on top of SDL3 and
OpenGL ES 3. It evaluates the original compact werkkzeug3 production, generates its textures and geometry, executes its timeline
and synthesizes the soundtrack with a V2 synthesizer, with results that are bit-identical to the original arithmetic where that
can be checked.

Original credits: **fiver2** (visuals and direction), **chaos / ryg** (code), **ronny** (soundtrack), **kb** (synthesizer),
**wayfinder** (sound effects), **giZMo / tron / fried**.

## Build and run

Requirements: a C99 compiler (GCC or Clang), CMake 3.25 or newer, Ninja (or another generator), and either an installed SDL3
(3.2 or newer) or a network connection: without an installed SDL3 the pinned release is fetched and built as a static library.
Linux needs the usual SDL3 build dependencies (X11 and/or Wayland, ALSA, Mesa/EGL development files).

```sh
cmake --preset release            # build/release
cmake --build --preset release
./build/release/debris            # plays the demo; Esc quits
```

The executable looks for `debris_party.kx` and `debris_loader.v2m` in `assets/` next to itself (the build copies them there).

| Key | Action |
|---|---|
| Space | pause |
| Left / Right | seek 10 s back / forward |
| F | fullscreen |
| C | credits |
| F10 / F11 | statistics overlay / statistics on stdout |
| Esc | quit |

Useful options: `--size 1280x720`, `--fullscreen`, `--no-vsync`, `--no-audio`, `--single-thread`, `--texture-quality high|normal|low`
(`high` is the default, the authored size; every step down halves the width and height of the generated textures), `--licenses`. The options for tests and
measurements (`--time`, `--screenshot`, `--self-check`, `--gl-check`, `--soak`, ...) are described at the top of
[src/player/main.c](src/player/main.c).

## WebAssembly

The same player runs in the browser (WebGL 2, Web Audio) as one page, `index.html`, plus `index.js` and `index.wasm` (about 1.7 MB,
0.6 MB compressed; the data files are inside). Requirements: [Emscripten](https://emscripten.org/docs/getting_started/downloads.html)
7.0.0 or newer, CMake 3.25+ and Ninja. SDL3 is fetched as for the native build.

```sh
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install 7.0.0 && ~/emsdk/emsdk activate 7.0.0
source ~/emsdk/emsdk_env.sh                 # sets EMSDK and puts emcc on PATH

cmake --preset web                          # build/web
cmake --build --preset web
python3 -m http.server -d build/web/dist    # then open http://localhost:8000
```

The page asks for a click (browsers only start audio after one) and for a texture quality: *Normal* is the default there, `?quality=low|normal|high`
in the address preselects it. Loading takes a few seconds to half a minute depending on the device, the loader bar shows how far it is.
The keys are the native ones. A browser without WebGL 2 gets a message on the page.

Browsers have no worker thread for a static page (it needs COOP/COEP headers), so the player loads in 12 ms slices between two pictures
of the loader (`--single-thread`, which also works natively and is tested). Use the Emscripten that comes with the SDK: a Binaryen from
another source can reject the flags of a newer Emscripten (`Unknown option '--enable-bulk-memory-opt'`).

### GitHub Pages

[.github/workflows/pages.yml](.github/workflows/pages.yml) builds the web player with a pinned Emscripten on every push and pull request, and
deploys it when the push is to `main`. Once per repository: *Settings > Pages > Build and deployment > Source: GitHub Actions*. The site is then
at `https://<user>.github.io/<repository>/`. The job fails when `index.wasm` outgrows 4 MiB (`WASM_BUDGET_BYTES`).

## Tests

```sh
cmake --build --preset release --target check     # or: ./build/release/debris_tests
ctest --preset release -j8                        # the same suites, one CTest test each
```

`debris_tests` is a single program that needs no arguments. It runs every suite of [tests/suites](tests/suites) in a process of
its own, a few at a time, and prints one line per suite; the output of a suite is shown when it fails. The player itself is
started headless (SDL offscreen video driver, dummy audio) by a few of the suites and they skip when the machine has no OpenGL
ES context. [doc/TESTING.md](doc/TESTING.md) describes the suites, the optional comparisons with the reference implementation
and the sanitizer and `-march=native` presets.

## Style

The code is C99, formatted by [.clang-format](.clang-format): `cmake --build build/release --target format` applies it and
`--target format-check` verifies it. The libraries in `include/debris/core` keep the layout of their reference sources and are excluded.

## Layout

| Path | Contents |
|---|---|
| [include/debris/core](include/debris/core) | `v2.h` (V2 synthesizer and sequencer) and `wz3.h` (the werkkzeug3 layers: math, KX document, bitmaps, meshes, runtime, frame jobs); no platform code |
| [include/debris/render](include/debris/render) | `glr.h`: the OpenGL ES 3 / GL 3.3 executor of the frame jobs; no window, no loader |
| [include/debris/player](include/debris/player) | `player_*.h`: the SDL-free parts of the player (clock and audio hand-over, loading, resident scene, seeking, text, credits, command line) |
| [src/player](src/player) | `main.c`: the SDL3 application (native and Emscripten) |
| [web](web) | `index.html`: the page of the WebAssembly build |
| [tools](tools) | `v2_render` (song to PCM), `wz3_inventory` (what a KX document needs), `v2m_tool` |
| [tests](tests) | the test runner, its suites, the reference fixtures (`tests/data`) and optional oracle checkers |
| [cmake](cmake) | the build modules and scripts used by `CMakeLists.txt` |
| [assets](assets), [third_party/licenses](third_party/licenses) | the production data and the licences embedded in the player |
| [doc](doc) | architecture, testing and the reference implementation |

The libraries are single headers in the style of stb: define `V2_IMPLEMENTATION`, `WZ3_IMPLEMENTATION` or `GLR_IMPLEMENTATION`
in exactly one source file before including the header. [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) explains the design.

## Build options

| Option | Default | Meaning |
|---|---|---|
| `DEBRIS_BUILD_PLAYER` | ON | build the SDL3 player |
| `DEBRIS_BUILD_TOOLS` | ON (top level, not Emscripten) | build `v2_render`, `wz3_inventory`, `v2m_tool` |
| `DEBRIS_BUILD_TESTS` | ON (top level, not Emscripten) | build `debris_tests` and register it with CTest |
| `DEBRIS_FETCH_SDL3` | ON | fetch SDL3 (`DEBRIS_SDL3_TAG`) when no installed one is found |
| `DEBRIS_WEB_MAX_MEMORY` | 2GB | Emscripten only: the largest the WebAssembly memory may grow to |
| `DEBRIS_SANITIZE` | OFF | AddressSanitizer and UndefinedBehaviorSanitizer |
| `DEBRIS_BUILD_FUZZERS` | OFF | libFuzzer harnesses for the KX parser and the V2M loader (Clang) |
| `DEBRIS_V2_RONAN` | ON | include the Ronan speech synthesizer (V2 channel 15) |
| `DEBRIS_TEST_FULL_SIZE` | ON | also run the comparisons at full texture size |
| `DEBRIS_TEST_SOAK` | OFF | add the ten-minute soak test |
| `DEBRIS_REFERENCE_DIR` | (empty) | reference checkout that enables the optional oracle comparisons |

## Determinism

The generated pictures and the audio must match the reference bit for bit, so the code relies on strict IEEE-754 binary32
arithmetic: no `-ffast-math`, no fused multiply-add contraction (`-ffp-contract=off`, set through `debris::headers`), and the
transcendental functions are implemented in the headers instead of taken from the C library. A test checks that the player
imports none of them from libm, and the `native-march` preset builds with `-march=native` to prove that contraction stays off.

## Licences

The code in this repository is derived from the released werkkzeug3 sources (BSD), v2redux (public domain), the SGI GLU
tessellator and the Arimo and Gelasio fonts (SIL OFL); the production data is CC BY 3.0. The notices are in
[third_party/licenses](third_party/licenses) and [assets/ATTRIBUTION.md](assets/ATTRIBUTION.md), and `debris --licenses` prints them.
