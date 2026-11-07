# Testing

## The runner

```sh
./build/release/debris_tests                  # every suite, a few at a time; no arguments needed
./build/release/debris_tests --list           # the names of the suites
./build/release/debris_tests wz3_ player_     # the suites whose name contains one of the words
./build/release/debris_tests --jobs 2 --verbose
cmake --build build/release --target check    # builds and runs it
ctest --test-dir build/release -j8            # one CTest test per suite
```

[tests/test_main.c](../tests/test_main.c) is the only `main` of the test program. The suites are the functions
`int suite_<name>(int argc, char **argv)` of [tests/suites](../tests/suites); [tests/suites.def](../tests/suites.def) lists them with
their arguments (the directories of the shipped data and of the fixtures), time limit and flags. CMake reads the same file to register
the CTest tests, so adding a suite means writing the function and adding one line.

A suite returns `0` on success, `77` (`DEBRIS_TEST_SKIP`) when it cannot run on this machine and anything else on failure.

On POSIX systems the runner starts every suite in a child process:

* a crash, an `exit()` or a hang (the time limit is enforced) in one suite cannot touch another;
* the output of a suite is captured and printed only when it fails (or with `--verbose`);
* up to `--jobs` suites run at once (default: the number of processors, at most 8); suites marked `SUITE_SERIAL` measure wall-clock
  behaviour and run alone afterwards;
* `DEBRIS_TEST_TIMEOUT_SCALE=N` multiplies the time limits (slow machines, sanitizer builds).

Elsewhere the suites run one after the other in the runner's own process.

Each suite includes the library headers with `*_IMPLEMENTATION` and `*_STATIC`, so it has a private copy of the code it tests and can
use its internals (`wz3__...`, `glr__...`). The one exception is the `linkage` suite: it compiles the implementation into one
translation unit and uses the header without it in another, which is what a user of the single-header libraries does.

## Suites

| Suite | Checks |
|---|---|
| `linkage` | the single-header layout links across translation units |
| `v2`, `v2_alloc` | the V2 player: API, robustness fuzzing, the shipped songs, allocator override, no allocation while rendering |
| `wz3` | core math, KX parser, the bitmap operators against the hashes of the JavaScript port, fuzzing, allocation failures |
| `wz3_dxt5` | the BC3 codec against the vectors of the JavaScript port |
| `wz3_mesh`, `wz3_minmesh`, `wz3_geometry`, `wz3_pose_geometry` | all 10,391 meshes, the 549 minmeshes and the render-ready geometry of the party document |
| `wz3_runtime`, `wz3_host` | events, animation VM and splines; the sinks, progress and cancellation of the precalculation |
| `wz3_text_m2`, `wz3_text_0` | the Text bitmap operator at two texture sizes |
| `wz3_frame`, `wz3_material_view`, `wz3_draw_list`, `wz3_draw_list_frames` | frame jobs, materials, and the draw list of every viewport of every frame |
| `wz3_render_plan`, `wz3_texture_plan`, `wz3_render_boundary` | render planning, texture upload plan, the Bitmap_Render boundary |
| `wz3_shadow`, `wz3_shadow_recipes` | shadow topology and volumes |
| `glr_trace`, `glr_frames` | `glr.h` against a recording GL: the call stream of the reference renderer for the scripted lifetime and for the production frames |
| `glr_egl`, `glr_egl_frames` | `glr.h` on a real driver (headless EGL, Mesa llvmpipe); skip without EGL development files or a driver |
| `player_core`, `player_load`, `player_scene`, `player_scene_egl`, `player_text`, `player_credits`, `player_glcheck`, `player_effects_replay`, `player_args`, `player_replay` | the SDL-free player headers |
| `player_*` (the rest) | the player executable, started headless: `--licenses`, bad arguments, a missing document, seek and replay, fixed-time pictures, the GL accounting, overlays, the short soak, live playback with pause and seeks, cancelling while loading, window resizing |

## Static checks

`ctest` also runs two checks that need no suite: `headers_no_mutable_statics` (no mutable global state in the core headers) and
`no_libm_imports` (the test program imports no transcendental function from libm).

## Pixel goldens

With node and the player available, `golden_320x180` and `golden_270x270` run the player over the times of the Chrome pictures in
[tests/data/goldens](../tests/data/goldens) and compare them with [tools/compare_goldens.mjs](../tools/compare_goldens.mjs). The
renderers are not bit identical (SwiftShader against Mesa or a GPU, canvas fonts against the player's glyph rasterizer), so a picture
passes when it is close; the script's header explains the metric.

## Comparisons with the reference implementation

These run in CTest only. Configure with `-DDEBRIS_REFERENCE_DIR=/path/to/fr-041_debris` (found automatically when this directory is
inside the reference checkout); each part is skipped when its prerequisite (a C++ compiler, node, Chrome) is missing.

| Test | Compares |
|---|---|
| `oracle_wz3_blur`, `_cell`, `_perlin`, `_misc`, `_lookup` | bitmap operators and lookup textures against the C++ oracles |
| `oracle_wz3_dxt5` | BC3 compression against the released `_rygdxt.cpp` |
| `oracle_wz3_shadow` | shadow volumes against `EngMesh::UpdateShadowCacheJob` |
| `oracle_v2_*` | PCM from `v2.h` and from the player's audio layer against v2redux, bit for bit (`DEBRIS_TEST_SLOW=ON` adds the full-length song) |
| `detmath_v8` | the fdlibm ports against V8's `Math` |
| `text_browser_-2`, `_0` | the Text operator against a Chrome canvas, within the calibrated tolerance |
| `fixtures_match_reference` | `tests/data` against the reference's fixtures |

## Sanitizers and `-march=native`

```sh
cmake --preset asan && cmake --build --preset asan && ctest --preset asan -j6     # Clang, ASan + UBSan
cmake --preset native-march && cmake --build --preset native-march && ctest --preset native-march
```

The `native-march` preset enables every instruction set of the build machine, FMA included; the tests still pass because contraction is
switched off explicitly. The Mesa/EGL driver keeps one-time allocations that the leak checker reports; [tests/lsan.supp](../tests/lsan.supp)
and the runner's suppression list cover them.

## Soak

`-DDEBRIS_TEST_SOAK=ON` adds `player_soak_long`: the whole demo about a thousand times in deterministic steps for ten minutes of wall time,
with seeks, resizes and the player's screens in every loop. It fails on GL errors, on GL objects that differ between loops or on memory growth.
