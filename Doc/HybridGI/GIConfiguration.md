# GI configuration and the Optimal preset

The editor's **Rendering → GI settings** panel, `Data/engine.cfg` and ZenApp's `--gi-*` arguments now
share one option table (`GetGIConfigOptions` in `VoxelGISettings.cpp`). The default configuration,
**optimal**, gathers screen probes at native render resolution with the world radiance cache.
Device capabilities determine how the probes are traced. [Native 4K measurements](../NativeGIWorldCachePlan.md#native-4k-startup)

## One option list

| engine.cfg key | ZenApp argument | Panel control | Values |
|---|---|---|---|
| `voxel_gi_enabled` | `--gi-enabled` | Enable GI | `true`, `false` |
| `voxel_gi_preset` | `--gi-preset` | Optimal / Custom | `optimal`, `custom`, older presets |
| `voxel_gi_method` | `--gi-method` | Method, All combinations | `per-pixel`, `per-pixel-restir`, `per-pixel-cache`, `per-pixel-restir-cache`, `screen-probes` |
| `voxel_gi_tracing` | `--gi-tracing` | Tracing, All combinations | `auto`, `compute`, `ray-queries`, `rt-pipeline` |
| `voxel_gi_pixel_resolution_divisor` | `--gi-pixel-resolution-divisor` | GI resolution | `auto`, `1`, `2`, `4` |
| `voxel_gi_restir_spatial`, `voxel_gi_restir_spatial_samples` | `--gi-restir-spatial(-samples)` | Spatial reuse, Reuse neighbors | `true`/`false`, `1`–`4` |
| `voxel_gi_field_resolution`, `voxel_resolution` | `--gi-field-resolution`, `--gi-voxel-resolution` | Compute: distance field cells, material voxels | `32`/`64`, `64`/`128`/`256` |
| `voxel_gi_denoiser` | `--gi-denoiser` | Denoiser | `spatiotemporal`, `temporal`, `spatial`, `off` |
| `voxel_gi_bicubic_history`, `voxel_gi_data_driven_anti_lag`, `voxel_gi_anti_firefly`, `voxel_gi_young_receiver_samples`, `voxel_gi_footprint_receivers` | same names | Denoiser details | `true`, `false` |
| `voxel_gi_shadows`, `shadow_map_resolution` | `--gi-shadows`, `--gi-shadow-map-resolution` | Shadow method, Shadow map size | `maps`, `ray-traced`, `ray-traced-reuse`; `128`–`2048` |
| `voxel_gi_indirect_diffuse`, `voxel_gi_indirect_intensity`, `voxel_gi_analytic_lighting`, `voxel_gi_environment_lighting`, `voxel_gi_emissive_lighting`, `voxel_gi_specular_occlusion`, `voxel_gi_reflections`, `voxel_gi_shadow_enabled` | same names | Indirect lighting, Cast shadows | content |

- The ZenApp name is the key without `voxel_gi_`, with `-` for `_`. `ZenApp --help` prints the table.
- **Copy engine.cfg lines** and **Copy ZenApp arguments** export the panel's values.
  **Save as startup defaults** replaces the GI lines of `Data/engine.cfg` (older GI keys included)
  and keeps every other line and comment.
- A test round-trips all 240 method × tracing × denoiser × shadow combinations through engine.cfg
  text and through ZenApp arguments (`EveryCombinationRoundTripsThroughEngineConfigAndZenAppArguments`).
- The older `voxel_gi_mode`, `voxel_gi_restir`, `voxel_gi_temporal`, `voxel_gi_filter`,
  `voxel_gi_rt_shadows`, `voxel_gi_backend` and similar keys still load in engine.cfg. ZenApp accepts
  only the table's names.

## Optimal configuration

- **Configuration keys** (method, tracing, resolution, reuse, denoiser and its details, shadow method,
  field and grid resolutions, shadow map size) are chosen by optimal. Any of them in a config, an
  argument or a panel edit switches to custom, starting from the resolved choice.
- **Content keys** (diffuse and specular GI, intensity, light sources, reflections, cast shadows) stay
  as set in both modes.
- `.zenrender` setups record `rendering.gi.optimal`. Setups written before it load as custom, so
  archived captures keep their configuration. `--gi-preset=optimal` opts an existing setup in. A
  forced `--backend` stays forced, and the rest follows its tier.
- A setup loaded with `optimal: true` takes the current optimal configuration, not its stored fields,
  so later improvements reach saved setups.
- **Code follows the same rule.** `IsOptimalVoxelGIConfiguration` holds only while every configuration
  field keeps its optimal value; a forced provider may differ. Code that edits a field, such as a test
  or the editor draft, gets that configuration.
- **Accessors.** `RendererServer::GetVoxelGISettings` returns the applied configuration as explicit
  settings. `GetRequestedVoxelGISettings` keeps the optimal request for the editor draft.
  `GetEffectiveVoxelGISettings` is resolved, for display.

| Device capability | Optimal choice |
|---|---|
| Usable RT pipeline | Screen probes and world cache, RT pipeline, native-resolution gather, spatiotemporal filtering, ray-traced shadows with reuse |
| Ray queries only or no hardware RT | Screen probes and world cache, compute distance field, native-resolution gather, spatiotemporal filtering, shadow maps |

Forcing `--backend=hardware` on a ray-query-only device selects per-pixel ReSTIR with automatic
resolution. The world cache supports RT pipelines and compute field traversal. Per-pixel automatic
resolution keeps at least 540 GI lines when the view is that large (`ResolvePixelResolutionDivisor`).

Capability decides, never device or driver identity. `ZenApp` and the editor log the result, for example
`GI preset optimal: ray_query_available; method=screen-probes tracing=rt-pipeline
resolution_divisor=1 denoiser=spatiotemporal shadows=ray-traced-reuse`.

## Methods and tracing tiers

| Method | Compute | Ray queries | RT pipeline |
|---|---|---|---|
| Per-pixel | yes | yes | yes |
| Per-pixel ReSTIR | no: reuse needs HW RT | yes | yes |
| Per-pixel + world cache | yes | no: the cache needs the RT pipeline or Compute | yes |
| Per-pixel ReSTIR + world cache | no | no | yes |
| Screen probes + world cache | **optimal without RT pipeline** | no | **optimal** |

When the world cache has zero capacity (including when Vulkan memory-budget reporting is unavailable),
screen probes fall back to per-pixel GI on the selected tracing tier with automatic pixel resolution.
The requested method stays selected so probes can resume when the cache is available.

An unavailable combination still renders: ReSTIR is skipped and cache methods trace exact hits. The
panel names the requirement, and its **All combinations** matrix marks what the GPU runs and the
optimal cell. Selecting a cell sets both the method and the tier.

**Shadow reuse for per-pixel methods.** The coarse 4x4 visibility grid with exact refinement was
reachable only from screen probes. The new `shadowReuse` setting applies it to per-pixel methods.
`nativeShadowReuse` keeps its meaning for screen probes. `voxel_gi_shadows` sets both.

## Historical per-pixel measurements (2026-11-18)

The measurements below used the earlier per-pixel ReSTIR preset. “Optimal” in these results labels
that captured configuration.

New Sponza, `build/checks/rt-accepted-baseline-20261114/capture-1/setup.zenrender` (setup SHA-256
`f3db78c3…c97ecf`), ZenApp 3840x2160 fullscreen, HW tier, async compute off, RTX 5080. Quality comes
from `tools/gi_quick_gates.py --own-references`, which compares each candidate with its own converged
references. Without that option, different methods and resolutions were scored against the first
candidate's settled image. Cost is Nsight GPU Trace at base clocks, frame median.
ZenApp SHA-256 `a79d85ce…6d9427`, shaders `377f8e9f…7b205b21`. Results are under
`build/checks/gi-config/hw-a` (17.4 min) and `hw-b` (14.1 min).

Every candidate uses 4 diffuse samples per GI texel, the RT pipeline unless noted, and quarter
resolution (960x540) unless noted.

| Candidate | Frame ms (p95) | Static flicker | Static RMS | Pan slow | Pan fast | Dolly | Moving sparks | Light /16 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Previous `balanced`: ReSTIR, 2 neighbors, exact shadows | 16.61 (16.74) | 0.0174 | 0.648 | 0.180 | 0.174 | 0.108 | 8.9 | 0.106 |
| + shadow reuse | 15.19 (15.81) | 0.0174 | 0.648 | 0.180 | 0.174 | 0.108 | 8.9 | 0.106 |
| **+ shadow reuse, 4 neighbors (optimal)** | **15.33 (15.42)** | **0.0167** | **0.631** | **0.145** | 0.203 | **0.101** | 9.0 | **0.083** |
| Previous `balanced` on ray queries | 17.44 (17.63) | 0.0175 | 0.648 | 0.179 | 0.175 | 0.108 | 8.9 | 0.105 |
| Optimal at half resolution (cost only) | 28.84 (28.95) | – | – | – | – | – | – | – |
| ReSTIR + world cache, 4 neighbors | 18.61 (18.84) | 0.0224 | 1.368* | 0.229 | 0.449 | 0.147 | 11.6 | 0.342 |
| Screen probes + world cache | 20.99 (21.20) | 0.0364 | 2.179* | 0.580 | 0.605 | 0.493 | 0.9 | 2.494 |
| Optimal with temporal-only denoiser | 13.84 (13.98) | 0.0371 | 0.658 | 0.302 | 0.459 | 0.210 | 53.8 | 0.245 |

Motion columns are the mean percentage of pixels above 4/255 over eight poses. Light /16 is the
percentage above 4/255 sixteen frames after a light move. The optimal candidate measured 15.33 ms again
in the second run.

\* The static reference is the one-bounce matched CUDA image; world-cache methods add bounces by design,
so their static RMS includes that energy difference. Their flicker and motion columns compare with their
own references and are not affected.

**Decision.**
- **Shadow reuse** saves 1.42 ms (direct visibility 4.70 → 2.86 ms) with every quality metric unchanged.
- **The RT pipeline** beats ray queries by 2.1 ms in total: its trace is slower, but ray-query shadows
  cost 5.1 ms inside `SceneLighting`, and reuse needs the pipeline.
- **Half resolution** (28.8 ms), **the world cache** (18.6 ms) and **screen probes** (21.0 ms) exceed the
  16.7 ms budget at base clocks. The cache and probe methods are also noisier in motion here.
- **Temporal-only denoising** saves 1.5 ms but doubles flicker and motion error and sextuples moving
  sparks.
- **Four reuse neighbors instead of two** add 0.14 ms. Against this harness's recorded A/A spread
  (`build/checks/gi-refinement-phase1-quick`: static flicker ±2%, static RMS ±0.1%, dolly ±1.3%,
  light ±2%, pans ±17–25%), four neighbors reduce static flicker (−4%), static error (−3%), dolly
  (−6%) and light recovery (−21%) outside the spread. Both pan changes (slow −19%, fast +17%) are
  inside it.

The earlier per-pixel preset measured 15.33 ms (65 fps) at base clocks on this view, with 1.4 ms of margin.

**Parity.** Three 64-frame fixed-step launches of the final ZenApp (SHA-256 `c9d1a7a4…5f19892`) write a
bit-identical frame 64 (`d9fe06a5…710a01b7`) with 960x540 GI and no error or validation markers:
- the explicit optimal candidate;
- a setup with `optimal: true` and a deliberately different stored configuration;
- an older explicit setup launched with `--gi-preset=optimal`.

The launches are in `build/checks/gi-config/parity-final`. They were repeated on the build with the
seed experiment and the stricter optimal check (ZenApp `e2eb6185…`, `parity-seed-build`) and wrote the
same frame.

### Per-pixel energy with and without the world cache

The quality table above scores noise. It does not show that the per-pixel methods without the world
cache transport one bounce. `tools/native_gi_energy_gate.py` now takes `--world-cache 0|1` and
`--pixel-divisor` in pixel mode. These runs use the matched uniform fixture
(`build/checks/native-gi/implementation/phase1-uniform.zenrender`, 24 lights, no environment lighting)
and its CUDA reference (`phase1-reference-final/reference.npz`), with 3,830 matched receivers.
Each run is 4,096 frames at 3840x2160 with 960x540 GI, ReSTIR with 4 neighbors, the final ZenApp
(`c9d1a7a4…`) and two launches per candidate (`build/checks/gi-config/energy`, `energy.py`). The
energy tolerance is ±1.49%, three sigma of the reference halves.

| Candidate | Mean irradiance | vs 1 bounce (0.004400) | vs 16 vertices (0.010060) | RMS vs 16 vertices |
|---|---:|---:|---:|---:|
| Optimal (no world cache), launch a / b | 0.004315 / 0.004316 | −1.93% / −1.93% | **−57.10% / −57.10%** | 0.00750 / 0.00750 |
| ReSTIR + world cache, launch a / b | 0.009910 / 0.009896 | +125.2% / +124.9% | **−1.49% / −1.63%** | 0.00376 / 0.00376 |

- Without the cache, the optimal configuration matches the one-bounce reference and lacks 57% of the
  16-vertex indirect light. The real-material Sponza reference shows the same 57% deficit (62% on the
  floor; [Denoiser.md](Denoiser.md#footprint-receivers-2026-11-15)).
- With the cache, energy is within 1.5–1.6% of the 16-vertex reference: launch a is inside the
  tolerance and launch b is 0.14 points outside it. Per-receiver error against the 16-vertex reference
  halves, from 0.00750 to 0.00376. Both values are at or below the reference's own half-to-half
  noise (0.00751).
- Adding cached hit shading to per-pixel ReSTIR cost 3.3 ms (18.61 ms frame) and increased
  noise during motion and after a light change. Those measurements led to the earlier per-pixel
  preset with exact one-bounce hit shading.

### Where the world cache's extra noise comes from

`build/checks/gi-config/hw-c` repeats the quality launch with the cache's multi-bounce feedback off
(`worldCacheFeedback=false`). Hits are then shaded from the cache with one bounce, the same light the
optimal configuration carries. That run used `--no-timing`, so it has no cost figure.

| Same harness, own references | Flicker | Pan slow | Pan fast | Dolly | Moving sparks | Light /16 |
|---|---:|---:|---:|---:|---:|---:|
| Optimal: exact one-bounce hit shading | 0.0167 | 0.145 | 0.203 | 0.101 | 9.0 | 0.083 |
| Cache, one bounce (feedback off) | 0.0162 | 0.138 | 0.200 | 0.100 | 9.3 | 0.195 |
| Cache, multi-bounce (feedback on) | 0.0224 | 0.229 | 0.449 | 0.147 | 11.6 | 0.342 |

- **The cache lookup adds no static or motion noise.** With one bounce it matches exact hit shading
  within the noise band. Its static error against the one-bounce reference is 5% higher (0.663 versus
  0.631), which is the cost of cell quantization.
- **The extra noise comes with the extra light.** Feedback adds 125% more indirect light, so the
  converged frames at the reference poses are 12% brighter (mean 8-bit level 12.6 → 14.1). Flicker
  rose 35%, less than the indirect light it carries. The quality metrics count absolute 8-bit
  differences, so a brighter indirect term scores higher.
- **Motion error with feedback** (fast pan 2.2x) has a likely cause that was not isolated. Cells
  entering view, or changing level with camera distance, restart short histories, and the feedback
  bounce is their noisiest part.
- **Light recovery is slower with any cache.** Exact hit shading uses the new light on the next frame.
  Cached lighting restarts its history on the light signature, which makes recovery 2.3x slower with
  one bounce. Feedback then propagates one more bounce per cache update, which makes it 4.1x slower.
- **The RTXGI-style noise reductions are not implemented.** SHaRC reduces noise relative to
  path-tracing the extra bounces. The uses that would reduce this renderer's noise were planned as
  [GI refinement Phase 3](../GIRefinementPlan.md#phase-3-cache-driven-fixes-for-motion-and-lighting):
  seeding disoccluded receivers from the cache, and cache lighting gradients as history confidence.
  Work stopped after Phase 2. The cache currently only shades hits.

### Native world cache

The cache starts one path from a sampled G-buffer pixel per tile. Hardware records triangle hits in the RT pipeline and shades them in compute. Compute traversal uses the distance field. Both tiers share the directional irradiance records, parent fallback and temporal resolve. The renderer applies each hit's own reflectance and emission.

| Setup field | ZenApp argument | Default | Supported range |
|---|---|---:|---:|
| `worldCacheTileSize` | `--gi-world-cache-tile-size` | 8 pixels | 6–16 |
| `worldCacheHistory` | `--gi-world-cache-history` | 128 updates | 1–128 |
| `worldCacheUnusedFrames` | `--gi-world-cache-unused-frames` | 240 frames | 1–4096 |

A smaller tile launches more fill paths. The history cap counts frames that supplied samples to a cell. Lowering the eviction age frees hash slots for reuse within the allocated table.

### Experiment: seeding disocclusions from the world cache (Phase 3 item 1)

`voxel_gi_world_cache_seed` (`worldCacheSeed`; panel: Denoiser details, cache methods only) is
experimental and off by default.
- **Cache record.** The cache gets a seventh record per cell: returning surface radiance projected along the
  sampled vertices' shading normals, without the cell's direct light or sky escapes. The record count is
  `uWorldCacheData.lightCount.y`, so without the seed the layout, capacity and images are unchanged.
- **Denoiser.** `GIDenoiseTemporalAntiLagBicubicSeed{32,64}` starts a receiver with no diffuse history
  from that record when the matching cell and geometric face have at least 32 samples. The seed is
  weighted as four frames and includes the indirect intensity.

`build/checks/gi-config/seed-a`: per-pixel ReSTIR + world cache, quarter resolution, 4 neighbors,
shadow reuse, own references.

| | Frame ms | Flicker | Pan slow | Pan fast | Dolly | Moving sparks | Light /16 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Cache | 18.85 | 0.0218 | 0.235 | 0.456 | 0.154 | 13.1 | 0.348 |
| Cache + seed | 18.85 | 0.0224 | 0.291 | 0.478 | 0.149 | 11.8 | 0.332 |

**Result: no improvement.**
- Slow pan is 24% worse; at pose 8 the error is 1.9x.
- Fast pan is inside its A/A spread: pose 8 is 36% better, pose 64 is 39% worse.
- Dolly (−3%) and sparks (−10%) improve slightly; static flicker and light recovery are unchanged.
- Likely causes, not isolated:
  - **Bias.** The seed is a cell average along the cell's mean normal, so it differs from the
    receiver's own converged value. At four frames' weight that bias stays visible in slow pans.
  - **Wrong pixels.** The fast-pan error sits in revealed receivers whose reprojection was accepted and
    whose histories are 1–8 frames old ([starting point](../GIRefinementPlan.md#starting-point)).
    The seed only replaces a missing history.
  - **Immature cells.** Cells for newly revealed surfaces may not have 32 updates yet.
- Next candidates: weight the seed by cell maturity and normal agreement; blend it into young
  histories (age < 4) instead of only missing ones; evaluate it per receiver normal, which needs
  indirect-only directional records.

### Limits of the November 18 measurements

- The Compute choice and the automatic resolution below 2160 lines are rules, not measurements.
- One view of one scene was measured.
- A/A was not rerun; the spread comes from the earlier quick-gates run of the same harness at 1080p GI.
- No full test suites were run. The runs were `RenderCoreTest` (`VoxelGIRuntimeSettings.*`, 29 tests),
  `UIDrawPacketTest` (19) and `RuntimeLaunchTest` (22), all passing, plus the ZenApp launches above and
  an editor capture of the panel with Sponza loaded (`build/checks/gi-config/editor/editor-scene.ppm`).
