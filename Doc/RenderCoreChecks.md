# RenderCore check tiers

Choose a tier by what the change can affect. All three write evidence under `build/checks/` (quick, PR) or new `build/production-fix-phase{4,8}/<name>-*` folders (full), and never overwrite earlier evidence. Run them from the repository root on the Windows/MSVC/RTX development host; the PR and full tiers call the local runners in `build/`.

| Tier | Command | Time | Covers |
| --- | --- | --- | --- |
| Quick | `python tools/check_quick.py [--native]` | ~15 s, ~2 min with `--native` | Incremental Release build of RenderCoreTest, all RenderCore tests (mock RHI), clang-format on changed C++ and GLSL files, no-exceptions check; `--native` adds the Vulkan integration and ray-query suites |
| PR | `python tools/check_pr.py` | ~10 min with up-to-date builds, plus rebuild time | Debug and Release builds of every target, all 15 suites per build, 8 trimmed smokes plus 2 previous-frame-radiance GI smokes per build, and 28 deterministic capture pairs against the last qualified binary |
| Full | `python tools/check_full.py [--name N]` | ~3.5 h | The complete production qualification: 96 smokes, freeze, captures, 32 strict resource profiles, 18-asset glTF corpus, benchmarks, static/dynamic/adaptive-light image gates, 720p/1080p timing and the input/test-record audits |

Use the quick tier while editing. Run the PR tier before submitting a change. If its 28 capture pairs are byte-identical and the shader pack is unchanged, rendering did not change in those configurations and the full tier can wait for the next phase close. Run the full tier to close a phase, or when the PR tier reports changed output or shaders. A successful full run records its frozen binary in `build/checks/qualified.json`, which becomes the PR tier's image baseline.

## Trimmed smoke matrix

The full matrix is 24 cases per build (modes 1-3 x inline/threaded RHI x async off/on x vsync off/on), run once with immediate and once with previous-frame radiance. `build/run_production_fix_smoke.py --trimmed` runs eight: every GI thread/async pair, the PBR and voxel views once each, and vsync on twice (`m1-t0-a0-v0`, `m2-t1-a1-v0`, `m2-t0-a0-v1`, `m3-t0-a0-v0`, `m3-t0-a1-v0`, `m3-t1-a0-v0`, `m3-t1-a1-v0`, `m3-t1-a1-v1`). `build/run_delayed_radiance_smoke.py --trimmed` runs the two GI cases `m3-t0-a0-v0` and `m3-t1-a1-v0`, since previous-frame radiance only changes mode 3. `--cases` selects any explicit subset.

## Converged-reference cache

`tools/reference_cache.py` is enabled with `ZEN_REFERENCE_CACHE=strict` (the full tier sets it). `tools/capture_hybrid_gi.py` and `tools/validate_hybrid_gi.py` then key each capture that renders converged references by the executable, every SPIR-V module, the engine configuration written for the capture, the scene and environment inputs, and the native command line with the output folder normalized. A hit copies the earlier outputs instead of re-rendering; only captures with clean logs are stored. Rerunning the full tier on the same frozen build, for example after an interrupted late stage, therefore skips the 65,536-sample references. A new executable or shader pack never hits: references are not reused across versions, because they are that version's own converged output.

Stability, camera-motion and light-change captures render their references in-process after scripted events, so they are cached and replayed as one unit. Entries live in `build/reference-cache/` (override with `ZEN_REFERENCE_CACHE_DIR`); they can be large, and deleting the folder is always safe.

Unattended capture windows ignore the Escape and Tab shortcuts, and closing one fails the run with a logged reason.
