# Render pipeline hardening plan (2026-11-13)

## Progress at pause — 2026-11-14

**Work is paused at the user's request. The whole plan is not complete.** The main
Phase 1–4 implementation is present, but several acceptance gates remain unmet.
Recent work was quality/performance tuning; the current capture and its helper
were stopped. No optimization or application run should resume without a new request.
This update records progress; it does not waive the requirements below.

| Phase | Implementation | Qualification / remaining work |
|---|---|---|
| 0 — baseline | Complete: promoted tools, corrected pan, frozen presets and two-repeat gate matrix | Baseline recorded; reference is the supplied sparse ZenApp Monte Carlo dataset, not independent Mitsuba output |
| 1 — removals | All six removal groups applied; settings, UI, config/setup loading, registrations and tests updated | Strict Compute image equivalence remains open; field-publication timing and permitted lighting/resolution changes are documented |
| 2 — denoiser | All seven substeps implemented behind `relaxDenoiser` | Static limits pass once on all three presets; eight-pass cost is about 6.98 ms in the latest warm HW capture. Full motion, sparks, repeats and lighting-response qualification remain open |
| 3 — RT foundation | Implemented and validated: RHI/RDG, SBT, shader stages, direct/indirect tracing, opacity and optional EXT/NV reordering | Native foundation checks pass; NV variants compile but this device selected EXT for runtime checks |
| 4 — HW ports | Primary GI, ReSTIR visibility, receiver shadows, light-occluder cache, guides/counts and approved forward fallback implemented | RT pipeline kept as the HW tier (user decision). The timing gate was amended to a no-regression budget against the accepted RT baseline: replaced passes ≤ 21.915 ms, HW frame ≤ 33.018 ms, frozen from three RTSS-free repeats. The ray-query comparison (+4.9 ms) stays on record ([RT optimization](#rt-pipeline-decision-and-optimization-2026-11-14)). Strict query/RT A/A parity and diffuse-hit reflection-reuse qualification remain open |
| 5 — defaults/removal | Not performed | New paths remain opt-in; old denoiser and query reference paths are retained |
| 6 — close-out | Capability fallback chain, pass-list pins, device-memory limit option and the field-pacing fix landed on 2026-11-14 ([resumed work](#resumed-work-2026-11-14)) | 8 GB runs pass for HW and, after the local-field fix, Compute. Forced-fallback ZenApp runs, remaining robustness matrix and full final verification remain open |

The [implementation record below](#implementation-record-at-pause) lists retained
changes, measurements, evidence and rejected experiments. The full chronological
run ledger remains in [Denoiser.md](HybridGI/Denoiser.md), including intermediate
failures, expected differences and superseded candidates. Historical statements
there describe their dated revision, not necessarily the current code.

This is the last planned rendering work before moving to other subsystems (physics, VFX). It
reduces the renderer to two GI tiers built on one denoiser. It also replaces in-shader ray queries
with full Vulkan ray-tracing pipelines on hardware that supports them.

Decisions taken on 2026-11-13:

- **Tiers:** keep HW RT and one Compute fallback: the distance-field GI with the voxel radiance
  cache. Without either, the renderer falls back to plain PBR.
- **HW RT:** the HW tier uses `VK_KHR_ray_tracing_pipeline` (raygen, hit and miss shaders, SBT)
  instead of `GL_EXT_ray_query` inside compute and fragment shaders.
- **Screen probes:** removed. Pixel gather with reduced-resolution GI is the only gather.
- **Denoiser:** a new ReLAX-style chain replaces the SVGF chain (temporal accumulation and five
  à-trous iterations) for both tiers.

## Why: measured starting point

Evidence: `build/gi-noise-20261113/FINDINGS.md` (New Sponza, ZenApp, 4K fullscreen, RTX 5080).

### Current frame flow (half-resolution GI preset, frame 300)

| Stage | Passes | Notes |
|---|---|---|
| G-buffer | `OffScreen`, then `HybridDownsample` | |
| Trace and reuse | `HybridTrace`, `ReSTIRTemporal`, `ReSTIRSpatial0/1` | |
| Denoise | `HybridTemporal`, `HybridFilterPrepare`, `HybridFilter0..3`, `HybridSkySupport`, `HybridFilter4`, `HybridSkySearch` | About 11.9 ms |
| Upsample | `HybridReconstruct`, `HybridReconstructFallback` | About 2.5 ms |
| Lighting | `SceneLighting` (fragment ray queries for shadows) | |
| Forward and output | Forward transparency, `ForwardToneMap` | |

The denoise and upsample stages cost about 14 ms at 1920x1080 GI. Nothing temporal runs after
the upsample.

### Where the visible noise comes from

- **Camera motion is the main source.** Over a 0.25°/frame pan, 3.5% of pixels differ by more
  than 4/255 from the settled image (half preset). The share is 7.8% with the editor defaults and
  8.3% with the filter off.
- **The static image is nearly clean** in the half preset: mean flicker 0.03/255. It is 10x worse
  with the editor defaults (native resolution, ReSTIR off) and 7x worse with screen probes.
- **Fallback receivers flicker by design.** These are receivers the upsample cannot match to a GI
  texel. They are traced fresh every frame with no history, which leaves flickering edges.

### Why SVGF fails here

- Its variance-driven filter barely filters when history is short, which is exactly what happens
  after a disocclusion or while moving.
- History caps of 4–8 frames during motion restart reflections constantly.
- ReSTIR's correlated reservoirs break its variance model. The current code compensates with the
  fixed divisors `/4` and `/20`.

## Target frame flow

```
HW RT tier                                   Compute tier (same stages unless noted)
---------------------------------------      ----------------------------------------
G-buffer (OffScreen) + GI downsample          + compute voxelizer, radiance inject, shadow maps
[RT] GI trace raygen      (GI resolution)     field trace (compute), no ReSTIR
[RT] ReSTIR temporal + spatial reuse          -
Denoiser (GI resolution, diffuse + specular):
  pre-blur -> temporal (slow + fast history) -> history fix -> a-trous x3
Upsample to output (no un-historied fallback)
Output-resolution GI stabilization
[RT] shadow visibility (one pass per frame)   shadow maps
SceneLighting (reads visibility, no ray queries)
Forward transparency, tone map, present
```

The pass budget for the denoise and upsample stages is at most 8 passes (11 today) and at most
7 ms at 1920x1080 GI (about 14 ms today).

## Phases

Phases 2 and 3 touch disjoint files and can run in parallel. Every other phase depends on the
one before it.

### Phase 0: baseline and gates (no engine change)

1. Promote `build/gi-noise-20261113/noise_probe.py` to `tools/gi_noise_probe.py`. Fix the
   last-key anomaly first: pose 64 at frame 224 measured about 0 error in every configuration.
   Then extend the pan to two speeds, 0.25°/frame and 1°/frame, plus a dolly.
2. Freeze the presets. **HW:** half-resolution GI, ReSTIR on, all features HW. **Compute:**
   field traversal at native resolution with automatic half-resolution diffuse, as today.
   **Editor default:** currently native resolution with ReSTIR off.
3. Record the baseline for each preset:
   - static flicker
   - pan error
   - static RMS against `E:/Dev/ZenGroundTruth/restir/matched-4k-20261112-v2`
   - sparks (`tools/gi_matched_sparks.py`)
   - an Nsight GPU Trace of the frame (`tools/profile_hybrid_gi.py`)
   - the RDG pass list
4. Gate definitions for every later phase. "Within A/A" means within the noise band measured by
   running the same configuration twice.

   | Gate | Required result |
   |---|---|
   | Static RMS against the reference | Not worse than baseline beyond A/A noise |
   | Sparks | Not worse than baseline |
   | Pan error | Pass threshold set in phase 2 |
   | Frame time | Not worse than baseline, except where a phase states a budget |
   | Vulkan validation | Clean |
   | Four submission-mode GPU suites | Pass |

### Phase 1: remove what the target flow does not use

Do the removals in this order. After each step, the HW and Compute preset captures must stay
byte-identical, or show the documented expected difference.

| Remove | Includes |
|---|---|
| Screen-probe gather | `ScreenProbeGather`, 34 `probe_*` shaders, `Gather` and `probeSpacing` settings, UI, config keys, tests |
| Cone voxel GI | Cone bounce, `cone_trace.glsl`, `BounceSource::Cone`, cone settings, coarse cone preview during field preparation (show PBR plus environment until the field is ready) |
| Geometry (raster) voxelizer | `GeometryVoxelizer`, voxelization `.vert/.geom/.frag`, `VoxelizerMode`; the Compute voxelizer stays and feeds the Compute tier's radiance cache |
| Half-diffuse path | `DiffuseResolution`, `HybridTraceHalf`, `HybridDiffuseUpsample`, `HybridTraceFallback`, fallback buffers. First extend `pixelResolutionDivisor` to the Compute tier, which today requires HW for all features (`HybridGIRenderer.cpp:453`); gate the Compute preset at divisor 2 against its half-diffuse baseline, then delete |
| Per-feature mixed providers | The six per-feature selectors become one tier choice (HW or Compute); removes the `*_field` HW variants, `inject_hardware_radiance*` and `sky_irradiance_hardware*` |
| Unqualified trace variants | Buffered HW guide variants kept only for experiments, `--hybrid-trace-*-module` overrides, `adaptiveSampling`, `sky_irradiance_legacy`, `previousFrameRadiance` |

Keep:
- the reference estimator, ground-truth capture and `HybridCapture`
- ray-count diagnostics, folded into one optional variant
- the debug views; retarget them to the new denoiser's stages in phase 4

Update `RenderingSettings`, the editor panels, `engine.cfg` loading and ZenTest. Note the tests
removed with each feature.

### Phase 2: new denoiser prototype (behind a setting)

New program family `GIDenoise*`. The old chain is kept until phase 5.

1. **Signals.** Demodulated diffuse (sky + bounce) and specular radiance, plus hit distance, at GI
   resolution. Inputs reuse the existing receiver guides, motion and the virtual-motion
   reflection reprojection.
2. **Pre-blur.** A single pass, before temporal: a Poisson-disk blur whose radius depends on hit
   distance and history length.
3. **Temporal.** Bilinear reprojection with the existing identity, plane and normal tests:
   - Each signal keeps a slow history (up to `historyFrames` × divisor²) and a fast history
     (2–4 frames).
   - The slow history is clamped to the fast history's mean ± kσ. This replaces the hard
     motion caps of 4–8 frames and the radiance-cache delta logic where clamping covers it.
4. **History fix.** Receivers with history under 4 frames are filled from a small mip chain of
   the current frame, using plane and identity weights. This replaces `HybridSkySupport` and
   `HybridSkySearch` and the early-frame 3x3 variance estimate.
5. **À-trous ×3** (steps 1, 2, 4):
   - Blur radius and strength scale with 1/√(history length).
   - Edge stopping uses plane distance, normal, and a luminance weight from the fast history's
     variance, not from ReSTIR-corrected moments.
   - Specular weights respect lobe and roughness.
6. **Upsample.** Keep the `HybridReconstruct` tests. Receivers with no support take the history-fix
   result of their nearest GI texel instead of the fresh `HybridReconstructFallback` trace.
7. **Stabilization at output resolution.** A temporal pass on the upsampled GI, with history
   clamped to the 3x3 neighbourhood. It gives edge and bevel receivers a history.

Gates at both tiers' presets and the editor default:
- pan error (% of pixels > 4/255) down at least 50% at both speeds
- static flicker not worse
- static RMS not worse beyond A/A noise
- sparks not worse
- cost at or under 7 ms (half-resolution GI, 4K)

Record each run in `Doc/HybridGI/Denoiser.md`.

### Phase 3: ray-tracing pipeline foundation (RHI, RDG, shaders)

| Area | Work |
|---|---|
| RHI | `RHIPipelineType::eRayTracing`; ray-generation, miss, closest-hit, any-hit and callable stages; RT pipeline creation with shader groups; the pipeline cache; shader binding table buffers with handle alignment; `TraceRays` and `TraceRaysIndirect` commands; device limits and capability reporting (no device or driver IDs) |
| RDG | A ray-tracing pass type with bindings and barriers like compute passes, the `eRayTracingShader` stage, and async-compute eligibility |
| Shaders | Build `.rgen/.rmiss/.rchit/.rahit` with `glslangValidator --target-env spirv1.4`; reflect descriptor sets across all pipeline stages |
| Opacity | Alpha-tested geometry uses an any-hit shader and the existing opacity micromaps |
| Shader execution reordering | Use it where the device reports support (NVIDIA or EXT invocation reorder), selected by capability; otherwise plain `traceRayEXT` |
| Tests | RHI unit tests for SBT layout and alignment; an integration test that an RT-pipeline trace of the calibration scene matches the ray-query checks (`scene_ray_query_check`, micromap checks) across the four submission modes with validation on |

### Phase 4: port HW tracing to ray-tracing pipelines

1. **GI trace.** One raygen at GI resolution traces diffuse and reflection rays and
   writes the reservoirs and raw signals:
   - Hit groups evaluate the material and hit lighting (`ray_hit_lighting.glsl`) in closest-hit.
   - Shadow rays from hits use a terminate-on-first-hit miss-only group.
   - The 128-register limits of the compute-shader trace no longer apply.
   - Re-qualify stage 4's diffuse-hit reflection reuse.
2. **ReSTIR visibility.** Reuse visibility in the temporal and spatial passes moves to a
   raygen pass. Evaluate merging the two spatial passes into one.
3. **Shadows.** A per-frame raygen writes light visibility for the receivers. `SceneLighting` reads
   it and drops its fragment ray queries (`hybrid_gi_shadows*.frag`). Re-evaluate whether the
   `LightOccluders` candidate cache is still needed once any-hit shaders and opacity micromaps are
   in place.
4. **Remaining ray queries.** Forward transparency keeps fragment ray-query shadows (per-layer
   fragments cannot use a screen raygen); this is the one documented exception. Calibration
   shaders keep ray queries as reference checks.
5. **Gates.** The image matches the ray-query path within A/A noise; quality gates as in phase 2.
   Timing is measured on warm launches (see the cold-compile note in `HybridTraceProfile.md`).

   **Timing gate amended 2026-11-14 (user decision).** The original requirement, that the GI
   trace plus shadow passes be no slower than the ray-query passes they replace, is withdrawn. The
   RT pipeline stays the HW tier with its measured cost accepted ([RT optimization](#rt-pipeline-decision-and-optimization-2026-11-14)).
   The replacement gate is now a no-regression budget against the **accepted RT baseline**:
   - The replaced passes (`HybridTrace`, `ReSTIRTemporal`, `ReSTIRSpatial0/1`, `ReceiverVisibility`,
     `SceneLighting`, `LightOccluders`) must not exceed the baseline's worse repeat.
   - This phase states the HW whole-frame budget. Under the Phase 0 rule "except where a phase
     states a budget", it replaces the frozen ray-query limit (31.7262 ms) for the HW preset. Apply
     it with `tools/gi_noise_gates.py --frame-budget hardware=<summary>,<summary>`.
   - The ray-query cost stays on record as a comparison (+4.9 ms for the replaced passes), not a gate.

   | Accepted RT baseline (ms) | Repeat 1 | Repeat 3 | Repeat 4 | Budget (worse) | Repeat 2 (slow launch) |
   |---|---:|---:|---:|---:|---:|
   | Replaced passes | 21.821 | 21.915 | 21.890 | **21.915** | 23.065 |
   | Whole frame | 32.925 | 32.961 | 33.018 | **33.018** | 34.747 |
   | Eight denoise/upsample passes | 6.960 | 6.941 | 6.953 | ≤ 7 (Phase 2, unchanged) | 7.399 |

   Source: `build/checks/rt-accepted-baseline-20261114`, four captures taken with RTSS closed:
   ZenApp SHA256 `559f2be3…`, setup `f3db78c3…`, 3840x2160 fullscreen, 1920x1080 GI, base clocks,
   async compute on. `accepted-baseline.json` records the budget, and `gates-amended.json` is the
   gate tool's output (`--frame-budget` with repeats 1, 3 and 4), which keeps the frozen 31.7262 ms
   on record.
   - **The slow launch is recorded but not budgeted.** Repeat 2 is slower on all 30 frames. Its
     `ReceiverVisibility` sits at 5.2–5.4 ms against 4.66 ms in every other launch, which points to
     a per-launch driver compile state rather than the renderer; the driver recompiles each launch
     because its shader cache is full. About 1 launch in 9 of this code landed in that state.
     Budgeting it would loosen the budget by 1.7 ms.
   - **Candidates in that state are re-run, not counted.** Judge a candidate on repeated launches; a
     launch that is uniformly slow across all its frames and passes should be re-run.
   - The two earlier repeats taken while RTSS ran (32.867 / 33.177 ms) are kept in
     `build/checks/rt-accepted-baseline-20261114-rtss-hooked`. `gi_noise_gates.py` refuses
     RTSS-hooked summaries as budget sources.

### Phase 5: switch defaults and delete the old chain

1. Make the new denoiser and RT pipelines the defaults.
2. Change the editor defaults to the qualified preset: half-resolution GI with ReSTIR on.
3. Delete:
   - `HybridTemporal*`
   - `HybridFilterPrepare`, `HybridFilter0..4`, the cached and final filter variants
   - `HybridSkySupport`, `HybridSkySearch`
   - `HybridReconstructFallback`
   - the ray-query HW trace variants (`hybrid_trace_*hardware*`, `restir_*` ray-query visibility)
   - the related settings, uniforms and tests
4. Re-run the phase 1 dead-code audit: shader includes, program registrations, and dynamic
   program names.

### Phase 6: hardening and close-out

- **Frame flow.** Document the final pass list for each tier: names, resolutions, queues and
  budgets. Add an RDG test that pins the HW and Compute pass lists so new passes are deliberate.
- **Capability fallback.** RT pipeline, then Compute field, then PBR, chosen by capability. Each
  fallback reports its reason in the status bar and the logs. Test each step with forced
  capability masks.
- **Robustness.** Cover resize, history reset reasons, scene reload, device-memory pressure
  (8 GB budget) and `historyId` multi-view in tests.
- **Full verification.**
  - ZenTest suites in all submission modes, with validation on
  - 4K ZenApp quality and timing gates for both tiers
  - the editor rendering tests
- **Docs.** Update `Doc/HybridGI/README.md`, `HybridTraceProfile.md` and `HybridTraceQuality.md` to
  point here. Rendering is then closed apart from bug fixes.

## Out of scope

- Full-scene TAA or upscaling. Only GI gets the output-resolution stabilization pass.
- Path-traced reference renderer changes (Mitsuba remains the ground truth).
- Transparency, refraction and voxel-debug passes, other than removing what dies with the
  features above.
- New lighting features.

## Risks

| Risk | Mitigation |
|---|---|
| RT pipeline trace slower than ray query on some scenes (SBT overhead, extra stack) | Phase 4 timing gate; keep shader execution reordering optional; measure on warm launches |
| Fast-history clamping adds lag or ghosting on lamp lighting | Pan gate at both speeds, light-path test (`--light-path`) |
| Removing cone preview leaves an unlit scene during field build | PBR and environment until the field is ready; record build time |
| Removing per-feature providers loses a debugging tool | Tier switch remains; per-feature A/B lives in experiment branches only |

## Resumed work 2026-11-14

Four Phase 6 items that need no captures. None of them changes a quality or timing measurement, and
no gate threshold changed.

Native verification (release, Vulkan validation on, `build/checks/hardening-fixes-1`): RenderCore
783 of 792 pass (9 disabled); the ray/field suite 120 of 136 (4 fixture skips, 12 disabled);
Vulkan RHI integration 510 of 515 (5 existing device-limit skips); editor rendering 77 of 79
(2 disabled); focused denoiser checks 52 of 52. No validation hazards and no leaks were reported. No
ZenApp run or capture was made.

### Field construction pacing

Throughput construction compared the whole frame interval with an absolute 16 ms budget and halved
its batch toward one triangle whenever the frame was slower. Phase 0's 4K Compute frames took
83–85 ms, so the batch collapsed and later local generations never published (the generation 5
stall in the [implementation record](#phase-1-completed-removal-changes)).
`PaceFieldThroughput` (`VoxelResourcePlanning.h`) now also admits
`throughputFrameBudgetShare` (0.25) of a measured render-only interval. When a generation runs over
the absolute budget, it first records no construction for eight frames to measure that interval;
fast frames never probe. `FieldThroughputPacing.*` unit tests model 83 ms frames and show the old
budget starving and the new one finishing the recorded 215,361-triangle generation. Not yet measured
in ZenApp. Captures stay non-deterministic: publication still follows frame time, so the strict
Compute removal identity needs a settle-then-reset capture mode (open).

### Capability fallback: RT pipeline, then Compute, then PBR

- `GetHardwareTierUnavailableReason` resolves the HW tier by capability only: usable ray queries,
  plus RT pipelines with a second recursion level when `rayTracingPipeline` is set. Auto resolves to
  Compute with `ray_query_unavailable` or `rt_pipeline_unavailable`.
- When Auto resolved to HW (`RendererServer` calls `VoxelGIRenderer::SetAutomaticTier`), a persistent
  scene failure (`IsPersistentHardwareTierFailure`: missing features, AS budget, scene preparation or
  hit-stream allocation) moves the tier to Compute until settings, scene or tier choice change.
  Pending builds and failed frames retry. An explicit HW tier still stays unavailable (PBR) with its
  reason.
- The log reports the fallback once. The status bar shows "Compute GI fallback" with the reason; PBR
  frames during field preparation say "HW RT unavailable (<reason>)"; the GI selection reason becomes
  `hardware_failed_compute_fallback`. ZenApp's summary records `tierFallback`.
- Tests: `RenderCoreTest.AutomaticTierFallsBackFromHardwareToCompute` forces each step on the mock
  RHI (allocation failure, missing RT pipelines, capable RT pipelines, explicit-tier failure);
  `VoxelGIRuntimeSettings.AutomaticTierRequiresTheConfiguredHardwarePipeline` covers the policy.
  ZenApp runs with forced capability masks (for example `--disable-rt`) remain to be recorded.

### Device-memory limit

`RHIOptions::SetDeviceMemoryLimit` (ZenApp `--device-memory-limit-mb=N`) caps every device-local
heap through VMA's heap-size limit, so budgets and allocations behave as on a smaller GPU and every
budget-driven choice sees it. `VulkanProductionFailureTest.DeviceMemoryLimitCapsHeapsBudgetsAndAllocations`
checks an 8 GB cap's heap sizes and budgets, and that a small cap stops device-local commitments.
The HW preset at 4K committed 9,622 MiB in the latest capture, so the 8 GB ZenApp run
(`--device-memory-limit-mb=8192`) is expected to show pressure; it has not been run.

### Final frame flow (target configuration, pinned)

`RayQueryIntegrationTest.HybridGIFramePassListsArePinned` pins the steady-state live passes of both
tiers, in recording order, in all four submission modes. It uses ReLAX at divisor 2; HW also uses RT
pipelines and ReSTIR. Its fixture is a static scene with one directional light, environment lighting
and transparent layers. Shadow maps, the field, voxelization and the radiance cache are current there,
so only per-frame work is live. Scene content adds passes: transmission (`InitializeRefraction`),
scattering media and a disabled environment guide change the list. All pinned passes plan on the
graphics queue; async compute carries only construction work.

| Pass | HW RT | Compute | Resolution | HW 4K ms |
|---|:-:|:-:|---|---:|
| `SkyboxDraw`, `GBufferFrustumCull`, `OffScreen` | ✓ | ✓ | Output | 1.52 |
| `LightOccluders` | ✓ | | Light candidates | 0.04 |
| `HybridDownsample` | ✓ | ✓ | Output → GI | 0.32 |
| `HybridGuidePrepare`, `HybridGuideFinish` (environment guide) | ✓ | | GI | not in capture |
| `HybridTrace` (raygen on HW, field trace on Compute) | ✓ | ✓ | GI | 8.66 |
| `ReSTIRTemporal`, `ReSTIRSpatial0`, `ReSTIRSpatial1` | ✓ | | GI | 0.54 / 4.11 / 4.16 |
| `GIDenoisePreblur`, `GIDenoiseTemporal`, `GIDenoiseHistoryFix` | ✓ | ✓ | GI | 0.75 / 0.76 / 0.73 |
| `GIDenoiseAtrous0..2` | ✓ | ✓ | GI | 0.92 / 0.89 / 0.83 |
| `HybridReconstruct`, `GIDenoiseStabilize` | ✓ | ✓ | Output | 1.27 / 0.83 |
| `ReceiverVisibility` | ✓ | | Output | 4.66 |
| `ForwardBackground`, `SceneLighting`, `ForwardOpaqueDisplay` | ✓ | ✓ | Output | 0.15 / 1.02 / n/a |
| `ForwardHiZ` (one per mip: 7 in the fixture, 12 at 4K), `ForwardCull`, `ForwardPrefix`, `ForwardCullScan`, `ForwardEmit` | ✓ | ✓ | Output mips | 0.12 HiZ |
| `ForwardTranslucent`, `ForwardToneMap` | ✓ | ✓ | Output | 1.52 / 0.05 |

HW times are the medians of `build/render-hardening-phase4-compact-visibility` (3840x2160,
1920x1080 GI, New Sponza). Budgets: the eight denoise/upsample passes ≤ 7 ms (6.98 ms there); the
RT trace, ReSTIR and visibility passes must not exceed the query passes they replace (open:
about +6 ms). No Compute capture with the retained denoiser exists, so its column has no times.

### ZenApp checks (2026-11-14)

All runs use the canonical `ZenApp.exe` (SHA256 `59d95a1a…c4ecc77091`), New Sponza, 3840x2160
fullscreen, threaded RHI, async compute on. Each run folder keeps its exact command, setup hash
and log.

**RT pipeline vs ray queries at the current revision** (`build/checks/rt-pipeline-ab-20261114`).
`tools/profile_hybrid_gi.py` ran four warm Nsight captures, alternating, at base clocks with
validation off: 30 frames each after 60 s. The setups are the compact-visibility HW preset (ReLAX,
ReSTIR, divisor 2) with `rayTracingPipeline` on or off; everything else is identical. Repeats agree
within 0.3 ms.

| Median ms (mean of 2 repeats) | RT pipeline | Ray query | RT − query |
|---|---:|---:|---:|
| Whole frame | 34.21 | 28.60 | +5.61 |
| `HybridTrace` | 8.67 | 6.09 | +2.57 |
| `ReSTIRTemporal` + `ReSTIRSpatial0/1` | 8.75 | 6.07 | +2.68 |
| Shadows: `ReceiverVisibility` + `SceneLighting` + `LightOccluders` vs query `SceneLighting` | 5.73 | 4.85 | +0.87 |
| Eight denoise/upsample passes | 6.95 | 7.38 | −0.43 |

The Phase 4 replacement gate fails by 6.1 ms across the replaced passes at the same revision. On
the query path the whole frame (28.60 ms) is below the frozen HW baseline (31.73 ms), but its eight
denoise passes sum to 7.24 / 7.52 ms, above the 7 ms budget. Pass intervals can include overlapping
neighbours, so that sum is a stage diagnostic.

**Compute field publication** (`build/checks/zenapp-field-memory-20261114/compute-*`). The recorded
wide-repair Compute launch (320 frames, validation on) published generations 1–4 and never 5.
Re-running it with the same arguments and setup (hash `b14ec51f…`) publishes all 24 generations,
including generation 5 (215,361 triangles) in 5 graphs and 181 ms; refinement ends at frame 212.
A 1,200-frame run without validation matches (frame 213) with no warnings or errors. These runs
had 29–33 ms frames at unaltered clocks. Device-local memory peaks at 10,655 MiB and settles at
9,845 MiB, more than the HW tier.

**Emulated 8 GB device** (`--device-memory-limit-mb=8192`, 600 frames, validation on):

| Run | GI | Device-local MiB (peak) | Frames 300–600 median CPU ms | Errors |
|---|---|---:|---:|---|
| HW, no limit | pixel, HW | 9,122 | 28.38 | none |
| HW, 8 GB | pixel, HW | 8,191 | 28.14 | none |
| Compute, 8 GB | pixel, scene field only | 8,187 (7,377 final) | 27.78 | 44 |

- **HW at 8 GB** keeps rendering with no warning. About 930 MiB of device-preferred allocations
  were placed in host memory instead, and the CPU-side frame interval did not change. These setups
  use two frames in flight, so the frames-in-flight pressure rule (from three) does not apply. GPU
  pass timing under the cap has not been captured.
- **Compute at 8 GB** publishes the scene field (generations 1–2), then rejects all 22 local
  generations with "Distance field allocation rejected (memory budget)". Each local generation
  reuses the scene build's 925 MB scratch workspace because it is large enough. Once GI is active
  and the device is nearly full, that workspace plus the 65 MB geometry snapshot exceed the shrunken
  scratch budget, and the reuse path never resizes the workspace to the local generation's demand
  (about 120k–230k triangles). Local volumes fall back to the coarse scene field and are not
  retried. Fixed the same day (next section).

### RT pipeline decision and optimization (2026-11-14)

The user chose to keep the RT pipeline as the HW tier and close its gap rather than fall back
to ray queries.

**8 GB Compute local volumes: fixed.** The hard rejection compared a generation's total scratch
with current scratch plus half the free memory. On a full device even a generation that reuses
the retained geometry and workspace failed, because of its few-KB range buffer, and was then
never retried. Now a generation's range buffer is always admitted. Only a generation that
exceeds the configured caps fails; a temporary shortage requeues it. The rejection log now prints
the budget terms. Re-running `--device-memory-limit-mb=8192` (`build/checks/zenapp-8gb-field-fix-2`)
publishes all 24 generations, ends refinement at frame 212 and logs no errors. An intermediate
attempt that replaced a reused workspace with a smaller one starved later generations and was
reverted (`build/checks/zenapp-8gb-field-fix`).

**Counter diagnosis.** The A/B captures' `GPUTRACE_REGIMES.xls`, at the same register-file use:
- RT `HybridTrace` keeps about 14 warps active against 19 for the query trace, with RT-core
  utilization at 38% vs 55% and 3.6× more shared-memory traffic.
- RT `ReSTIRSpatial0` showed instruction-cache starvation (`no_instruction` stalls 2.38 vs 0.39
  warps per cycle, heavy instruction fetches from L2) and `membar` stalls from the per-ray reorder.
- `--pipeline-statistics` stack sizes show the cause. Every value live across a `traceRayEXT` is
  saved on the RT stack: the primary raygen saves 352 bytes per trace in the raygen-shading
  variant (320 in the closest-hit variant).

**Retained: gathered ReSTIR spatial visibility.** `restir_reuse.glsl` now selects both signals'
neighbors, precomputes each pair's source-at-center density, traces the pending pair rays from
one data-dependent loop, merges, then traces the final rays from a second loop. Only packed
neighbors, masks and the results stay live; its raygen stack is 96 bytes. Compute shares the
order and every estimator value matches the inline form. The ReSTIR occlusion/reuse, GI trace
parity and pass-list tests pass in all four submission modes.

| Warm capture (median ms) | `HybridTrace` | `ReSTIRSpatial0/1` | Frame | Disposition |
|---|---:|---:|---:|---|
| Ray-query path (A/B) | 6.09 | 2.68 / 2.72 | 28.60 | reference |
| RT before (A/B) | 8.67 | 4.09 / 4.13 | 34.21 | baseline |
| ReSTIR visibility without reorder | 8.69 | 6.13 / 6.18 | 38.52 | rejected |
| Inline ray queries inside the ReSTIR raygens (diagnostic) | 8.99 | 2.90 / 2.90 | 32.50 | diagnostic: not RT traversal |
| **Gathered ReSTIR spatial visibility** | 8.69 | 3.45 / 3.50 | 33.07 | **retained** |
| Gathered, without the visibility reorder | 8.66 | 5.21 / 5.27 | 36.62 | rejected |
| Raygen shading, thin closest-hit, recursion 1 | 9.07 / 9.07 | 3.45 / 3.51 | 33.26 / 33.46 | rejected, reverted |
| Raygen shading with primary reorder | 10.15 / 10.16 | 3.69 / 3.50 | 34.81 / 34.89 | rejected, reverted |
| Gathered + `SKIP_AABBS` pipeline flag | 8.65 / 8.69 | 3.44 / 3.49 | 32.96 / 33.02 | neutral, reverted |
| Gathered primary trace (raygen shading, batched phases) | 58.66 / 58.34 | 3.47 / 3.47 | 84.15 / 83.31 | rejected, reverted (RTSS running) |

Captures are under `build/checks/rt-opt`. The diagnostic shows a raygen launch costs only about
0.2 ms more than compute for the same work; the remaining ReSTIR cost is RT traversal and the
reorder it needs. Without the reorder, warps wait for their slowest ray.

**Gathered primary trace: rejected.** At the user's request the primary trace was rewritten the
gathered way, RT-only, with compute and field code untouched:
- `PrepareHitShading`/`FinishHitShading` split hit shading around its visibility rays.
- Each batch of four samples ran in phases: sample, trace hits, prepare shading, trace the light
  and sky rays, then accumulate in sample order.
- Hits were shaded in raygen, with a thin closest-hit and recursion depth 1.

The RT/query parity, pass-list, hit-lighting, ReSTIR and transparency tests all passed. But
`HybridTrace` took 58.3–58.7 ms. The batch arrays (prepared shading, hits, directions) live in
the raygen's frame, and the RT stack grew from 320 to 1,152 bytes. Large per-thread RT stacks are
far slower here than spilled registers. The patches are kept in
`build/checks/rt-opt/patches` (`gathered_trace.py`, `hit_shading_split.py`, `raygen_shading.py`).

A smaller variant was also tried and reverted. It refetched the receiver's position and normals,
the hit seed and the reconstruction guide after each primary trace, rather than keeping them live.
The raygen stack stayed exactly 320 bytes: the compiler already rematerializes those values. What
the stack holds is the per-receiver accumulators and both ReSTIR reservoirs (about 80 words), which
the estimator needs across every sample's trace.

**Remaining gap at the retained state:** the replaced passes take 21.9 ms against 17.0 ms with
ray queries: trace +2.6, ReSTIR spatial +1.5, temporal −0.1, shadows +0.9. Whole frame is
33.0 vs 28.6 ms. The primary trace's gap follows from the trace-call model: every primary and
shadow ray saves the pixel's accumulators, while an inline query keeps them in registers. No
retained-precision restructure inside one raygen removes that state. The untried directions:
- Split the trace across passes, keeping accumulators in global memory between per-sample
  launches. This costs memory traffic of roughly the same order as the gap.
- Change what the estimator carries per pixel, for example fewer samples per launch with more
  temporal reuse. That alters the image and needs requalification.

The counter extraction (`regimes.py`) and capture helper are alongside the patches.

## Implementation record at pause

### User decisions and execution state

- The user approved **PBR/environment fallback for forward GI** when a forward
  fragment cannot reuse the matching opaque screen receiver. Forward diffuse and
  reflection ray queries are removed. Per-layer fragment shadow queries remain
  the documented exception; calibration queries remain reference checks.
- Transparent objects must retain correct ordering, shadows, transmission,
  refraction and attenuation, without replacing opaque receiver guides or
  inheriting another layer's visibility. Forward cost is measured separately.
- Every application test/profile now reuses
  `E:/Dev/ZenEngine/build/x64-windows-msvc-release/bin/ZenApp.exe`, rebuilt in place.
  Earlier helpers created runtime/executable archives; those are historical
  evidence and are not the current launch workflow. No new per-run executable
  copies or routine launch approvals are needed. Native test executables remain
  separate test targets.
- ZenApp runs use explicit `--setup` files and startup overrides. Profiling uses
  `tools/profile_hybrid_gi.py`; `Data/engine.cfg` is not temporarily edited.
  Captures retain exact arguments, executable/setup/input/shader hashes, actual
  framebuffer/render/GI extents, fullscreen/backend/async state and clock policy.
- The working tree contains the implementation and test/documentation changes;
  it has not been staged or committed as part of this task. ZenTest is a dirty
  submodule containing test changes. ZenAssetProcessor was not edited.
- At pause the canonical executable SHA256 is
  `162696501d73aca71d6bd9d1acb10d4b9c16c7c103d2c9c4c677c0d616e341d7`.
  The retained history-fix SPIR-V SHA256 is
  `e5880fa859ef857d64a5fbe231669c0f333e13cbb99fb04bdc36a9678d26c25f`.
  Private probe modules were restored; no temporary candidate remains installed.

### Phase 0: completed baseline and tooling

`tools/gi_noise_probe.py`, `tools/gi_noise_gates.py` and
`tools/gi_removal_gate.py` now support the frozen presets, both pan speeds,
dolly, sparks, static/reference checks, Nsight timings and strict removal
comparisons. Missing measurements or changed provenance do not pass a gate.

The last-pose anomaly was an orbit through nearby geometry, not a valid pan.
The fixed-eye pan and an extended endpoint control remove that confound. The
endpoint control differs at only one pixel by 1/255, with zero pixels over 4/255.
The complete two-repeat baseline is `build/render-hardening-phase0`, including
`report-all.json` and `gates.json`; the endpoint control is
`build/render-hardening-phase0-endpoint`.

The reference at `E:/Dev/ZenGroundTruth/restir/matched-4k-20261112-v2` is the
existing sparse ZenApp Monte Carlo capture: 130,560 selected pixels, approximately
1.574% of 4K. It shares engine shading and must not be described as an independent
Mitsuba ground truth. The frozen gate data was not replaced by easier thresholds.
The original plan's approximately 14 ms denoising estimate and the frozen
Nsight baseline's 8.5721 ms stage sum are different measurements.

### Phase 1: completed removal changes

| Removal group | Retained result and verification record |
|---|---|
| Screen probes | Removed `ScreenProbeGather`, shared probe layout, 34 probe shaders, probe debug program, gather/spacing settings and UI. Moved six surviving pixel temporal/reconstruction/reuse tests into `HybridDenoiseIntegrationTests.inl`; removed probe-only tests. Fixed first-frame guide-buffer initialization exposed by the removal. `build/render-hardening-phase1-probes-v2` and `build/checks/hardening-probe-*-tests` |
| Cone GI | Removed cone bounce, shaders, settings and dedicated composition. Cold Compute preparation shows PBR/environment while field construction continues; cache injection waits for a usable field. Unsupported Compute forward/scatter layers use the approved environment fallback. Removed cone-only tests; retained normal bias and field/cache coverage. `build/render-hardening-phase1-cones`, `build/checks/hardening-cone-verified` |
| Raster voxelizer | Removed `GeometryVoxelizer`, raster voxel shaders, mode selectors and their three obsolete tests. Compute voxelization continues to feed cache construction and voxel debug. Surviving shader modules and all 41 sampled HW frames matched the preceding build. `build/render-hardening-phase1-geometry`, `build/checks/hardening-geometry-tests` |
| Half diffuse | Extended `pixelResolutionDivisor` to pure Compute, brought output stabilization forward, then removed `DiffuseResolution`, half trace, diffuse upsample, fallback resources/programs and obsolete test portions. Reference/count diagnostics remain native. Tools now derive divisor-2 Compute setups; the original baseline stays native/half-diffuse. `build/render-hardening-phase1-compute-stabilize/resolution-qualification.json`, `build/render-hardening-phase1-half` |
| Mixed providers | Replaced six per-feature providers with a whole-tier choice; removed mixed HW/field variants and HW cache injection/sky programs. Independent signal/shadow enables remain. Updated editor tier/reload tests and UI controls, including stale cone controls. `build/render-hardening-phase1-tiers`, `build/checks/hardening-tier-*` |
| Unqualified variants | Removed adaptive sampling, delayed previous-frame radiance, legacy sky, buffered guide variants and arbitrary trace-module overrides. Retained explicit qualified no-guide programs, reference estimator, built-in sparse capture, optional counts and debug views. Updated tools/build registrations and lifecycle tests. `build/render-hardening-phase1-final-removal`, `build/checks/hardening-final-removal-*` |

Retired SPIR-V/dependency files were archived with hashes before removal.
The detailed removed-test inventory and each intermediate build/suite result are
recorded under the corresponding Phase 1 sections of `Denoiser.md`.

The reduced Compute step was locally accepted under the documented expected-
difference allowance after stabilization fixed its dolly/spark regressions.
Its recorded `original_frozen_gates_passed` remains false. Main/local field
publication depends on elapsed frame time and shifts submitted GI sample indices;
strict image identity therefore remains unresolved. A later local field
(generation 5, 215,361 triangles, 925,107,920 scratch bytes in the recorded scene)
still fails to publish during sampled launches. `NextCycleCount` reduces its
batch against total frame time, including unrelated rendering. No fix has been
qualified, and these Compute captures are not fully settled-field measurements.

### Phase 2: retained denoiser implementation

The opt-in family implements demodulated sky/bounce/specular signals and hit
distances, Poisson preblur, bilinear slow/fast temporal histories, spatial
fast-history clamping, history repair, three 1/2/4 atrous iterations,
reconstruction and output stabilization. Unsupported reconstruction receivers
take the nearest history-fixed signal; they do not trace fresh output rays.
Clamp/repair outputs feed the next frame's slow history; atrous output does not.

Retained corrections and refinements include:

- Reprojected and surface-validated preblur history ages, independent diffuse and
  reflection footprints, and removal of redundant reflection loads.
- Surface/lobe-aware 5x5 fast-history statistics in YCoCg, a four-sigma normal
  clamp, and a one-sigma bounce/reflection clamp during lighting-response flags.
  Sky keeps its independent invalidation. No global mature-history reset is added.
- Fixed 1/2/4 atrous lattices, age-dependent radius/strength, shared reads where
  measured useful, and factored spatial/kernel weights.
- Correct reflection history for fixed-eye camera rotation; virtual reprojection
  and rejection remain for parallax. CPU-confirmed unchanged receivers avoid
  fractional reprojection roundoff.
- Output histories up to `historyFrames * divisor²`, capped at 255 per packed
  age channel. The legacy stabilization path retains its separate eight-frame cap.
- Disabled-sky specializations and one-texel sky bindings, while preserving
  traversal-availability flags needed by temporal accumulation.
- ReSTIR spatial publication updates the hit-distance guide to match its selected
  radiance sample, only for the new denoiser. Availability and legacy guides remain.
- History repair now covers ages below **eight** frames, with sixteen mip taps
  spaced eight texels apart and clamped to viewport bounds. This is a measured
  prototype refinement of the original under-four-frame design, not a passed
  motion gate. Empty/six/eight-frame and small-viewport regressions cover it.
- Removed unused guide-pyramid level zero and allocated one-texel legacy
  `BounceMoments` in ReLAX. No-guide programs use descriptor-sized learned-guide
  buffers; real guide/count/reference variants retain full storage and invalidate
  guide history correctly on size changes.
- Histories publish only after successful submission. Resize, reset, declined
  recording, disabled signals and distinct view histories have targeted coverage.
  The pooled-texture lifecycle test now checks actual shader/descriptor bindings
  instead of allocation labels, which can survive pool reuse.

The eight half-resolution/output passes are `GIDenoisePreblur`,
`GIDenoiseTemporal`, `GIDenoiseHistoryFix`, `GIDenoiseAtrous0/1/2`,
`HybridReconstruct` and `GIDenoiseStabilize`. This is the current prototype list,
not the final Phase 6 per-tier queue/budget contract.

### Phases 3–4: retained RT and transparency implementation

The RHI/RDG foundation supports raygen, miss, closest-hit, any-hit and callable
stages; reflected multi-stage descriptors; shader groups; cached RT pipelines;
pipeline-owned, correctly aligned SBT buffers; direct/indirect trace commands;
device limits; and RT pass barriers/async eligibility. RT shader builds target
SPIR-V 1.4; production compute shaders target SPIR-V 1.3. EXT/NV invocation
reordering is chosen by reported support, with plain modules retained.

The primary raygen shades hit material/lighting in closest-hit and uses a separate
nested shadow payload/group. Typed radiance/shadow any-hit interfaces fixed the
large guided-image discrepancy found during cross-path testing. The retained
radiance payload has twelve scalar values. Ordinary/exact environment guides,
ReSTIR, reference samples, reduced extents, counts and multiple histories have
native checks; matched-proposal raw signals and issued-ray totals are compared
against query references with explicitly documented floating-point tolerances.

ReSTIR temporal and spatial visibility use raygen passes. The two spatial
checkerboard passes remain: an in-place merge races, while a separate reservoir
copy adds about 398 MB at native 4K and changes update order. Reordering is retained
for ReSTIR because removing it regressed measured cost. No-environment primary
tracing uses plain tracing because primary SER measured slower. Visibility uses
EXT/NV hit objects without unnecessary hit/miss shader execution when supported.

Receiver shadows and `LightOccluders` have RT pipelines. The exact triangle cache
remains because uncached receiver shadow timing regressed substantially. Cached
triangle area occupies the existing unused component, retaining the 48-byte
triangle layout and exact finite-segment tests. Primary-hit use of this cache
was tested and rejected separately.

Receiver visibility uses RG32UI below 32 scene lights, reserving the unused high
visibility bit for the camera light. At 32 scene lights it selects RGBA32UI and
a separate camera component. This saves 128 MiB committed VRAM in the measured
scene and preserves all 32 HW static images byte-for-byte. Native tests cover
the 31/32-light transition and camera-light on/off cases.

Forward layers retain matching receiver GI reuse and their own shadow queries.
Unsupported forward GI uses the approved material/environment fallback.
Integration tests cover overlapping transparent layers, CPU/GPU draw-order image
equality, opaque occlusion, shadowed opaque receivers behind lit front layers,
transmission, screen refraction, thickness/absorption, alpha-tested geometry and
unchanged opaque position/normal/depth guides while layers move or are hidden.

### Latest retained measurements and verification

Static values below are display-space 8-bit flicker/RMS. The three-preset source
is `build/render-hardening-phase2-wide-repair-production/report-flicker.json`;
`build/render-hardening-phase4-compact-visibility` reproduces all 32 HW images.
Each current row is **one run**, not the complete repeated acceptance matrix.

| Preset | Phase 0 flicker / RMS | Retained flicker / RMS | Result |
|---|---:|---:|---|
| HW half GI, ReSTIR | 0.02941915 / 0.55659251 | 0.01540332 / 0.54811914 | Both static limits pass once |
| Compute divisor 2 now; native/half-diffuse baseline | 0.08725427–0.08725530 / 3.38677437–3.38678516 | 0.00499578 / 3.38581222 | Both static limits pass once; strict removal identity is a separate open gate |
| Editor native GI, ReSTIR off | 0.29833789 / 1.17509391 | 0.17472290 / 1.06278843 | Both static limits pass once |

The latest retained warm HW capture is at 3840x2160 fullscreen, 1920x1080 GI,
threaded RHI, recorded async policy, base GPU clocks and validation disabled:

| Metric | Retained value | Interpretation |
|---|---:|---|
| Eight denoise/upsample passes | 6.977102 ms | Stage-median sum meets 7 ms in this run; full repeated qualification remains open |
| Primary GI trace | 8.660865 ms | RT replacement timing gate remains unmet |
| Receiver visibility | 4.661005 ms | Separate from deferred composition |
| ForwardTranslucent | 1.524335 ms | Below frozen HW 3.32696 / 3.32688 ms; final all-preset verification still required |
| Whole frame | 34.10240 ms | Above frozen HW 31.72620 / 31.72175 ms |
| VRAM requested / committed / demoted | 9,608 / 9,622 / 0 MiB | Down 128 MiB committed from wide-repair production; not an 8 GB pressure-test pass |

Do not sum overlapping pass times to infer whole-frame time. The compact
visibility capture's executable hash differs from the current rebuilt binary;
its own manifest remains the authoritative provenance for that measurement.

Focused production fast-pan pose 32 measures **0.28372155%** over 4/255 and
RMS 0.63757545, versus the required **0.19942371%** threshold. The private
bounded-repair full HW pan run measures slow poses 8/32/64 at
0.127170 / 0.160108 / 0.170983%, and fast poses at
0.160289 / 0.282600 / 0.201980%. Only fast poses 8 and 64 meet the 50%
reduction requirement. Production repeats across all presets, dolly and sparks
remain outstanding. The production lighting-response diagnostic reduces
recovery-frame-16 large errors from 0.221764% to 0.080693%, with a measured
increase in low-level mean error; it is not complete lighting qualification.

Strict guided query/RT A/A parity remains open. After the typed payload fix,
`build/checks/guided-pipeline-aa-typed` records query A/A RMS about 0.103707,
RT A/A about 0.104955, and cross-path about 0.108210–0.108322. The remaining
cross-path difference is not waived as equivalent.

| Verified scope | Actual passing cases | Evidence / limits |
|---|---:|---|
| RT foundation RHI/reflection | 95 | `build/checks/rt-foundation-tests/VulkanRHITest.xml`; includes SBT/stages/launch limits |
| Optional RT capabilities | 21 | `build/checks/rt-capability-optional/result.xml`; foundation masks, not the unfinished whole-tier fallback matrix |
| Current ray integration suite | 116 | `build/render-hardening-phase4-compact-visibility/native.xml`; four fixture skips and twelve disabled cases are not passes |
| Core after restoring retained sources | 776 | `build/checks/compact-visibility-restored/core.xml`; nine disabled cases |
| Retained history repair | 52 | `build/checks/wide-repair-age-green/native.xml`; all four submission modes |

The recorded GPU runs are clean of Vulkan validation/synchronization hazards and
report no memory leaks. Transparency also passed after repair and after compact
visibility. Earlier full release/editor/UI/config/runtime/Python verification is
recorded per phase in `Denoiser.md`; it must not be represented as a new full-suite
run on the final paused revision. Four lantern experiments need the external
hashed fixture. No disabled or skipped experiment counts toward completion.

### Rejected experiments and last diagnostics

These were investigative candidates, not retained implementation or passed gates.
Original modules/sources were restored. Detailed commands, identities and results
are in the named directories and the chronological `Denoiser.md` ledger.

| Experiment | Result / disposition |
|---|---|
| Primary SER, material hints, explicit hit-object execution and specialized stack size | No useful primary timing gain; retain plain no-guide primary and automatic stack policy |
| Removing ReSTIR reordering | Spatial passes became slower; retain reordering |
| Primary-hit occluder cache | Trace regressed; retain cache only in the qualified receiver-shadow role |
| Shared-control eleven-scalar payload | Trace regressed; restore twelve scalars (`build/checks/primary-shared-control`) |
| Unused legacy filter-output placeholders | No actual RDG-pool saving and no frame improvement; reverted. The corrected pooled-resource binding test remains (`build/render-hardening-phase2-unused-filter-output`) |
| 8x8 primary launch tiling | Trace 10.323150 ms, whole frame 36.85480 ms; rejected (`build/checks/primary-tiles-8x8`) |
| Dedicated diffuse closest-hit group | Trace 8.607075 ms; difference did not establish a useful gain amid variation; extra group removed (`build/render-hardening-phase4-diffuse-hit`) |
| Packed-normal ten-scalar payload | Native suites pass, but trace 8.921120 ms and frame 34.43705 ms regress; four source files restored (`build/render-hardening-phase4-packed-normal`) |
| ReSTIR confidence cap / removing temporal or spatial reuse | Did not improve the required pan metric; original confidence/reuse retained |
| Wider young preblur, longer luminance fade and dense sixteen-frame repair | Insufficient motion improvement or worse error; not adopted |
| Sixteen-texel repair spacing | No useful gain beyond eight; not adopted |
| Sixteen-frame wide repair window | Fast-pan 0.29645303%, worse than retained 0.28372155%; restored (`build/checks/history-repair-window-16-compact`) |
| Nearby mature temporal support | Fast-pan 0.28316696%, RMS 0.63751775; negligible improvement, not adopted (`build/checks/history-mature-support-pan`) |
| Unique clamped boundary taps | Fast-pan 0.28163580%, RMS 0.63595716; still fails the gate, not adopted (`build/checks/history-unique-boundary-pan`) |

The last completed diagnosis locates 10,159 of 23,533 threshold-exceeding
production fast-pan pixels in the bottom-left 240x240 block. In diagnostic
replays, all flagged corner pixels have histories under eight frames (mostly
two); 9,308 show accepted reprojection and 851 show outside-view rejection.
All have ordinary reconstruction support. This points to newly exposed
short-history receivers, not missing reconstruction support. The current repair
footprint almost never reaches an eight-frame-old neighbour there. Evidence:
`build/checks/wide-repair-production-pan/error-blocks.json` and
`build/checks/pan-corner-history/analysis.json`.

`build/checks/pan-corner-signals` completed only its moving/settled bounce-debug
pair before the user requested stopping. Corner bounce-debug RMS is 7.064054/255;
70.44618% exceed 4/255. These are isolated sRGB debug values, not final-image gate
metrics or linear radiance ratios. The reflection sequence was interrupted;
there is no completed reflection comparison or full report for that run.
Its Python helpers and ZenApp process were explicitly stopped and zero matching
capture processes remained. Do not resume or treat that partial run as complete.

### Work remaining if the user resumes

1. Resolve the open removal-equivalence/Compute publication issue and qualify
   the complete Phase 2/4 quality and timing matrices against the frozen gates.
   This includes both tiers and editor preset, both pan speeds/all poses, dolly,
   sparks, static repeats, lighting response, strict query/RT A/A and diffuse-hit
   reflection reuse. Current failures are recorded above. The only threshold changed is the
   Phase 4 timing gate, amended on 2026-11-14 at the user's decision to an accepted-RT-baseline budget.
2. Only after qualification, perform Phase 5: default to ReLAX and RT pipelines,
   select half-resolution/ReSTIR editor defaults, delete the old filter and HW
   query implementations/settings/tests, and repeat the dead-code audit.
   Current source defaults are still `relaxDenoiser=false`,
   `rayTracingPipeline=false`, divisor 1 and `restirGI=false`.
3. ~~Complete capability fallback **RT pipeline -> Compute field -> PBR**~~ —
   implemented and unit-tested on 2026-11-14 ([resumed work](#resumed-work-2026-11-14)).
   Still open: ZenApp runs with forced capability masks recording each step's
   status and log output.
4. Pass-list pins and the frame-flow table landed on 2026-11-14, along with the
   device-memory limit option and the field-pacing fix. Still open: a Compute
   timing column, 8 GB ZenApp pressure runs for both tiers, full
   resize/reset/reload/multiview coverage, a deterministic settle-then-reset
   capture mode for strict Compute identity, and final all-mode
   GPU/editor/runtime verification.
5. The README, trace-profile and trace-quality documents already link to this
   plan. Update them to a genuinely qualified final pipeline only when the
   remaining work is complete. Rendering is not yet closed apart from bug fixes.
