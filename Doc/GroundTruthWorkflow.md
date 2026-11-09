# New Sponza diffuse ground-truth workflow

Implementation of `GroundTruthWorkflowPlan.md`, phases 0–4. Phase 5 (normal-mapped
references, specular, compute-tier qualification and forced-core leaf GI) remains separate.

## Run

Use `build/venv/Scripts/python.exe` with `tools/requirements-gi-quality.txt` and a
prepared New Sponza `.zenrender` launch setup. The source glTF, its external assets
and the prepared `.zenasset` must remain available. No command edits `Data/engine.cfg`.

```powershell
# Feasibility and analytic GPU fixtures, including measured per-term noise.
build/venv/Scripts/python.exe tools/ground_truth_workflow.py benchmark `
  --template=<prepared-newsponza.zenrender> --output=<benchmark.json>

# Write once. Existing versions are rejected, including read-only versions.
build/venv/Scripts/python.exe tools/ground_truth_workflow.py build `
  --template=<prepared-newsponza.zenrender>

# Verify all reference files, source dependencies and reference-tool hashes.
build/venv/Scripts/python.exe tools/ground_truth_workflow.py verify

# The output directory must not exist; its parent must exist.
build/venv/Scripts/python.exe tools/ground_truth_workflow.py run `
  --output=E:/Dev/ZenGroundTruth/runs/my-build --accept-baseline

# Recompute reports from a verified capture without running ZenApp.
build/venv/Scripts/python.exe tools/ground_truth_workflow.py compare `
  --output=E:/Dev/ZenGroundTruth/runs/my-build
```

The default dataset is `$ZEN_GROUND_TRUTH_ROOT/newsponza/v2`, with
`E:/Dev/ZenGroundTruth` as the root fallback. Every command accepts `--dataset`.
`v2` pins earlier revisions of the reference tools, so `verify` now rejects it; pass
`--dataset=E:/Dev/ZenGroundTruth/newsponza/v3`. `v3` was built from the same
template with the current tools, and its frames, setups, inputs, fixtures and truth
arrays are identical to `v2`. The default path is part of a hashed tool, so changing
it requires another dataset version.
Use a new version directory when changing configurations, input assets or the
reference implementation. Publication uses a sibling staging directory, hash
verification, rename, then read-only files. Interrupted `.building-*` directories
are unpublished evidence and are never treated as datasets.

Build defaults are 960 × 540, 256 samples per term, batches of 16, and a 720-second
build budget. `--samples`, `--batch`, `--width`, and `--height` are explicit overrides.
The two independent half estimates retain per-pixel and spatial noise information.
The sky and secondary environment estimators each use one ray from an equal
environment/cosine MIS mixture per sample. Secondary diffuse reflectance uses
linear bitmap albedo, vertex color, and the cache contract's `0.96 * (1 - metallic)`.

## Engine capture

The wrapper launches the standalone `ZenApp.exe` once:

```text
ZenApp --ground-truth=<dataset-directory> --output=<new-run-directory>
```

ZenApp loads the prepared scene once, applies every native setup, resets history,
locks camera input, and waits for GI preparation/refinement. It refuses RTSS-loaded
runs. Capture starts after at least 128 frames and beyond the configured history
length. Two captures 16 frames apart must differ by no more than the reference
noise RMS for each raw term; otherwise it continues in 16-frame increments up to
the recorded limit and fails explicitly if convergence is not reached. Each check also
requires that both the lighting and the hybrid capture were recorded by a submitted
frame. Only the converged pair is written; earlier checks stay in memory. Consecutive
setups that share an environment texture do not reload it.

The qualified configuration uses hardware providers and 128 history frames, making
the earliest accepted pair frames 144/160. The original 32-frame-history `v1`
failed this check on the bright-sky view even at 512 frames. It is retained unchanged;
`v2` carries the revised settings and validated local-light positions. C2, C3, C4
and the wall-facing C5 explicitly carry indoor-view metadata; all five cameras
pass the floor-height, clearance, near-frustum, back-face and close-wall checks.

Lighting records contain ten float4 values per pixel. Components 0–6 retain their
existing materialed diagnostic meanings; 7–9 contain raw `D_direct`, `D_sky`, and
`D_bounce`. The separate hybrid records retain their thirteen-component layout.
Raw capture validity restricts comparison to receivers: deferred pixels and
forward-shaded opaque receiver layers record all ten components. Normal maps,
including clearcoat normals, are disabled through the native `rendering.normalMaps`
field; older setups default to enabled.

The wrapper records executable and shader hashes, exact startup arguments, setup
hashes, elapsed time and capture-file hashes in `run.json`. ZenApp records actual
framebuffer/render extents, fullscreen, backend, async-compute state, resolved
setups and convergence values. Run the wrapper for provenance-checked comparison;
direct ZenApp invocation is useful for capture debugging.

## Reports and interpretation

`report.json` and `summary.md` report surface-match rate and, for every diffuse term
and their sum, relative bias, noise-corrected RMS, p95 error, noise-corrected 8 × 8
block RMS and near/far splits. Block noise uses the half-estimate block means so
spatial covariance is retained. Each frame also has error heat maps and an
albedo-times-diffuse visual check. These display images are not the scored signal.

Each report measures the captured level-zero environment cube against the source
HDR separately. Bright, narrow HDR sun features can differ substantially after
cube conversion even when most directions agree. Consider this fixture when
interpreting sky differences. `noise_below_quarter_error` identifies terms whose
reference noise is small enough for fine comparisons; use a new, higher-sample
dataset before drawing conclusions about smaller changes when it is false.

`--accept-baseline` stores the first accepted report under the run parent's
`baselines/<dataset-hash>/<executable-and-shader-hash>/report.json`. It never
overwrites an existing baseline. Subsequent reports include RMS changes from that
baseline, without arbitrary renderer-quality pass/fail thresholds.

## Verification

```powershell
build/venv/Scripts/python.exe tools/ground_truth_fixtures.py
build/venv/Scripts/python.exe -m unittest discover -s ZenTest/Python -p test_ground_truth_workflow.py
build/x64-windows-msvc-release/bin/RuntimeLaunchTest.exe
```

The GPU fixtures cover open sky, a closed box, directional and point lights,
textured secondary diffuse, and CPU/GPU panorama seam/rotation conventions.
The Python tests cover noise correction, spatial covariance, partial blocks,
empty receiver sets and dataset corruption/path rejection.

### Local qualification, 8 November 2026

- Published dataset: `E:/Dev/ZenGroundTruth/newsponza/v2` (20 validated frames,
  960 × 540, 256 samples per term, immutable files and verified hashes).
- Accepted run: `E:/Dev/ZenGroundTruth/runs/20261108-hardware-v2`.
- Reported times: database 87.5 s, ZenApp 42.7 s, comparison 30.8 s. Hash checks
  and publication add a little overhead outside those reported phase times.
- Surface agreement: 97.45–99.99%. All frames passed the convergence check.
- Re-comparing against the saved baseline produced zero RMS drift.
- ZenApp/shader build, six analytic GPU checks, 18 runtime launch/camera tests,
  and five Python integrity/statistics tests passed.
- The separate `RenderCoreTest` target could not be rebuilt: its existing source
  subset lacks asset-reader/private-header/generated-shader dependencies. Its
  result is not claimed as passed; the actual ZenApp GPU batch exercises the
  new capture path.

On 9 November the wrapper verified `v3` with the current tools and ran it end to end:
68 s in total (ZenApp 35.2 s, comparison 28.2 s), 140 hashed capture files, and
surface agreement unchanged at 97.5–100%.

At this sample count, reference noise is below a quarter of the measured per-pixel
error for all direct terms, three sky terms, and no bounce terms. Noise-corrected
metrics and spatial block results are available, but this baseline does not qualify
small per-pixel bounce improvements. Increase reference samples in a new dataset
version when testing those changes, and re-qualify engine convergence against its
smaller noise bound.
