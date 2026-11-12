# HybridTrace profile (2026-11-11)

Baseline for optimizing `HybridTrace`, the most expensive GI pass. Measured on the build that
includes the [spark fix](SparkFixPlan.md) (ZenEngine `b6f9dfce`). The measurements below describe
that baseline, not the current optimization code.

## What the pass does

The per-pixel HW RT configuration with ReSTIR uses `HybridTraceReSTIRHardwareSP`:
[hybrid_trace_restir_hardware.comp](../../Data/Shaders/VoxelGI/hybrid_trace_restir_hardware.comp)
includes [hybrid_trace.glsl](../../Data/Shaders/VoxelGI/hybrid_trace.glsl) with
`HYBRID_HARDWARE_PROVIDER` and `HYBRID_RESTIR`.

- **Dispatch:** one 16x16 workgroup (256 threads) per screen tile of the sky-visibility guide,
  one thread per GI texel. Half-resolution GI launches about 2.08 million threads; quarter about
  0.51 million.
- **Per thread, in order:**
  1. `BuildGuide`: the workgroup loads its tile's visibility guide into shared memory
     (`guideWeight`, `guideDirection`, 384 tiles each, plus row moments and reduction scratch).
  2. `TraceDiffuse`: 4 diffuse rays (6 on reset, dark or uncertain receivers when adaptive
     sampling is on; it is off in this setup). Each hit runs `HardwareHitRadiance`
     ([ray_hit_lighting.glsl](../../Data/Shaders/VoxelGI/ray_hit_lighting.glsl)): material
     texture reads (base color, metallic/roughness, normal, emission), all unshadowed lights,
     and **one shadow ray** to a light chosen in proportion to its unshadowed luminance. The
     environment-visibility ray runs only when environment lighting is on (off here). Hits feed
     the diffuse reservoir (`initialGI`).
  3. `TraceSpecular`: 1 reflection ray (more only when `samples > 4`), the same hit shading and
     shadow ray, feeding the reflection reservoir.
  4. Stores raw sky, bounce, specular and reflection guides, the two packed reservoirs (24 bytes
     each), then `FinishGuide`: shared-memory reductions and the next frame's guide.
- **Ray queries per GI texel:** up to 5 primary rays plus up to 5 shadow rays.

## Configuration

Standalone ZenApp through `tools/profile_hybrid_gi.py` (Nsight Graphics 2026.3.1 GPU Trace, base
GPU clocks, 30 frames after a 30-second delay), RTX 5080 (84 SMs, 48 warp slots per SM),
3840x2160 fullscreen render and framebuffer, threaded RHI, async compute and GPU culling on.
Setup: `E:/Dev/ZenGroundTruth/restir/editor-hw-20261111/quarter-clean/setup.zenrender`
(BC-compressed New Sponza, center view, normal maps on, every GI feature on HW RT, per-pixel
gather, ReSTIR GI with spatial and rough-reflection reuse, 4 rays, environment lighting off).
Half resolution changes only `pixelResolutionDivisor` to 2.

Evidence: `E:/Dev/ZenGroundTruth/restir/spark-20261111/` (`half-timing-{1,2,3}`,
`r2-timing-{1,2,3}`) and `E:/Dev/ZenGroundTruth/restir/filter-20261111/timing-half-{1,2,3}`;
extracted counters in `spark-20261111/hybridtrace-counters.json`. Each directory holds the trace,
exact arguments, input hashes and the BASE export (`D3DPERF_EVENTS.xls`, `GPUTRACE_REGIMES.xls`).

## Timing

| Run | GI | Frame median | HybridTrace | Registers / thread | Warps / SM |
|---|---|---:|---:|---:|---:|
| half-1 | 1920x1080 | 43.44 ms | 15.58 ms | ~200 | 8.0 |
| half-2 | 1920x1080 | 40.84 ms | 10.48 ms | 128 | 16.0 |
| half-3 | 1920x1080 | 35.85 ms | 10.60 ms | 128 | 16.0 |
| half, filter off, 1-3 | 1920x1080 | 31.65 / 30.08 / 30.18 ms | 10.48 / 10.47 / 10.47 ms | 128 | 16.0 |
| quarter-1 | 960x540 | 24.07 ms | 3.86 ms | ~200 | 8.0 |
| quarter-2 | 960x540 | 20.02 ms | 2.60 ms | 128 | 16.0 |
| quarter-3 | 960x540 | 20.41 ms | 2.60 ms | 128 | 16.0 |

Registers per thread are derived: allocated registers per SM / (active warps x 32). At 128
registers, two 256-thread workgroups fit the 64K-register file; above 128 only one does.

Pass time scales with GI texels (4.0x the texels: 2.60 -> 10.48 ms, 4.03x). Half-2's slower frame
comes from other passes (ReSTIR spatial 4.92/4.97 ms against 2.71/2.76 ms in half-3), not HybridTrace.

## Counters (median of 30 frames)

| Counter | Normal launch (half-3) | Slow launch (half-1) |
|---|---:|---:|
| Register file allocated | 99.84% | 77.96% |
| Active warps (% of 48 per SM) | 33.28% | 16.63% |
| RT-core cycles executed | 42.57% | 26.85% |
| SM throughput | 29.91% | 20.15% |
| L1/TEX throughput | 38.81% | 16.33% |
| L2 throughput | 66.09% | 26.35% |
| Stall: long scoreboard, L1/TEX | 9.72% | 4.31% |
| Stall: long scoreboard, RT core | 8.42% | 3.96% |
| Stall: wait | 4.92% | 3.29% |
| Issued (selected) | 2.51% | 1.68% |
| Stall: barrier | 1.79% | 0.54% |
| Stall: short scoreboard | 1.57% | 0.94% |
| Stall: no instruction | 1.34% | 0.50% |
| Stall: branch resolving | 0.92% | 0.62% |
| Stall: MIO throttle, RT core | 0.64% | 0.18% |

Stall values are PC-sampler percentages of elapsed warp-issue slots. Quarter-resolution launches
show the same two profiles (normal: 42.4% RT core, 33.25% warps; slow: 26.9%, 16.63%). DRAM
throughput was not in the exported metric set.

## Earlier ray-count scaling

From [ReSTIRGIImplementation.md](ReSTIRGIImplementation.md) (native GI, 3840x2160, before the
spark fix): HybridTrace 38.40 ms with 4 diffuse rays, 24.14 ms with 2, 17.85 ms with 1. Each
diffuse ray with its hit shading costs about 6.8 ms at native resolution (about 1.7 ms at half);
the reflection ray, guide and stores make up the remaining ~11 ms (~2.8 ms at half).

## Reading

- **Occupancy is register-limited and quantized by the workgroup.** The normal compile fills the
  register file with two workgroups (16 of 48 warp slots). A third workgroup needs at most
  85 registers per thread.
- **The pass is latency-bound, not ray-throughput-bound.** The RT core is busy 43% of the time,
  and warps mostly wait on texture reads (hit materials, guides) and ray queries; only 2.5% of
  issue slots issue.
- **The launch-to-launch swing is register allocation.** Some launches (in these sets, the first
  after a shader rebuild) compile to about 200 registers. One workgroup per SM then takes 1.47x
  as long (15.58 vs 10.60 ms). The cause in the driver compile is not identified.

## Optimization directions to evaluate

Each change should keep the image identical (or measure quality as in the spark plan) and be
timed over at least three launches, recording the register and warp counters above per launch.

1. **Make the allocation stable at or below 128.** Find what pushes some compiles to ~200
   registers (live state across ray queries: the two reservoirs, guide data, accumulated
   diffuse/specular) and reduce it. This removes the 1.47x slow launches.
2. **Reach three workgroups per SM (at most 85 registers), or allow finer occupancy steps.**
   Options: split diffuse and specular tracing into separate dispatches; move hit shading out of
   the trace loop (trace, store hits, shade in a second pass); or use smaller workgroups with the
   guide built in its own pass, so occupancy is not quantized in 256-thread steps.
3. **Cheaper hit shading.** The texture-read stall is the largest single stall reason. Candidates:
   fewer material reads on secondary hits (for example a lower mip or skipping the normal map),
   and the cost of the per-hit shadow ray.
4. **Ray budget.** 2 diffuse rays save about a third of the pass but raised ground-truth error
   from 7.22% to 8.55% relative RMS at native GI (ReSTIR records).

Not yet measured: source-level hot spots (needs the Nsight real-time shader profiler and the GUI),
the trace's share of shared-memory use, and the 6-ray adaptive path.

## Optimization plan

Status: the follow-up below fixes the verification defects and qualifies the environment-off
default at stable 128-register allocation, baseline-identical images, and about 25% less trace
time. **Finishing stages 0-3** (last section before the targets) records the stage 0 cost
breakdown and counts, a blocked stage 2 occupancy exit (the default stays at 128 registers and
15.6 warps per SM; every tested way to force fewer registers either spilled into a slower pass or
lost the device), environment-on timing that met its goal on the unchanged shared-guide default in
six launches, and the stage 3 gates (hit stream stays opt-in, alpha candidates and placeholder
stores dropped). No default shader or dispatch changed in that pass. **Stage 4 (2026-11-12)**
changed the image on purpose: per-pixel ReSTIR traces now skip the normal map on diffuse-ray hits
and estimate rough reflections (roughness >= 0.5) from the diffuse rays. Together they cut the
pass by 20% with environment lighting off and 12% with it on, and improved the matched quality
metrics; ray-cone mips, light candidates and two rays for settled receivers were rejected on
their quality gates. Baseline byte identity no longer applies; `stage4-20261112/reference/` holds
the new retained captures. The plan orders the directions above from the shaders at `b6f9dfce`.
Every time below marked "estimate" is derived from the baseline counters, not from a run.

### Implementation and experiment status

- **Stage 0:** `ZenApp --pipeline-statistics` enables supported Vulkan executable statistics and
  logs the actual module paths (including `.omm.spv`). It is diagnostic-only: omit it from timing
  runs. `tools/build_hybrid_trace_variants.py --output=<new-directory>` writes separate control,
  combined-loop and six ablation modules, plus source/module hashes. It never replaces the shipped
  `Data/SpvShaders` modules. `--hybrid-trace-module=<absolute-file.spv>` selects an exact all-HW,
  per-pixel ReSTIR module at startup; the profiling runner records its hash and arguments.
  Finished on 2026-11-12: cost ablations of the current 8x8 no-guide default (builder
  `--buffered-guide --no-environment` now builds them), one real-time shader-profiler capture
  (hot spots are in the raw trace only; the CLI export has no source table), and
  `ZenApp --hybrid-ray-counts` for per-feature HW ray and candidate-loop counts.
- **Stage 1:** diffuse outputs and the GI reservoir are stored before reflection tracing;
  material/geometry/texture reads, including emission, precede lighting queries. Visibility-only
  queries share traversal, clipping and alpha testing with surface queries. A combined diffuse /
  reflection ray-slot loop with one hit-shading site is available as the `combined` variant.
  Keep it selectable for the later comparison; it is not the default. Earlier exploration of a
  superseded combined shader increased diagnostic register use, so its source-level simplification
  is not evidence of a performance gain.
- **Stage 2:** full-resolution traces without environment lighting use 8x8 no-guide variants.
  Environment lighting retains the shared 16x16 guide after the buffered variant regressed normal
  launches in qualification. The explicit buffered-module option still runs the experiment:
  `HybridGuidePrepare` pools the guide and clears accumulation; tracing learns into the next-guide
  buffer; `HybridGuideFinish` averages it. Neither guide pass nor the pooled allocation runs without
  sky light, and tracing skips `GuideTotal`. Publication records whether a successful frame actually
  wrote a guide, including transitions back to environment lighting. Exact-guide captures,
  ray-count diagnostics and the half-diffuse/fallback path retain their original layouts.
  The pooled allocation is 2,320 bytes per 16x16 guide tile; accumulation reuses existing history.
  Occupancy exit (24 warps per SM) blocked on 2026-11-12: the default needs between 96 and 128
  registers without spilling, no single ablated component lowers the 128 step, and forced caps
  through larger workgroups either lose the device (576-768 threads) or run 10-43% slower.
  Environment-on keeps the shared 16x16 path, which ran at 128 registers and 3.20 ms in six of
  six launches on the final build; the buffered path remains experimental. Stage 4 identified the
  trigger of its 190-register state: the first launch after the driver sees new SPIR-V (a salted
  module with identical code) compiles to 192-224 registers, every later launch to 128.
- **Stage 3:** hardware queries reconstruct position/normal from one index triplet. The later
  verification rejected retaining vertex/UV arrays and the triplet across shading: their register
  cost outweighed the saved reads. Shading now preserves the baseline expressions; zero emission
  skips its texture read. The zero-metallic shortcut was removed after isolating a one-pixel
  byte-identity failure. `--hybrid-hit-stream` opts into 48-byte vertex
  attributes and 368-byte secondary-hit material records at source precision, with frame-safe
  updates, explicit failure handling, query-budget accounting and logged allocated bytes. It
  remains optional: sampled memory is below 8 GiB. Its three-launch gate passed on 2026-11-12
  (faster in every launch at both resolutions) but the gain is 0.3-0.8%, under the plan's 2%
  drop threshold, so it stays opt-in. The trace counter record now has 16 words:
  Compute rays, HW rays, field work, then HW candidate-loop iterations, each group four words.
  The half-diffuse previous-bounce output writes zero when disabled, matching its full-write
  declaration; the upsample does not read it in that configuration.

For an isolated combined **8x8** variant, pass `--buffered-guide --variant=combined` to the variant
builder and select the result with `--hybrid-trace-buffered-module=<absolute-file.spv>`. Both module
selection options and `--hybrid-hit-stream` are supported by `tools/profile_hybrid_gi.py`. Select
the `.omm.spv` file on OMM devices; a supplied module is used exactly as named.

The conditional trace/shade split was evaluated during the follow-up below and rejected: its
combined pass cost exceeded the single-pass trace. Qualification results are recorded separately
below; earlier exploratory captures do not qualify the subsequently changed code. The original
targets and acceptance protocol follow.

### Verification (2026-11-12)

This is the initial verification snapshot. The subsequent fixes and replacement measurements
are in **Follow-up implementation and verification** below.

Release build of the working tree on `b6f9dfce`, same machine and profiling configuration as the
baseline above. Evidence: `build/hybridtrace-optimization/verify-20261112/` (setups, captures,
logs, Nsight exports, variant modules, `extract.py` for the counters, `ppmdiff.py` for images).
The baseline image is `build/hybridtrace-optimization/baseline/quarter.ppm` (frame 128).

**Defects**

| # | Defect | State |
|---|---|---|
| 1 | HW RT did not render. `SceneRayQuery::BindInputs` binds `RayHitVertices` / `RayHitMaterials` for every ray-query pass, but only shaders including `ray_hit_lighting.glsl` declared them. The render graph rejected `LightOccluders` and the ReSTIR passes ("Unknown or incompatible binding"), and ZenApp exited on the first frame. | Fixed: `hardware_ray_query.glsl` includes `ray_hit_material.glsl`, so every ray-query shader declares both buffers |
| 2 | With environment lighting on, the default (buffered-guide) path fails on the first frame with "Invalid Vulkan descriptor set bindings" and ZenApp exits. `hybrid_guide_prepare.comp` and `hybrid_guide_finish.comp` include all of `hybrid_trace.glsl`, so they declare every trace binding, and the passes bind only the few they use. The mock RHI of RenderCoreTest does not check this. The 16x16 path (`--hybrid-trace-module`) renders with environment lighting on. | Open. The buffered guide has never run on Vulkan, so its learning and sampling are unverified |
| 3 | `ScreenProbeHistoriesSurviveDeclinesAndResetAcrossGatherChanges` counted only `hybrid_trace_field` dispatches and failed against the buffered program. | Fixed in ZenTest |
| 4 | Half-diffuse path: the trace now skips the `halfBouncePrevious` store when `hybrid.bounce.w` is zero, but the pass still declares a full write and the upsample still reads the image. The value is unused downstream. | Open, low |

**Tests and checks.** Before fix 1: RenderCoreTest 2 failures, RayQueryIntegrationTest 16. After
fixes 1 and 3: RenderCoreTest 781 passed (9 disabled), RayQueryIntegrationTest 108 (12 disabled),
ConeVoxelGIIntegrationTest 192, RuntimeLaunchTest 21. Format and no-exception checks pass. A
validation-enabled HW run without environment lighting reports no errors. The variant builder
produces all modules; its control modules match the shipped ones byte for byte.

**Image, quarter GI, frame 128, environment lighting off**

| Comparison | Result |
|---|---|
| New default, two runs | Identical |
| New default vs 16x16 shared guide, combined loop (16x16 and 8x8), `--hybrid-hit-stream` | Identical |
| New default vs baseline | 2.37% of pixels differ; mean 0.024/255; 99.99th percentile 1; maximum 22; 10 pixels above 8, on one diagonal edge; mean colour -0.0006/255 |

The guide move, the 8x8 groups, the combined loop and the hit stream are image-identical. The
remaining difference comes from changes common to all new trace modules (hit shading and queries)
and is rounding-sized, but the byte-identity exit against the baseline is not met. Its source
change was not isolated.

**Timing (30 frames after 30 s, base clocks)**

| Launch | GI | HybridTrace | Registers / thread | Warps / SM |
|---|---|---:|---:|---:|
| Baseline, normal / slow | quarter | 2.60 / 3.86 ms | 128 / 200 | 16.0 / 8.0 |
| New default 1, 2, 3 | quarter | 3.68 / 2.57 / 3.68 ms | 240 / 128 / 240 | 7.9 / 15.7 / 7.9 |
| New 16x16 shared guide 1, 2 | quarter | 3.91 / 2.76 ms | 240 / 128 | 8.0 / 16.0 |
| Baseline, normal / slow | half | 10.48 / 15.58 ms | 128 / 200 | 16.0 / 8.0 |
| New default 1, 2, 3 | half | 14.76 / 14.76 / 14.71 ms | 240 / 240 / 240 | 7.9 / 7.9 / 7.9 |

- **Stage 1 exit not met.** The two-state allocation remains, and its high state rose from about
  200 to 240 registers. Five of six default launches were in the high state, not only the first
  after a rebuild.
- **Stage 2 exit not met.** No launch exceeded 16 warps per SM. At 128 registers the 8x8 pass
  measures 2.57 ms against 2.60 ms; at 240 registers it is about 5% faster than the baseline's
  slow launch (3.68 vs 3.86 ms, 14.7 vs 15.6 ms). The barrier stall is gone (1.7% -> 0.0%).
- **The probe reports the high state.** `--pipeline-statistics` gives 196 registers for the
  baseline and 238 for the new default, matching the slow launches (200 and 240), not the
  128-register ones. Use it as a worst-case reading.

**Where the registers went (probe, 8x8 buffered modules, images identical)**

| Module | Registers |
|---|---:|
| Baseline 16x16 | 196 |
| New default | 238 |
| Without the runtime stream branch in `LoadRayHitVertex` / `LoadRayHitMaterial` | 242 |
| Without guide code (`HYBRID_ABLATE_GUIDE`) | 222 |
| Without both | 168 |

The 168-register module timed 2.78 ms in its high state (11.75 warps per SM) and 2.50 ms at 128
registers. This confirms the 8x8 occupancy table: fewer registers now buy warps in steps, and the
worst launch improves from 3.68 to 2.78 ms. The earlier 16x16 no-guide probe read 128, so about
40 further registers come from the later hit-shading changes (`hitVertices[3]`, the UV arrays,
`vertexIndices` in the hit record).

**Hit stream.** New Sponza allocates 98,358,576 bytes of vertex attributes and 10,672 bytes of
materials. Its timing was not measured.

**Next steps**

1. Fix defect 2: give the guide passes shaders that declare only their own bindings, then verify
   environment-on learning against the 16x16 path.
2. Select guide code and the hit stream at compile time (program variants chosen in C++), not by
   runtime branches, and re-probe toward 128 and below.
3. Find what selects the 128-register compile. Until every launch reaches it, the pass is slower
   than the baseline's normal launch in most launches.
4. Isolate the rounding difference against the baseline, or accept it with the stage 4 quality
   checks.

### Follow-up implementation and verification (2026-11-12)

Evidence: `build/hybridtrace-optimization/refine-20261112/`. Capture inputs include the executable,
exact arguments, setup/package/module hashes, and the actual framebuffer/render/GI extents. Timing
uses standalone ZenApp through `tools/profile_hybrid_gi.py`, without pipeline-statistics enabled.
Completed raw traces were moved with SHA-256 verification to
`C:/Users/cxy/.codex/artifacts/ZenEngine/hybridtrace-refine-20261112/` when E: filled during an
export. Each affected capture folder has `trace-location.json`; its input manifests and table
exports remain in place. The incomplete `qualified-environment-buffered-2` export is excluded
from results and was retried separately.

- **Defect 2:** guide prepare/finish now have dedicated shaders. `hybrid_uniforms.glsl` shares only
  the receiver UBO layout; guide storage/build includes share the pooling implementation with the
  16x16 path. Prepare declares its UBOs, environment buffers and guide buffers; finish declares only
  its UBO and guide buffers. The first validation-enabled 4K environment-on capture completed.
- **Defect 4:** half trace writes zero to the allocated previous-bounce image when disabled;
  upsample skips that read. The producer's full-write promise is now true for active ray bounce.
- **Compiled variants:** CMake builds `.noguide` and `.stream` modules (and their combination/OMM
  variants). C++ selects the no-guide program from effective environment lighting each frame and
  the stream module from the startup option. The final policy keeps the 16x16 guide when the
  environment is lit unless a buffered module is explicitly selected. `rayScene.generation.z`
  no longer branches in shading.
  Exact/count captures retain their shared-guide path. The variant builder accepts
  `--buffered-guide --no-environment` and `--hit-stream` for equivalent isolated modules.
- **Register regression:** the initial compiled-variant probe still read 215 registers. Removing
  retained vertex/UV arrays reduced it to 128. The vertex triplet now remains local to surface
  reconstruction. Hit shading uses the original expressions and on-demand reads; these reversions
  are deliberate rejection of the stage 3 caching experiment, not a claim that caching passed.
- **Rounding isolation:** restoring the baseline trace shader with the current executable reproduces
  the baseline image exactly. The retained-array shading caused the large difference. After removing
  it, isolated zero-factor probes identified the metallic-read guard as the remaining one-pixel,
  one-level difference. The emission guard is byte-identical in isolation; metallic reads are kept.
- **Conditional split evaluated and rejected:** an all-HW, four-ray, environment-off prototype stored
  five lossless 20-byte hit records per receiver (51.84 MB at quarter GI, 207.36 MB at half).
  Its image matched the single pass exactly. Primary tracing used 64 registers, shading still 128.
  Quarter timing was 0.590 + 1.921 = 2.511 ms, versus 2.250 ms for the corresponding single-pass
  version. The prototype was removed from production; its source and capture are retained under
  `rejected-split-source/` and `timing-split-quarter/`. It did not meet the 24-warps target.

Final-revision checks: release build passed; RenderCoreTest 781 passed (9 disabled),
RayQueryIntegrationTest 108 passed (12 disabled, 4 lantern-fixture skips),
ConeVoxelGIIntegrationTest 192 passed, RuntimeLaunchTest 21 passed. Changed-source formatting,
`git diff --check`, Python syntax, isolated module/SPIR-V validation and the no-exception check
passed. Validation-enabled 4K captures completed at quarter/half GI and with environment on.

The final quarter-GI frame 128 and the compact-stream capture are byte-identical to the baseline.
Half GI is also byte-identical to the isolated baseline shader on the current executable.
The no-guide register probe reports 128 at both half and quarter GI (also with compact streams);
the environment-on buffered probe reports 168. The combined ray-slot loop is byte-identical but
uses 168 registers even without environment lighting, so it has not replaced the separate loops.

**Final no-environment timing**, three independent launches per resolution, including the first
after rebuilding; 30 frames after 30 seconds, base clocks:

| Launch | GI | Frame median | HybridTrace | Registers / thread | Warps / SM |
|---|---|---:|---:|---:|---:|
| qualified-quarter-1 | 960x540 | 20.482 ms | 1.950 ms | 128 | 15.62 |
| qualified-quarter-2 | 960x540 | 19.502 ms | 1.948 ms | 128 | 15.62 |
| qualified-quarter-3 | 960x540 | 19.333 ms | 1.945 ms | 128 | 15.62 |
| qualified-half-1 | 1920x1080 | 32.816 ms | 7.771 ms | 128 | 15.65 |
| qualified-half-2 | 1920x1080 | 32.972 ms | 7.777 ms | 128 | 15.64 |
| qualified-half-3 | 1920x1080 | 33.148 ms | 7.796 ms | 128 | 15.65 |

The maximum-to-minimum spread is 0.27% at quarter GI and 0.32% at half, within the 2% stage 1
gate. Median pass time improves about 25% from the baseline's normal 2.60 / 10.48 ms launches;
none of these six launches enters the former high-register state. This qualifies stage 1 for
the profiled environment-off setup, not all possible lighting/material configurations.

| Counter | Quarter, launches 1 / 2 / 3 | Half, launches 1 / 2 / 3 |
|---|---|---|
| Registers allocated per SM | 63985.55 / 63970.20 / 63976.95 | 64087.05 / 64081.25 / 64122.80 |
| RT-core cycles executed (%) | 51.72 / 51.76 / 51.87 | 52.75 / 52.78 / 52.60 |
| L1/TEX long-scoreboard stall (%) | 9.21 / 9.23 / 9.24 | 9.15 / 9.13 / 9.16 |
| RT-core long-scoreboard stall (%) | 11.61 / 11.59 / 11.56 | 11.54 / 11.57 / 11.52 |
| Barrier stall (%) | 0 / 0 / 0 | 0 / 0 / 0 |

**Stage 2 remains below its exit:** about 16 active warps per SM, not 24. Removing the register
regressions, specializing the guide and rejecting the slower split does not establish the
estimated 7 / 1.7 ms target. The register probe remains 168 with environment lighting on.

**Environment-on images:** buffered versus shared-guide frame 128 differs in 2.3212% of pixels,
mean 0.02327/255, maximum 7. Two independent buffered captures themselves differ in 2.2267% of
pixels, mean 0.02234/255, maximum 10. The cross-path difference is comparable to repeat-run
variation in floating-point guide learning; it does not establish byte identity. Mean RGB
changes are below 0.001/255. Exact-guide captures continue to use the original deterministic
16x16 path. The final shared-path repeat differs in 2.3316% of pixels, mean 0.02340/255,
maximum 8, confirming comparable variation without changing guide paths.

**Environment-on timing, quarter GI:** guide total is the median per-frame sum of trace, prepare
and finish durations, not total frame time. Every run uses the same shader revision as the table
above. Detailed counters are retained in `final-timing-summary.json`.

| Path / launch | Trace | Prepare | Finish | Trace + guide | Registers / thread | Warps / SM |
|---|---:|---:|---:|---:|---:|---:|
| Buffered 1 | 4.361 ms | 0.062 ms | 0.308 ms | 4.731 ms | 168 | 11.59 |
| Buffered 2 (retry) | 3.518 ms | 0.061 ms | 0.390 ms | 3.968 ms | 96 | 19.29 |
| Buffered 3 | 4.274 ms | 0.062 ms | 0.327 ms | 4.660 ms | 96 | 15.96 |
| Shared 1 | 4.905 ms | included | included | 4.905 ms | 192 | 7.99 |
| Shared 2 | 3.201 ms | included | included | 3.201 ms | 128 | 15.97 |
| Shared 3 | 3.840 ms | included | included | 3.840 ms | 128 | 13.31 |

The buffered path's best run is 24% slower than the shared path's normal launch once the extra
passes are counted. It also varies at the same register count, so allocation alone does not
explain its instability. **Buffered environment guides are therefore experimental, not the
default.** The final C++ selection restores the shared path for lit environments; it does not
change the qualified no-environment shader, dispatch dimensions, or workload. Both module
selection overrides remain available for further experiments.

**Other optional variants:** the combined-loop timing trial used 96 registers / 19.54 warps per
SM and took 1.997 ms, versus the separate-loop median of 1.948 ms; its diagnostic probe reported
168 registers. It is not promoted. The compact-stream trial used 128 registers / 15.62 warps
and took 1.928 ms, about 1% faster. That single trial does not meet the three-launch stage 3
qualification gate, and the stream remains opt-in. Its memory measurement follows below.

**Compact-stream memory:** standalone ZenApp, the same 4K quarter-GI setup, 2,000 frames,
validation on, two frames in flight, no screenshot; exact arguments and input hashes are in
`memory-{control,stream}.inputs.json`. Windows GPU Process Memory counters sampled the NVIDIA
process 20 / 19 times respectively:

| Metric | Default | Compact stream |
|---|---:|---:|
| Renderer device-local peak | 7,845,134,496 bytes | 7,943,493,072 bytes |
| Sampled process dedicated peak | 8,258,646,016 bytes (7.691 GiB) | 8,367,370,240 bytes (7.793 GiB) |
| Headroom below 8 GiB | 315.94 MiB | 212.25 MiB |

The logged stream allocation is 98,358,576 vertex bytes plus 10,672 material bytes. The process
counter includes overhead outside the renderer allocator; its samples are not a lifetime-peak
guarantee. This was measured on the RTX 5080 16 GB, not physical 8 GB hardware. Memory fits this
sampled preset, but the small single-run gain does not justify enabling the stream by default.

After the final selection change, the release build and all four suites passed again (1,102
passed). `release-quarter.ppm` and `release-half.ppm` remain byte-identical to their baselines.
`release-environment` selects the shared shader; `release-buffered-environment` verifies that the
explicit buffered override still renders. All four captures use Vulkan validation and report
no errors. `release-checks.json` confirms every timed shader hash is unchanged; the default
no-environment probe remains 128 registers. No post-selection timing improvement is inferred
from the policy change itself.

**Qualification still open:** stage 2's 24-warps target, stable environment-on timing, and the
three-launch gain gate for every stage 3 item are not passed. The planned full cost-ablation
campaign and source-level shader-profiler analysis have not been run; the measurement tools and
isolated modules are implemented. These results close the verification defects and establish the
default environment-off improvement; they do not claim that every original plan exit is met.

### Finishing stages 0-3 (2026-11-12)

Evidence: `build/hybridtrace-optimization/finish-20261112/`. Every Nsight capture folder keeps
`application-command.json`, `command.json`, `inputs.json` (executable, setup, package, all shipped
SPIR-V and any selected module, with SHA-256), `summary.json` (actual framebuffer, render and GI
extents, fullscreen, backend, async compute) and the BASE export; raw traces were moved with
SHA-256 verification to `C:/Users/cxy/.codex/artifacts/ZenEngine/hybridtrace-finish-20261112/`
(`trace-location.json` per folder). `timing-results.json` holds every timed launch with its exact
arguments; `report.py` and `occupancy.py` derive the tables below. All captures used one
executable (SHA-256 `3d16269cbb150aea...`), standalone ZenApp through `tools/profile_hybrid_gi.py`,
3840x2160 fullscreen, threaded RHI, async compute and GPU culling on, base clocks, 30 frames after
30 s. Setups: `verify-20261112/quarter.zenrender` (`fb89e437...`), `half.zenrender`
(`e22033dd...`), `quarter-env.zenrender` (`348cc267...`). No shipped shader changed: every timed
launch has the same `Data/SpvShaders` hashes as the follow-up qualification.

Changes kept, all diagnostic or tooling; the default image, shaders and dispatches are unchanged:

- `ZenApp --hybrid-ray-counts` (requires `--frames=2` or more) switches the last two frames to the
  existing 16x16 counting trace and logs the per-feature compute rays, HW rays, field work and HW
  candidate-loop iterations. The counting path keeps the pixel estimator without ReSTIR at full GI
  resolution (existing `HybridGIRenderer` behaviour), so its rays are those of the profiled view at
  native GI extent, not the quarter-GI image.
- `tools/build_hybrid_trace_variants.py`: `--buffered-guide --no-environment` builds the cost
  ablations of the current default (its control is byte-identical to the shipped
  `hybrid_trace_restir_hardware_buffered.comp.noguide.omm.spv`); `--define` and `--shader-root`
  build experiments from a copied shader tree.
- `tools/profile_hybrid_gi.py --shader-profiler` now also passes a metric set (`--metric-set-name`,
  default `Top-Level Triage`); without one, Nsight never attached (`failed/`).

#### Item 1: stage 0 measurements

**Cost ablations**, quarter GI, environment off, 8x8 no-guide default, three launches each,
interleaved with three control launches (`ablation-*`; modules in `ablation-modules/`). The
ablations change the image and are measurement-only.

| Variant | Launches (ms) | Registers | Median | Saved vs control | Share of pass |
|---|---|---|---:|---:|---:|
| Control (shipped module) | 1.952 / 1.948 / 1.946 | 128 / 128 / 128 | 1.948 ms | | |
| No shadow ray | 1.778 / 1.773 / 1.772 | 128 x3 | 1.773 ms | 0.175 ms | 9.0% |
| Constant material (no texture reads, no normal map) | 1.349 / 1.271 / 1.297 | 128 / 96 / 96 | 1.349 ms at 128 | 0.600 ms | 30.8% |
| No normal map | 1.675 / 1.672 / 1.614 | 128 / 128 / 96 | 1.674 ms at 128 | 0.274 ms | 14.1% |
| One light (of 24) | 1.607 / 1.588 / 1.582 | 128 x3 | 1.588 ms | 0.360 ms | 18.5% |
| No reflection ray (with its hit shading and shadow ray) | 1.447 / 1.446 / 1.445 | 128 x3 | 1.446 ms | 0.502 ms | 25.8% |

Shares are taken against the control at equal (128) register allocation; they overlap and do
not add up (constant material includes the normal map). Material texture reads are the largest
component, then the reflection ray, the 24-light loop, the normal map and the shadow ray. Spread
of every all-128 group is at most 1.6%. Three ablation launches compiled to 96 registers with
19.5 warps per SM and local-memory spills (0.78-0.91 G local sectors per pass); they ran 2-6%
faster than the same module at 128.

**Driver statistics** (`--pipeline-statistics`, `probe-results.json`; the probe shows one compile
with capture flags, registers are a worst-case reading):

| Module | Registers | Local memory / thread | Shared memory / workgroup |
|---|---:|---:|---:|
| Default (quarter and half) and every ablation above | 128 | 16 B | 5,632-6,144 B |
| No reflection ray | 128 | 16 B | 2,816 B |
| Combined ray-slot loop | 168 | 16 B | 1,536 B |
| Default with `--hybrid-hit-stream` | 128 | 32 B | 6,144 B |
| Environment-on shared 16x16 / buffered 8x8 | 190 / 168 | 80 B | 11,040 / 4,864 B |

No shader declares shared memory in the no-guide default; its 92 bytes per thread scale with the
number of inlined ray-query sites (halved without the reflection loop) and are the driver's
ray-query state. The Nsight counter `tpc__l1tex_sram_bytes_mem_untagged_data_shared_*` confirms
93.5 bytes per active thread and a 64 KiB shared carve-out (`occupancy.py`).

**Source-level hot spots:** `shader-profiler-default-2` is one real-time shader-profiler capture
of the default (1.946 ms, 128 registers, same as the timing launches). Nsight's `--auto-export`
writes only the pass tables; the source/SASS hot-spot table exists only inside the raw trace
(archived, SHA-256 `FFA5C3ED...`) and needs the Nsight GUI. **Not analysed: blocked on the GUI.**

**Alpha candidates** (`counts-quarter.log`, frames 127 and 128 agree within 0.005%):

| Slot (feature of the issuing ray) | HW rays, frame 128 | Candidate-loop iterations | Per ray |
|---|---:|---:|---:|
| Sky (diffuse primary rays; this 16x16 counting variant also traces source-tile visibility rays) | 44,999,257 | 0 | 0 |
| Specular (reflection primary rays) | 8,201,045 | 0 | 0 |
| Bounce (shadow rays from diffuse hits) | 32,265,107 | 0 | 0 |
| Reflection (shadow rays from reflection hits) | 8,064,940 | 0 | 0 |

Counts are at the native 3840x2160 GI extent (see above). The log confirms that the default
trace runs `VoxelGI/hybrid_trace_restir_hardware_buffered.comp.noguide.omm.spv` and the counting
trace `hybrid_trace_hardware_counts.comp.omm.spv`. New Sponza has 27 opaque materials and one
`BLEND` decal; with 4-state OMMs no ray reached the candidate loop. **Stage 3 item 4 is dropped:**
its component is 0% of the pass in the profiled view.

#### Item 2: stage 2 occupancy, environment off

**Outcome: blocked. Best stable point: the unchanged default, 128 registers, 15.62 warps per SM,
1.945 ms quarter / 8.066 ms half (table below).**

- **What holds the registers.** Every cost ablation, including removing the whole reflection loop
  or all texture reads, still probes at 128 registers. The 128 is therefore not one component's
  live state but the driver's allocation step above the shader's requirement.
- **128 is a real requirement, 96 is not enough.** Every 128-register launch of the default has
  zero local-memory traffic; every 96-register compile observed (combined loop, three ablation
  launches, earlier buffered launches) spills 0.8-1.8 G local sectors per pass. The spill-free
  requirement is above 96 and at most 128. The observed allocation steps are 64, 80, 96, 128,
  168, 192 and 240; nothing between 96 and 128 was produced.
- **Forced caps.** A workgroup of N threads caps registers at 65,536 / N. Isolated modules pack
  N / 64 consecutive 8x8 blocks of the unchanged dispatch grid into one workgroup, with the same
  lane-to-pixel order (`experiments/blocks/`, image-identical by construction, not shipped):

| Threads per workgroup | Probe registers / local memory | Result (one launch each) |
|---:|---|---|
| 512 | 128 / 32 B | 2.155 ms, 14.98 warps: 10.6% slower (one workgroup per SM, tail) |
| 576, 640, 768 | 96, 96, 80 / 176-304 B | **Device lost** (`VK_ERROR_DEVICE_LOST`, nvlddmkm event 153) at frame 1445-1456, reproduced without Nsight (`repro-t*.log`) |
| 1024 | 64 / 448 B | 2.791 ms, 30.08 warps, 3.9 G local sectors: 43% slower |

512 and 1024 ran 3,000 frames without a fault. The 80-96 register caps that would reach 20-24
warps cannot be timed on this driver (621.14).

- **Shared memory also limits 24 warps.** At 93.5 bytes of ray-query state per thread, 24 warps
  need 72 KiB of shared memory; the driver gave this kernel a 64 KiB carve-out in every launch.
- **Not tried:** removing ray-query sites by restructuring (the combined loop already showed that
  fewer sites raise the probe to 168), and anything that changes the image.

Stage 2's 24-warps exit is not met; the estimated ~1.7 / ~7 ms target is not reached.

#### Item 3: environment lighting on

Quarter GI, `quarter-env.zenrender`. Trace + guide is the per-frame median of the summed trace,
prepare and finish ranges.

| Path | Launches | Trace + guide | Registers | Warps / SM |
|---|---|---:|---|---|
| **Shared 16x16 (default), launches 1-6** | 3.198 / 3.196 / 3.199 / 3.203 / 3.202 / 3.202 ms | median 3.200 ms, spread 0.22% | 128 x6 | 15.97 x6 |
| Buffered 8x8 (explicit module) | 3.513 + 0.061 + 0.394 ms | 3.967 ms | 96 | 19.29 |
| Buffered, 512-thread workgroups (cap 128) | 3.991 + 0.061 + 0.174 ms | 4.231 ms | 128 | 14.29 |
| Buffered, 640/768-thread workgroups | | not timed | 96 / 80 | device lost (as above) |

- **Outcome: met on the final build, without a change.** All six default launches ran at 128
  registers and 3.196-3.203 ms, against the 3.20 ms normal launch of the follow-up. The
  high-register state (192, 4.9 ms) did not occur in these six launches, but the probe still
  reports 190 registers for this shader and nothing here prevents the driver from choosing it.
  Its trigger remains unidentified. Validation-enabled frame 128 renders without errors; two
  repeats differ in 2.29% of pixels, mean 0.023/255, maximum 5, in line with float guide learning.
- **Why the buffered path varies at equal registers.** In the follow-up's 96-register buffered
  launches (3.518 vs 4.274 ms) and 128-register shared launches (3.201 vs 3.840 ms), active warps
  and RT-core activity drop in exact proportion to the longer time (19.29 / 15.96 warps = 1.21,
  4.274 / 3.518 ms = 1.21; shared 1.20 and 1.20) while registers and shared memory are not
  exhausted. The marker range contains idle time that the shader does not cause; in the shared
  launches, adjacent passes (OffScreen, ReSTIR spatial) changed duration at the same time.
- **Why finish costs 0.3-0.4 ms.** The finish pass touches 385 words per guide tile. Its marker
  range shows 96-register warps, RT-core activity and 0.10 G local-memory sectors that only the
  spilling trace produces: it measures the trace's tail, not the guide average. The buffered
  path's real cost is therefore its trace + guide sum, still 24% above the shared path.
- **Rejected:** buffered as default (3.97 ms), forced-cap buffered (4.23 ms or device lost). A
  two-tile 512-thread shared-guide workgroup was not built: the 512-thread forced cap cost 10.6%
  without the guide, more than the variance it would remove.

#### Item 4: stage 3 qualification

**Hit stream**, three launches each, environment off, interleaved with the default:

| GI | Default (ms) | `--hybrid-hit-stream` (ms) | Median gain |
|---|---|---|---:|
| Quarter | 1.949 / 1.945 / 1.943 | 1.929 / 1.936 / 1.929 | 0.82% |
| Half | 8.065 / 8.066 / 8.077 | 8.031 / 8.044 / 8.055 | 0.27% |

Every stream launch is faster than every default launch at both resolutions, all at 128
registers, and stream frames 128 are byte-identical to the baselines. The gain is below the
plan's 2% drop threshold, and the stream costs 98.4 MB (212 MiB headroom left on the 8 GiB
preset). **Decision: opt-in, not default.**

**Placeholder stores:** in the full-resolution trace every store to a 1x1 placeholder is already
guarded (`rawBounce` by `hybrid.bounce.x`, `rawBouncePrevious` by `.w`, `rawReflection` /
`rawReflectionPrevious` by `hybrid.reflection.x` / `.w`, `nextBudget` by `hybrid.provider.w`,
reservoirs by `restirOptions` or pixel 0). `rawSky`, `skyGuide` and `rawSpecular` are full size.
**Dropped: nothing left to guard.** Items 1 (single geometry read) and 3 (zero-factor reads)
remain as recorded in the follow-up: geometry caching rejected, emission skip kept, metallic skip
removed for byte identity.

#### Final timing and checks

| Configuration | Launch 1 / 2 / 3 | Median | Registers | Warps / SM |
|---|---|---:|---|---|
| Quarter, environment off | 1.949 / 1.945 / 1.943 ms | 1.945 ms | 128 x3 | 15.62 |
| Half, environment off | 8.065 / 8.066 / 8.077 ms | 8.066 ms | 128 x3 | 15.12 |
| Quarter, environment on (trace, no guide passes) | 3.198 / 3.196 / 3.199 ms (4-6: 3.203 / 3.202 / 3.202) | 3.200 ms | 128 x6 | 15.97 |

Half GI is 3.8% slower than the follow-up's 7.771-7.796 ms with identical SPIR-V hashes and 128
registers. Active warps (15.12 vs 15.65) and RT-core activity (50.9% vs 52.8%) fall in
proportion, and a little graphics work overlaps the range (29 vs 2 pixel-shader registers per
SM): a scheduling difference outside the trace shader between the two executables, not a shader
change. Quarter GI matches the follow-up within 0.3%. Not measured: half GI with environment on.

Release build of the final tree; RenderCoreTest 790 (9 disabled), RayQueryIntegrationTest 124
(12 disabled), ConeVoxelGIIntegrationTest 192 and RuntimeLaunchTest 21 pass (`tests-summary.json`;
totals include disabled tests). Format check on changed C++/GLSL, `check_no_exceptions.py`,
`git diff --check` and Python syntax pass. Validation-enabled frame-128 captures (`release-*`,
`release-checks.json`): quarter and half byte-identical to `baseline/quarter.ppm` and
`refine-20261112/qualified-baseline-half.ppm`, also with `--hybrid-hit-stream`; environment on
renders with the shared path; no validation errors. Nothing is committed.

**Still open:** stage 2's 24-warps exit (blocked by the driver's allocation steps, the device loss
under forced caps and the 64 KiB carve-out); the trigger of the environment-on 190-register
compile; source-level hot spots (GUI); stage 4. (Stage 4 and the 190-register trigger are covered
in the next section.)

### Stage 4: fewer or cheaper rays (2026-11-12)

Evidence: `build/hybridtrace-optimization/stage4-20261112/`. Every Nsight capture keeps its exact
arguments, input hashes (executable, setup, package, shipped SPIR-V, selected module) and actual
extents as in the previous section; `timing-results.json` lists all of them and raw traces are in
`C:/Users/cxy/.codex/artifacts/ZenEngine/hybridtrace-stage4-20261112/`. Quality runs are under
`quality/<candidate>/` (`report.json`, per-capture `capture.json` with arguments and hashes,
lossless frames); `quality-summary.json` compares them. Candidates were built as isolated modules
from a patched copy of the shader tree (`experiments/patch_stage4*.py`); its control compiles
byte-identically to the shipped modules. Executable `b005fd1d00d4e7dd...` for every gate and
quality run (the single-launch screen used `3d16269cbb150aea...`).

**Gates.** The plan gives fixed limits only for energy (1%) and ground-truth `D_bounce` (1 point);
its 0.34/255 motion limit is already above the control (0.377/255 at quarter GI). Because the
environment-off default is deterministic (repeat captures are byte-identical, A/A difference zero),
the remaining gates compare against the same-build control with these stated tolerances:

| Gate | Pass condition |
|---|---|
| Timing | Median gain of at least 2% of the pass at quarter and half GI, environment off (three launches each, interleaved with control launches); environment on recorded |
| Static image (five views, quarter and half, frame 256 against the four-ray reference) | Pooled relative RMS at most 1% (relative) above control at each resolution; no view's RMS more than 0.05/255 worse |
| Motion (eight poses) | Mean maximum-channel error at most 1% (relative) above control at each resolution |
| Sparks (divisors 4, 2, 1; moving and static) | One-pixel and four-plus counts per frame at most 0.5 above control |
| Energy (4K noise protocol, normal maps off) | Mean final colour, specular GI and `D_bounce` within 1% of control |
| Ground-truth `D_bounce` | Within 1 point, on/off on the same build |
| ZenTest GPU fixtures | The four suites, ReSTIR fixtures in all four submission modes |

`tools/gi_candidate_quality.py` runs the static, motion, spark and energy gates against the
matched reference images of `matched-4k-20261112-v2` without recapturing them. Its control run
reproduces that dataset's report exactly (center 3.413% / 2.891%, motion 0.3769 / 0.2484, sparks
10.50 / 7.42 / 7.42 / 5.25).

**Prerequisite for item 1.** The package was cooked with `--texture-mipmaps on` and no atlas option
(`cook-bc.provenance.json`; no atlas entry in the cook log), so its textures carry mip chains and no
spatial atlas pages. The ray-cone code still keeps atlas regions (single-level pages) at level zero.

#### Timing, quarter GI screen and three-launch gate

Medians of three launches; control interleaved in every round. Environment-on launch 1 of every
new module is a driver-cold compile (see the last subsection); its median is over launches 2 and 3.

| Item | Variant | Quarter, env off | Half, env off | Quarter, env on (launch 1 / 2-3) |
|---|---|---:|---:|---|
| Control | shipped pre-stage-4 | 1.943 ms | 7.782 ms | 3.204 ms (128 regs throughout) |
| 1 Mip | ray cone, narrow (0.18 rad diffuse, 0.25 alpha specular), one level | 1.767 (-9.1%) | 7.037 (-9.6%) | 4.740 / 3.052 (-4.7%) |
| 1 Mip | wide (0.72 rad), screen only | 1.757 (-9.8%) | | |
| 1 Mip | first form, per-texture UV/position loads, screen only | 2.241-2.276 (+15-17%) | | |
| 1 No normal map | on diffuse-ray hits | 1.716 (-11.7%) | 6.861 (-11.8%) | 4.718 / 3.067 (-4.3%) |
| 2 Rough reflection | roughness >= 0.5 from diffuse hits | 1.793 (-7.7%) | 7.102 (-8.7%) | 4.368 / 2.989 (-6.7%) |
| 2 Rough reflection | roughness >= 0.7, screen only | 1.975 (+1.5%) | | |
| 3 Light candidates | 4 of 24, RIS | 1.810 (-6.8%) | 7.270 (-6.6%) | 4.496 / 3.004 (-6.2%) |
| 3 Light candidates | 8 of 24, screen only (96-register launch) | 1.915 (-1.6%) | | |
| 4 Settled rays | 2 rays for unchanged, settled, non-responsive receivers | 1.203 (-38.1%) | 4.796 (-38.4%) | 3.271 / 2.175 (-32.1%) |

Rough reflection at 0.7 is dropped on timing (fewer receivers qualify than its bookkeeping costs);
8 light candidates fall under the 2% threshold. Every other row passes the timing gate.

#### Quality against the four-ray reference

| Candidate | Pooled rel. RMS q / h | PSNR q / h | Worst view RMS change | Motion q / h /255 | One-pixel sparks moving q / h / native | Static q / h / native | Energy colour / spec. GI / D_bounce |
|---|---|---|---|---|---|---|---|
| Control | 5.469% / 4.640% | 51.89 / 53.32 dB | | 0.3769 / 0.2484 | 10.50 / 7.42 / 18.17 | 7.42 / 5.25 / 2.75 | |
| Mip, narrow | 5.512% / 4.699% | 51.82 / 53.21 | +0.016 | 0.3734 / 0.2475 | 10.50 / 8.58 / 17.25 | 7.42 / 5.08 / 2.75 | -0.17% / -0.11% / **-1.40%** |
| No normal map, diffuse hits | 5.475% / 4.650% | 51.88 / 53.30 | +0.004 | 0.3775 / 0.2492 | 10.50 / 7.50 / 18.17 | 7.42 / 5.17 / 2.75 | 0 / 0 / 0 (protocol has normal maps off) |
| Rough reflection >= 0.5 | 5.252% / 4.484% | 52.24 / 53.61 | +0.036 (C5) | 0.3546 / 0.2408 | 8.25 / 5.75 / 13.50 | 7.08 / 5.25 / 2.75 | -0.01% / -0.28% / 0.00% |
| 4 light candidates | **7.304% / 6.068%** | 49.38 / 50.99 | **+0.42** | **0.5537 / 0.3947** | 9.58 / 7.25 / **26.00** | 6.75 / 5.17 / 2.75 | -0.15% / **-5.07%** / +0.87% |
| 2 settled rays | **5.567% / 4.698%** | 51.73 / 53.21 | +0.023 | **0.3933 / 0.2625** | 10.50 / 7.42 / 18.17 | 7.25 / 5.25 / 2.75 | -0.04% / 0.00% / +0.02% |

Worst view RMS change is in /255. Bold entries fail their gate. Four-plus sparks stay within 0.5
per frame of control for every candidate except four light candidates (1.83 vs 1.25, moving at
quarter GI); all counts are in `quality-summary.json`.

- **Item 1, mip selection: rejected.** The ray-cone level saves 9-10% once it reuses the read's own
  corner UVs and the query's triangle area, but it darkens the bounce: noise-protocol `D_bounce`
  -1.40% (limit 1%), and the half-GI pooled RMS rises 1.3%. A wider cone saves no more time. The
  narrow form is the conservative choice; a still narrower cone was not tried.
- **Item 1b, no normal map on diffuse-ray hits: accepted.** Every gate is within tolerance. The
  energy protocol cannot see it (the ground-truth batch rejects normal-mapped setups), so the
  normal-maps-on check is the matched captures' mean display colour: +0.11% to +0.17% on the four
  lit views, +2.5% on the near-black C5 (0.535/255 reference signal).
- **Item 2, rough reflections from diffuse hits: accepted.** For receivers with roughness >= 0.5,
  each diffuse ray is reused as a sample of the normalized GGX lobe with weight
  `GISpecularDensity / pdf`, for reflected radiance, specular occlusion (escaped rays), hit distance
  and the reflection reservoir; the reflection ray is not traced. Specular occlusion receives the
  same treatment. Quality improves on four of five views, motion and sparks (four lobe samples per
  receiver instead of one ray). The one regression is C5, +0.036/255 RMS (-1.03 dB) on a
  near-black view, inside the 0.05/255 tolerance.
- **Item 3, light selection: rejected.** Resampled importance sampling over 4 uniformly drawn
  candidates of the 24 unbounded shadowed lights (unbiased: weights over the uniform proposal)
  raises pooled RMS 31-34%, motion error 47-59%, native moving sparks 18 -> 26 and loses 5% of the
  specular GI mean in the energy protocol. 8 candidates gain under 2%.
- **Item 4, two rays for settled receivers: rejected.** The settled test uses state the trace
  already binds: unchanged receivers (`reuseData.y`), no responsive lighting window, and a full
  history length; settled receivers trace 2 instead of 4 diffuse rays (no hysteresis is needed
  because the test does not depend on per-frame variance). It saves 38% but raises pooled RMS 1.8%
  / 1.25% and motion error 4.4% / 5.7%; energy is unchanged. Its static gain cannot be kept at the
  four-ray quality the plan asks for.
- **Ground-truth `D_bounce` on/off:** not run. The `newsponza/v3` dataset's input package
  (`E:/Dev/ZenEngine/Cache/SceneAssets/45805307d6863205.zenasset`) was deleted while the queue ran
  (the whole `Cache` folder disappeared and E: gained 54 GB; not by this session), so
  `ground_truth_workflow.py run` fails its input verification. For the accepted state the
  comparison holds by construction: the dataset's trace (`HybridTraceHardwareSP`, non-ReSTIR,
  normal maps off) and every other non-ReSTIR module are byte-identical to the pre-stage-4 build
  (`changed-shaders.json`: only the 24 per-pixel ReSTIR trace modules changed). The mip and
  settled-ray items, which would change that trace, were already rejected on other gates.
  `ZenApp --hybrid-trace-override=<program>=<file.spv>` and `ground_truth_workflow.py run
  --zenapp-arg` were added for this comparison and remain available.

#### Accepted state: shipped default

`Data/Shaders/VoxelGI/hybrid_trace.glsl` and `ray_hit_lighting.glsl` implement items 1b and 2 for
the per-pixel ReSTIR traces only (`HYBRID_RESTIR`, not the combined loop): reference, ground-truth,
probe, field-only and non-ReSTIR traces are unchanged. Rough reflection from diffuse hits also
requires bounce rays and reflections on, at most 4 samples, and the same provider for each
visibility/radiance pair. No setting, binding, pass or dispatch changed, so existing setups load
unchanged and no ZenTest expectation moved. The shipped modules are byte-identical to the timed
port modules, and frame 128 of the shipped default is byte-identical to the combined experiment
module at quarter GI.

The first port held a separate struct for the reflection sums. With environment lighting on, it
compiled to 224 registers in three of three launches (4.56 ms). The same logic with plain
globals, as in the experiment, runs at 128 registers once warm. The guards did not matter (a copy
without them behaved the same).

**Final timing**, three launches each, interleaved with the pre-stage-4 modules (byte-identical to
the previous shipped SPIR-V), quiet desktop. An earlier attempt of this table ran while other
desktop applications were active and is kept in `superseded/` (control launches fell to 13.9-14.7
warps per SM).

| Configuration | Pre-stage-4, launches | Final default, launches | Medians | Change |
|---|---|---|---|---:|
| Quarter, env off | 1.943 / 1.944 / 1.943 ms (128 regs) | 1.546 / 1.576 / 1.548 ms (96 / 128 / 96 regs) | 1.943 -> 1.548 ms | -20.3% |
| Half, env off | 7.778 / 7.782 / 7.769 ms (128) | 6.196 / 6.228 / 6.190 ms (96 / 128 / 96) | 7.778 -> 6.196 ms | -20.3% |
| Quarter, env on | 3.207 / 3.204 / 3.203 ms (128) | 2.822 / 2.811 / 2.818 ms (128) | 3.204 -> 2.818 ms | -12.1% |

The environment-off default now alternates between a 128-register and a spilling 96-register
compile (19.2 warps per SM); both are faster than before, and the three launches agree within 2%.
The first-ever launch of the new environment-on shader was the driver-cold 224-register compile
(4.25 ms in `envport-port-on-1`); later launches, including the warm-up `final2-env-default-0`
(2.814 ms), ran at 128.

**Final quality** (`quality/final-default`, same images as the timed build): pooled relative RMS
5.261% / 4.503% (control 5.469% / 4.640%), PSNR 52.23 / 53.58 dB (51.89 / 53.32), motion 0.3564 /
0.2422 /255 (0.3769 / 0.2484), one-pixel sparks moving 8.17 / 5.58 / 13.50 and static 7.08 / 5.17 /
2.67 per frame (control 10.50 / 7.42 / 18.17 and 7.42 / 5.25 / 2.75), energy colour -0.01%,
specular GI -0.28%, `D_bounce` 0.00%. Worst view: C5 +0.039/255 RMS at quarter, +0.028 at half;
all other views improve by 0.010-0.054/255.

**Environment lighting on** is outside the matched quality protocol. Frame 128 changes visibly from
the pre-stage-4 image (mean colour +0.39%, 1.26% of pixels above 2/255, maximum 142); two repeats of
the new default differ in 4.21% of pixels (maximum 14/255) against 2.29% (maximum 5) before, so
float guide learning now also feeds the reflection estimate. Its quality is not certified here.

**Checks on the final tree:** release build; RenderCoreTest 790 (9 disabled), RayQueryIntegrationTest
124 (12 disabled; includes the ReSTIR fixtures in all four submission modes),
ConeVoxelGIIntegrationTest 192 and RuntimeLaunchTest 21 pass (`tests-summary.json`); format check
of changed C++/GLSL, `check_no_exceptions.py`, `git diff --check` and Python syntax pass.
Validation-enabled frame-128 captures with environment lighting off (quarter, half, repeat
byte-identical) and on (two repeats) report no errors. **New retained references** replace
`baseline/quarter.ppm` for byte-identity checks: `stage4-20261112/reference/default-quarter.ppm`
(`47a2661d...`), `default-half.ppm` (`27e77fbe...`), and for environment on (repeat variation
applies) `default-environment.ppm` (`5267ffff...`). The new ZenApp option
`--hybrid-trace-override` has no ZenTest coverage (launch-option parsing is not reachable from the
suites; the same holds for the earlier diagnostic options). Nothing is committed.

#### Environment-on 190-register trigger

The gate's environment-on candidates all compiled to 192-224 registers (7.5-8.0 warps per SM,
4.4-4.7 ms) on their first launch and to 128 registers (15.97 warps) on launches 2 and 3. Two
"salted" modules that differ from the shipped shader only in one SPIR-V debug name
(`HYBRID_CACHE_SALT`, identical code) repeat it: 192.1 / 191.9 registers on launch 1, 128 on
launch 2 (`salt1-env-*`, `salt2-env-*`). **The trigger is the driver's first compile of SPIR-V it
has not cached**; a cached shader compiles to 128 (the six environment-on default launches of the
previous section, all after earlier runs of that shader). The environment-off no-guide shader does
not show it (its first launches were 96 or 128). Not established: whether the driver's cache
persists across driver updates or cache clears on user machines; nothing in the engine can request
the warm compile.

#### Stage 4 outcome

| Item | Decision | Pass gain (quarter / half, env off) | Quality |
|---|---|---:|---|
| 1 Ray-cone mips | Rejected | -9.1% / -9.6% | `D_bounce` -1.40% (limit 1%); half pooled RMS +1.3% |
| 1b Normal map skipped on diffuse hits | **Default** | -11.7% / -11.8% | Within all gates |
| 2 Rough reflections from diffuse hits (>= 0.5) | **Default** | -7.7% / -8.7% | Better on pooled RMS, motion and sparks; C5 +0.036/255 |
| 2 at roughness >= 0.7 | Dropped | +1.5% | Not measured |
| 3 Light candidates (4 / 8 of 24) | Rejected / dropped | -6.8% / -6.6% (8: -1.6%) | Pooled RMS +34%, motion +47% |
| 4 Two rays for settled receivers | Rejected | -38.1% / -38.4% | Pooled RMS +1.8%, motion +4.4% / +5.7% |
| Accepted set together | **Default** | -20.3% / -20.3% (env on -12.1%) | Pooled RMS -3.8% / -3.0%, motion -5.4% / -2.5% |

None of the rejected items is selectable in the engine; their modules and sources stay in the
evidence folder. **Still open:** ground-truth `D_bounce` runs (dataset input missing); environment-on
quality certification; a narrower mip cone or a settled test that keeps four rays where history is
noisy; stage 2's occupancy exit; source-level hot spots (GUI).

### Targets

| State | Half GI | Quarter GI | Basis |
|---|---:|---:|---|
| Baseline, normal launch | 10.5 ms | 2.6 ms | Measured |
| Baseline, slow launch | 15.6 ms | 3.9 ms | Measured |
| Stages 1-2: three workgroups' worth of warps (24 per SM) | ~7 ms | ~1.7 ms | Estimate: a latency-bound pass speeds up with warps in flight, 16 -> 24 |
| Same rays, RT core saturated | ~4.5 ms | ~1.1 ms | Estimate: the RT core is busy 42.6% of the pass today |

The last row is the floor for the current ray count. Going below it needs fewer or cheaper rays
(stages 3 and 4).

### Baseline source observations

- **Where active warps wait.** The listed stall reasons sum to 31.8% of issue slots, about the
  33.3% that hold a warp. As shares of that: L1/TEX 29%, RT core 25%, wait 15%, issued 7.5%,
  barrier 5.4%, short scoreboard 4.7%, no instruction 4.0%. L1/TEX covers buffer loads (indices,
  vertices, materials, guide) as well as texture samples.
- **The guide runs although this setup does not use it.** With environment lighting off,
  `residualEnvironment` is false, every diffuse ray is a cosine ray and the sky outputs are zero.
  The pass still runs `BuildGuide` (25 x 384 = 9,600 `guidePrevious` reads per workgroup and two
  barriers), `GuideTotal` per receiver, and the `FinishGuide` barrier. The 16 `guideRows` floats
  stay live across every diffuse ray query.
- **The 256-thread workgroup exists only for the guide's shared memory.** It is also why a warp
  that finishes tracing early keeps its registers until the whole tile reaches the last barrier.
- **Up to nine inlined ray-query sites.** Five `TraceFeature` sites (sky, separate bounce, source
  tiles, specular, separate reflection) plus the shadow and environment-visibility queries inside
  `HardwareHitRadiance`, which is inlined into both `TraceDiffuse` and `TraceSpecular`. Each site
  carries the alpha-test candidate loop (`RayCandidateAccepted`: material, vertex colours, UV, one
  texture).
- **State held across queries that could be stored earlier.** `DiffuseResult d` (16 words) and
  `initialGI` (11 words) stay live through all of `TraceSpecular`. In `HardwareHitRadiance` the
  emission texture is read after the shadow query, so the hit's triangle, barycentrics and
  material fields stay live across it. A shadow query builds the full hit record (position,
  normal, cell) although only `.hit` is read. The reconstruction fallback pass already showed this
  effect: values kept live across its queries made launches swing between 24.6 and 34.8 ms.
- **Per-hit memory traffic.** Each helper re-reads the index triplet (`RaySurfacePosition`,
  `RayGeometricNormal`, `RayHitNormal`, the colour loop, and `RaySurfaceUV` once per texture).
  `Vertex` is 112 bytes with colour at offset 96. Then come the node matrices, the full `Material`
  record and four textures at mip 0 (base colour, metallic-roughness, normal, emission). This
  happens for up to 5 hits per GI texel.
- **Light loop.** The profiled setup has 24 enabled, shadowed lights (23 point lights with
  unbounded range, one directional). Each hit evaluates all 24, then traces one shadow ray: 120
  light evaluations per GI texel. No light can be culled by range in this setup.

### Stage 0: measurement groundwork

1. **Register probe.** Restore the `VK_KHR_pipeline_executable_properties` readout used in
   [NsightOptimizations.md](NsightOptimizations.md) (it reported 128 registers and 18,192 bytes of
   shared memory for the all-HW trace) as an opt-in ZenApp startup diagnostic that logs each trace
   pipeline at creation. It gives the register count without a GPU trace, and shows in every
   launch whether the ~200-register compile happened.
2. **Cost breakdown by ablation.** Time measurement-only variants, three launches each: no
   shadow ray, constant material (no texture reads), no normal map, one light, no reflection ray,
   no guide. This replaces the pre-spark ray-count scaling and orders stages 3 and 4 by data.
   Variants are separate modules selected at startup; the shipped module in the shared
   `Data/SpvShaders` stays untouched while other sessions capture.
3. **Source-level hot spots.** One capture with `tools/profile_hybrid_gi.py --shader-profiler`.
4. **Protocol for every step.** Half and quarter GI, at least three launches including the first
   after a rebuild, with registers per thread, warps per SM and the stall table recorded per launch.

### Stage 1: stable register allocation (direction 1)

Image-identical. Sample sequences and seeds do not change.

1. **Store results when they are final.** Call `StoreDiffuse` and write the packed GI reservoir
   before `TraceSpecular`, so neither is live across the reflection ray.
2. **Fetch, then light.** In `HardwareHitRadiance`, do every vertex, material and texture read
   first, emission included. The light loop and the shadow query then hold only position, two
   normals, reflectance and emission.
3. **Visibility-only query.** A `HardwareOccluded` entry in
   [hardware_ray_query.glsl](../../Data/Shaders/VoxelGI/hardware_ray_query.glsl) that returns a
   bool and builds no hit record, used by the shadow, environment-visibility and source-tile
   queries.
4. **One ray site.** Merge the diffuse and specular loops into one loop over ray slots with a
   single `TraceFeature` and a single `TraceHitRadiance` site. The compiled program shrinks to one
   copy of hit shading (the no-instruction stall is 4% of active time), and the allocator no
   longer sees two inlined copies with different surrounding state.

Exit: the probe reports at most 128 registers on every launch, including the first after a
rebuild, and three launches agree within 2%. This removes the 15.6 ms launches. If a slow launch
still appears with an unchanged register count, the cause is not allocation and needs its own
investigation before stage 2.

### Stage 2: occupancy (direction 2)

1. **Move the guide out of the trace.** A `HybridGuidePrepare` pass writes each tile's pooled
   guide (weights, row moments, power) to a buffer, in the layout the half-resolution variant
   already publishes. The trace reads it through the existing `GuideWeightAt` / `GuideRowMomentAt`
   buffer path of the fallback variant and learns with compare-and-swap into an accumulation
   buffer. A `HybridGuideFinish` pass averages it. This is the guide-finalization experiment
   prepared but never timed in NsightOptimizations.md. The trace then has no shared memory and no
   barrier. When the environment contributes nothing, neither guide pass is scheduled and the
   trace skips `GuideTotal`. Buffer cost: 2,320 bytes per tile, 18.9 MB at half GI and 4.7 MB at
   quarter.
2. **8x8 workgroups.** With no shared guide, 64 threads per group remove the 256-thread
   quantization (assuming the 64K-register file and whole-group allocation, to be confirmed with
   the warp counter):

   | Registers / thread | Warps / SM, 16x16 | Warps / SM, 8x8 |
   |---:|---:|---:|
   | ~200 | 8 | 10 |
   | 128 | 16 | 16 |
   | 113 | 16 | 18 |
   | 102 | 16 | 20 |
   | 93 | 16 | 22 |
   | 85 | 24 | 24 |
   | 64 | 32 | 32 |

3. **Reduce registers toward 85 and below**, using the probe after each change. With 8x8 groups
   every ~10 registers is one step, so partial progress pays.
4. **Trace/shade split, only if the count stays above ~100.** The trace writes hit records
   (instance, geometry, primitive, barycentrics: 12 bytes per ray) and a second dispatch shades
   them. Five rays cost 60 bytes per GI texel: 125 MB at half GI, 31 MB at quarter. The 4K preset
   has about 235 MiB of headroom under 8 GiB, so this is the last resort, not the first step.

Risks: with environment lighting on, guide reads move from shared memory to a buffer. The earlier
shared-row and recomputed-row experiments were neutral or slower, although they kept the
256-thread group. Environment-on timing must be measured alongside the profiled setup.

Exit: at least 24 warps per SM on every launch at both resolutions, with the time recorded
against the estimate in the targets table.

### Stage 3: cheaper hits, same image (direction 3)

Order by the stage 0 ablation; drop any item whose component costs under 2% of the pass.

1. **Read hit geometry once.** Fetch the three indices once and one UV per used set, and pass
   them to position, normals, colour and every texture read.
2. **A hit-shading stream.** A vertex stream holding only what hit shading reads (normal,
   tangent, colour) at source precision, and a compact material record (texture indices, factors,
   the transforms actually used) in place of the 112-byte `Vertex` and the full `Material`. Its
   memory cost must be recorded against the 8 GiB preset.
3. **Skip reads that are multiplied by zero.** Emission texture when the emissive factor or
   `gi.lighting.z` is zero; metallic-roughness when the metallic factor is zero. Expected small.
4. **Alpha candidates.** Confirm the capture used the `.omm.spv` module, and count candidate-loop
   iterations per ray by extending `HYBRID_COUNT_RAYS`. Act only if the count is significant.
5. **Guard dead stores.** Skip `imageStore` to outputs that are 1x1 placeholders in the active
   configuration.

Exit: byte-identical captures on the same build with each item on and off; each kept item shows a
gain in all three launches.

### Stage 4: fewer or cheaper rays (directions 3 and 4, quality-measured)

Each item changes the image and is accepted only through the checks below.

1. **Mip selection at secondary hits.** Choose the mip from a ray cone (hit distance times the
   ray's spread: wide for diffuse rays, from roughness for the reflection ray) instead of mip 0.
   This is the main lever against the L1/TEX stall. Skipping the normal map on diffuse-ray hits
   is a second variant to measure. First confirm that the cooked package has mip chains and how
   atlas regions behave above mip 0.
2. **Two diffuse rays for settled receivers.** Extend the existing reduced-budget mechanism
   (`previousBudget`, exit hysteresis, unchanged-receiver test). It needs a settled test based on
   bounce history or reservoir age, since the current one reads sky moments, which are zero here.
   Two rays everywhere saved about a third of the pass and cost 7.22% -> 8.55% relative RMS; the
   adaptive form must stay near 7.22%.
3. **Rough reflections from the diffuse hits.** Above a roughness threshold, weight the diffuse
   hits by the ratio of lobe to cosine density instead of tracing the reflection ray. This saves
   one of five primary rays and its shadow ray. The weights grow without bound as roughness
   falls, so the threshold comes from measurement.
4. **Light selection without evaluating every light.** Only if the one-light ablation shows the
   loop matters. It changes the noise of the shadowed estimate.

Checks, as in [SparkFixPlan.md](SparkFixPlan.md): spark counts at divisors 1, 2 and 4 moving and
static, 4K noise protocol means within 1%, ground-truth `D_bounce` within 1 point with an on/off
comparison on the same build, the eight-pose motion test not worse than 0.34/255, and the ZenTest
GPU fixtures in all four submission modes.

Outcome (2026-11-12, **Stage 4: fewer or cheaper rays (2026-11-12)** above): the normal-map skip
on diffuse-ray hits (item 1's second variant) and rough reflections from the diffuse hits
(item 3, threshold roughness 0.5) are the default for per-pixel ReSTIR traces. Ray-cone mips
(item 1) failed the `D_bounce` energy limit, two rays for settled receivers (item 2) the static and
motion tolerances, and light candidates (item 4) every quality gate; all three are rejected. The
0.34/255 motion limit was already exceeded by the pre-stage-4 control (0.377/255); the gate
compared against the same-build control instead. The ground-truth `D_bounce` runs could not
execute (missing dataset input); the accepted changes leave that dataset's trace byte-identical.

### Not planned

- **A radiance cache for HW hit shading.** Actual-hit shading replaced it on 2026-11-09 and
  reduced bounce error from 19.37% to 2.78% (C1_L1).
- **Acceleration-structure build changes.** The BLAS already builds with prefer-fast-trace.
- **Shared guide rows, recomputed rows inside the 256-thread group, all-HW specialization.**
  Measured and rejected in NsightOptimizations.md.
- **Separate diffuse and specular dispatches.** Both would contain hit shading, where the
  register peak is expected; stage 1 item 4 covers the same ground without a second dispatch.
- **Two rays everywhere.** Rejected by the 8.55% error above.

### Scope and remaining decisions

- Stages 0-3 preserved the image; stage 4 was then taken into scope and changed it through the
  quality gates (2026-11-12). Byte-identity checks now use `stage4-20261112/reference/`.
- Keep deterministic guide capture (`HYBRID_EXACT_GUIDE`, 10 words per bin) on the current
  16x16 shader.
- Keep the hit-shading stream optional: the sampled preset fits below 8 GiB, but repeated gain
  qualification and physical 8 GB hardware validation remain open.
- The tested trace/shade split was rejected on combined pass time; reconsider only with a
  concrete change that reduces shading cost or registers.

## Matched quality follow-up (2026-11-12)

The fresh comparison is in [HybridTraceQuality.md](HybridTraceQuality.md), with exact setups,
arguments, input hashes and lossless images under
`E:/Dev/ZenGroundTruth/restir/matched-4k-20261112-v2/`.

| Metric | Quarter GI | Half GI |
|---|---:|---:|
| Profiled center, image relative RMS | 3.41% | 2.89% |
| Profiled center, image PSNR | 51.28 dB | 52.72 dB |
| Five-view pooled image relative RMS | 5.47% | 4.64% |
| Five-view pooled image PSNR | 51.89 dB | 53.32 dB |
| Eight-pose mean maximum-channel motion error /255 | 0.3769 | 0.2484 |
| One-pixel moving / static sparks per frame | 10.50 / 7.42 | 7.42 / 5.25 |

The image metrics use 130,560 stratified native 4K pixels against a high-sample, unfiltered
ZenApp reference sharing the engine's material shading; they are display-RGB metrics, not
independent path-tracer truth or the older raw diffuse-term RMS. Motion and sparks cover the
entire 4K image. Normal maps and the profiled fullscreen settings remain enabled. The older
normal-maps-off motion measurements are not directly comparable.

All five reference convergence checks pass at both candidate resolutions after extending C2/C3
to 512 frames. All shipped shader hashes remain equal to the timed shaders, and fresh quarter/
half frame-128 images match the retained release captures pixel for pixel. The new 64-sample
reference budget and buffered reference-module selection are diagnostic support; no candidate
sampling, filtering or material setting changed. The buffered module option now also accepts
explicit all-HW pixel references with environment lighting disabled. This follow-up does not
close the remaining occupancy, hit-stream performance or stage 4 gates above.
