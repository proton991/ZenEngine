# ReSTIR GI Phase 0

2026-11-10. Phase 0 complete. All seven baseline workloads succeeded. No ReSTIR estimator changes
are included. Accuracy remains limited by reference noise as quantified below; Phase 0 does not
establish the later phases' quality acceptance gates.

The starting tree already contained the camera-path and stability-tool drafts. The rendering
prerequisites were committed as `a22f4cdf` (actual-hit materials and visibility), `352c7d7e`
(one shadowed light sampled per secondary hit), and `76d2bac2` (memory-pressure frame limiting).

## Measurement tools

- `tools/gi_stability.py noise` measures mean absolute RGB change between captures 16 frames
  apart, divided by the current term's mean. This is a lag-16 difference, not an average of 16
  adjacent-frame differences. The default final capture is after 256 settling frames. Batch mode
  disables animation and normal maps and fixes the render extent. It does not establish oracle
  convergence: the deliberately loose batch threshold accepts the first pair after warmup.
- `tools/gi_stability.py motion` captures every frame of a scripted orbit/dolly and renders a
  static comparison at each identical pose. Metrics use the maximum absolute RGB channel
  difference per pixel in the displayed 8-bit image. Each static reference also records its own
  lag-16 residual, so stochastic noise is not silently classified as motion lag. The camera path
  contains a key for every moving frame; intermediate application frames interpolate linearly.
- `tools/gi_lamp_reference.py` publishes an immutable imported-lamps + sun dataset using the
  existing independent CUDA estimator at uniformly selected primary receivers. Its sparse
  reference is a diagnostic subset, not a full-image accuracy result. Emission and environment
  lighting are disabled to isolate the imported analytic-light transport.
- ZenApp accepts `--camera-path=<keys.json>` with `--fixed-step`; keys are strictly increasing
  positive scene-frame numbers with finite float coordinates. `--capture-start=N` writes
  `<capture>.frame-N.ppm` and actual-state JSON for every frame from N through `--frames`.
  Existing single final-frame captures retain their behavior.

The stability reports retain executable, shader, setup, source asset, package, environment and
tool hashes, exact launch commands, and actual framebuffer/render extent, fullscreen, backend,
gather and async state. Inputs are checked again after capture. All runs use standalone ZenApp;
`Data/engine.cfg` is not edited. Nsight measurements use `tools/profile_hybrid_gi.py`.

## Evidence

Output root: `E:/Dev/ZenGroundTruth/restir/phase0-20261110/`. `execution.json` records the exact
baseline commands and results. The retained `reproduce/run-restir-baseline.py` runs those workloads sequentially.

The immutable `newsponza/v3-pixel-phase0-20261110` dataset derives all 20 views from `newsponza/v3`
by changing only gather to per-pixel, refreshing setup hashes and retaining the independent
reference arrays unchanged. Its manifest records the source dataset hash. The original version
is untouched. The retained `reproduce/prepare-restir-dataset.py` records the derivation. Choose fresh
dataset/output paths when reproducing; existing immutable versions and evidence are not overwritten.

The lamp dataset `newsponza/lamps-v1` uses the validated C2_L1 camera with all 24 lights from the
4K center setup (one directional and 23 local lights), 512 deterministic receivers and 262,144
samples per receiver. Reference halves are retained for the noise estimate.

Initial noise attempts (`noise-all`, `noise-all-2`) are failed evidence, not baselines. The first
closed before publishing captures; the second explicitly refused capture because RTSS was injected
despite disabling its Vulkan layer. The user closed/excluded RTSS before the final runs.

## Verification

Release ZenApp and affected tests build. RuntimeLaunchTest: 20/20; RenderCoreTest: 780/780;
`SceneTriangleRecordsFollowMasksAndMaterialsAcrossPoseUpdates`: 4/4 with Vulkan validation across
inline/threaded and async off/auto modes. Logs are `build/restir-{runtime,rendercore,cone}-final.log`.
The runtime tests cover path interpolation, held endpoints, ordering, frame zero, malformed JSON,
float overflow and discarding partial parsed state.

Final capture logs contain no error, Vulkan validation or synchronization-hazard messages. This is
not a claim that baseline captures ran with validation enabled: validation is enabled in the GPU
regression, while the timing workload disables it. Capture hashes were rechecked for both accuracy
runs, and executable/shader identity matches across the workloads. Copies of test logs are retained
under the evidence root's `verification/` directory. Modified C++ formatting, Python compilation,
and parent/submodule whitespace checks pass.

The three pre-existing test failures were fixture drift: UV readback used Vec4 instead of the
current Vec2 layout; the forward fixture omitted the basic shader variant; the allocation test
omitted the existing unused-lighting placeholder. Production rendering behavior is unchanged.

## Recorded baseline

Executable SHA-256: `fb2d2c80804c4e852caf958a232b6b2c721d09cca0c35c53756186a5785a5eec`. All measurements use this executable and the same shader set. RTX 5080, hardware RT, per-pixel gather, threaded RHI and async compute available. Normal maps and animation are disabled.

| Check | Baseline |
|---|---|
| 20-view diffuse accuracy, 960×540 | Mean per-view bounce relative RMS 7.79% (noise corrected); per-view bias below |
| Imported lamps + sun, 496 matched receivers | Bounce bias -9.69%; RMS not resolved above reference noise; reference relative RMS noise 97.65% |
| Motion, 8 frames at 960×540 | Mean display error 0.768/255; static-reference residual 0.508/255 |
| 4K GPU frame, 30 frames | Median 94.512 ms; p95 96.251 ms |
| 4K memory, separate uncaptured run | Steady allocation 13.401 GB; peak 15.276 GB |

| Lag-16 noise at 3840×2160 | All 24 lights | Sun only |
|---|---:|---:|
| color | 1.328% | 0.244% |
| specular_gi | 30.190% | 8.416% |
| D_bounce | 2.922% | 0.564% |
| D_sky | 0.000% | 0.000% |
| D_direct | 0.000% | 0.000% |

Noise captures settle for 256 frames, then compare with frame 240. Render extent is 3840×2160; the window framebuffer is 1920×1080. These are fixed-warmup steady-state samples, not proof of noiseless convergence.

| View | Bounce bias | Relative RMS | Reference noise limited |
|---|---:|---:|---|
| C1_L1 | -1.66% | 6.29% | yes |
| C2_L1 | -5.62% | 9.11% | yes |
| C3_L1 | -10.03% | 13.48% | yes |
| C4_L1 | -5.69% | 8.35% | yes |
| C5_L1 | -8.66% | 9.06% | yes |
| C1_L2 | -0.44% | 7.43% | yes |
| C2_L2 | +1.49% | 6.34% | yes |
| C3_L2 | +0.81% | 11.49% | yes |
| C4_L2 | +0.48% | 7.41% | yes |
| C5_L2 | -6.87% | 7.63% | yes |
| C1_L3 | -0.74% | 8.03% | yes |
| C2_L3 | -2.99% | 10.14% | yes |
| C3_L3 | -4.05% | 9.68% | yes |
| C4_L3 | -2.27% | 6.36% | yes |
| C5_L3 | -10.55% | 11.47% | yes |
| C1_L4 | -1.99% | 5.58% | yes |
| C2_L4 | -1.37% | 6.01% | yes |
| C3_L4 | -0.60% | 5.89% | yes |
| C4_L4 | -1.13% | 1.52% | yes |
| C5_L4 | -4.00% | 4.62% | yes |

The 256-sample v3 references are noise limited in the marked views. Retain both the noise-corrected scores and their uncertainty; Phase 0 establishes a comparison baseline and does not claim every view passes a new quality gate.
The lamp reference mean half-difference is 10.37% of its mean. A single pair of halves is a noise diagnostic, not a confidence interval.

The motion path holds for 160 frames, then orbits 0.5° and dollies inward by 0.5% of the eye-target distance over eight consecutive frames. Every pose has a 256-frame static comparison and a frame-240 residual check. This display-space metric includes estimator noise and does not isolate ghosting by itself.

| Motion frame | Mean /255 | p99 /255 | Pixels >4/255 | Static residual /255 |
|---|---:|---:|---:|---:|
| 161 | 0.737 | 8.0 | 2.47% | 0.510 |
| 162 | 0.781 | 9.0 | 2.69% | 0.519 |
| 163 | 0.763 | 8.0 | 2.64% | 0.511 |
| 164 | 0.791 | 9.0 | 2.73% | 0.520 |
| 165 | 0.757 | 8.0 | 2.54% | 0.491 |
| 166 | 0.778 | 8.0 | 2.63% | 0.513 |
| 167 | 0.769 | 8.0 | 2.71% | 0.497 |
| 168 | 0.766 | 8.0 | 2.49% | 0.503 |

Nsight recorded 3840×2160 for both render and framebuffer extents, windowed presentation, threaded
RHI, hardware ray queries, available async compute, GPU indirect culling and no diagnostics UI.
The capture requests base clocks. Pass intervals may overlap and must not be summed into frame time.

| GPU pass | Median ms |
|---|---:|
| HybridTrace | 37.255 |
| SceneLighting | 14.773 |
| HybridFilter1 | 5.468 |
| HybridFilter4 | 4.760 |
| HybridFilter2 | 4.747 |
| HybridFilter3 | 4.561 |
| HybridFilter0 | 3.667 |
| HybridTemporal | 3.122 |
| HybridFilterPrepare | 0.592 |
| HybridSkySupport | 0.226 |
| HybridSkySearch | 0.026 |

Machine-readable results: `baseline.json`, the per-workload `report.json` files, `execution.json`, and `timing/{inputs,summary,application-command}.json` under the evidence root. Raw captures, independent reference arrays and verification logs are retained there or in the immutable datasets. Failed initial attempts are excluded.
