# Reference implementation

This project is a native C99 implementation of a demo that already has two other implementations. They are the reference for
every result the code produces, and the tests are built on them.

| Reference | Where | What it is |
|---|---|---|
| Original source | `vendor/` of the reference repository (`wz3`, `v2redux`, `libtess`, `font3d`) | The released werkkzeug3 C++ (Direct3D 9 era), the public-domain V2 player and the third-party pieces. The authority for arithmetic and behaviour. |
| JavaScript port | `src/` of the reference repository | A plain ES module port of the demo (WebGL2, Web Audio). Ties are decided here where the C++ cannot run (Direct3D, MMX, x87). It is the source of most fixtures. |
| Oracles | `tools/oracles/*.cpp` of the reference repository | Small C++ programs built from the original sources that print what the original computes: blur, cell and Perlin bitmaps, MMX paths, lookup textures, BC3 compression, shadow volumes, V2 PCM. |

The reference repository is `fr-041_debris` (the WebGL port). It is not needed to build, run or test the player. It is needed to
regenerate fixtures and to run the optional comparisons described in [TESTING.md](TESTING.md).

## Which code corresponds to which source

The composition authority of the original is `vendor/wz3/player_demo/demo_oplist.cpp`; the reference repository lists the
correspondence between the original files and the JavaScript modules in `notes/ES_MODULE_MAP.md`. The headers of this project follow
the same boundaries:

| Here | Original source (`vendor/`) | JavaScript module (`src/`) |
|---|---|---|
| `wz3/prologue_detmath.inc` | fdlibm lineage of V8's `Math` | `core.js` (uses `Math.*`) |
| `wz3/core.inc` | `wz3/_types.*` | `core.js` |
| `wz3/bitmap_pixel.inc`, `bitmap_objects.inc`, `graph.inc` | `wz3/genbitmap.*` | `bitmap.js` |
| `wz3/lookup_textures.inc` | `_start.cpp` (`MakeCubeNormalizer`, `MakeAttenuationVolume`) | `legacy_lookup.js` |
| `wz3/texture_bc3.inc`, `texture_levels.inc` | `_start.cpp::UpdateTexture`, `base/_rygdxt.cpp` | `dxt5.js`, `gl.js` |
| `wz3/class_meta.inc`, `kdoc.inc` | compact K reader and class table | `abi.js`, `classes.js`, `kx.js`, `data.js` |
| `wz3/frame_types.inc`, `runtime.inc`, `runtime_tail.inc` | `kdoc.cpp`, `genblobspline.cpp` | `runtime.js` |
| `wz3/mesh.inc` | `genmesh.cpp` | `mesh.js`, `mesh_to_min.js` |
| `wz3/libtess.inc`, `font3d_glyphs.inc` | `libtess`, `font3d` | `libtess.js`, `font3d_glyphs.js` |
| `wz3/minmesh.inc` | `genminmesh.cpp` | `minmesh.js` |
| `wz3/frame.inc`, `scene.inc`, `effects.inc` | `genmaterial.cpp`, `genscene.cpp`, `geneffect*.cpp`, `genoverlay.cpp` | `material.js`, `scene.js`, `effects.js`, `overlay.js` |
| `wz3/render_plan.inc`, `material_view.inc`, `draw_list.inc`, `texture_plan.inc`, `geometry.inc`, `shadow.inc` | `engine.cpp` (Direct3D side) | `renderer.js`, `gl.js` |
| `render/glr.h` | `engine.cpp`, Direct3D materials | `renderer.js`, `gl.js` (GLSL text included) |
| `core/v2.h` | `v2redux/src/*` (after kb's `synth.asm`) | `v2.js` |
| `player/player_core.h` | player lifecycle | `app.js`, `audio_core.js`, `audio.js` |
| `player/player_replay.h`, `player_load.h`, `player_scene.h` | | `app.js` |

Comments in the headers name the original files (`genmesh.cpp`, `kdoc.cpp`, `engine.cpp`, ... in `vendor/wz3`). Three kinds of reference remain, because the original has no counterpart:

* "the JavaScript port" marks behaviour taken from it (JavaScript number semantics, the canvas text adapter, the replay cadence of a seek) where the original cannot run here.
* "the reference renderer" (`glr.h`) is the WebGL renderer of the JavaScript port: the original draws with Direct3D 9.
* The tessellator sections of `wz3/libtess.inc` follow the file layout of libtess (`sweep.c`, `geom.c`, `mesh.c`, `tess.c`, ...), which is the SGI GLU sample implementation.

Generator scripts (`tools/*.mjs`, `tools/*.py`) and the `tests/data` fixtures belong to the reference repository.

## Where the test data comes from

| Here | Made by |
|---|---|
| `tests/data/*.txt` | The `tools/dump_*.mjs` scripts of the reference repository run the JavaScript port and print hashes, vectors and call traces; `tools/regen_fixtures.sh` regenerates all of them. They are copied here unchanged. |
| `tests/data/goldens/<size>/frame_<ms>.png` | Pictures drawn by Chrome (WebGL2, SwiftShader) with the JavaScript port, made by `tools/capture_chrome_goldens.mjs`. |
| `assets/debris_party.kx`, `assets/debris_loader.v2m` | Extracted from the original executable by `tools/extract_kx.py` and `tools/unpack_stage*.py` of the reference repository. |
| generated tables in the headers | `tools/sync_detmath.py` (the fdlibm ports), `tools/sync_font3d_glyphs.mjs`, `tools/generate_font3d_glyphs.py`, `tools/gen_bc3_tables.mjs`, `tools/sync_glr_shaders.mjs`. The blocks are marked `BEGIN ... / END ...`. |

To regenerate a fixture: run `tools/regen_fixtures.sh` in the reference repository, then copy the files it changed from
`native/tests/wz3/fixtures` to `tests/data`. The CTest entry `fixtures_match_reference` reports any drift between the two.

## Why two references

The C++ source is the truth, but several parts of it cannot be compiled here (Direct3D 9, MMX and x87 assembly, the original
tessellator in its Windows-only form). The oracles transcribe those parts to portable C++ and run them on the original data. The
JavaScript port was validated against the original's behaviour and runs the whole demo, so it provides the end-to-end numbers:
the hash of every operator output, the frame jobs of every frame, the GL call stream of the renderer. A native result that equals
both has two independent confirmations.

Where the two differ, the C++ wins for arithmetic and the JavaScript port wins for the data it actually shipped to a browser;
the differences that remain are in the pictures drawn by different GL drivers, which is why the golden comparison uses a
tolerance (mean, share of grossly different pixels, worst 16x16 block) and a control that proves the tolerance can tell a wrong
picture from a right one.

## Further reading in the reference repository

* `notes/ES_MODULE_MAP.md`: original source files against JavaScript modules.
* `notes/PORT_CONVENTIONS.md`: arithmetic and ownership conventions of the JavaScript port.
* `notes/RECONSTRUCTION.md`: how the production data was recovered from the executable.
* `README.md`: the browser port, including the credits and background material.
