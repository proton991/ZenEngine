# Transparency optimization execution — 8 November 2026

Execution record for [TransparencyOptimizationPlan.md](TransparencyOptimizationPlan.md).
The automatic-mode image checks pass and the CPU ordering spike is removed. The
**4 ms forward / 18 ms GPU-frame targets are not achieved**. Phase 0 attribution
is complete, with the precise hardware occupancy limiter left unproven. The pre-existing helper-shadow guard in
`forward_material.glsl` is part of the baseline.

## Execution status

| Phase | Outcome |
|---|---|
| 0 Attribution | Completed toggle, counter, CPU and GPU-metric experiments; the precise hardware occupancy limiter is unproven |
| 1 Current-path wins | Basic-material and compact-UV permutations plus ordering hysteresis retained; early-demote experiment rejected for no measured gain |
| 2 Core/fringe split | Automatic filtered-mip classification and runtime override implemented; this Sponza foliage stays blended in automatic mode |
| 3 Ordering/culling | Exact GPU sorting and conservative cluster frustum/Hi-Z culling retained; approximate cluster order rejected |
| 4 Shadows | Load-time cached opacity micromaps and same-node core shadow reuse retained; small-light cutoff rejected for negligible work reduction |
| 5 Lighting volume | Proposed grid fails the spatial image gate; volume storage and low-resolution foliage rendering are not retained |
| 6 Geometry scale | Deferred as requested |

The sections below retain the measurements and decisions from each stage. Final
performance, verification and remaining target gaps are at the end.

## Configuration and evidence

All new performance measurements use the real Release `ZenApp.exe`, the slow center
setup `build/editor-fps-20261107/center-sweep.zenrender.7.actual.zenrender`, hardware ray
queries, threaded RHI, GPU culling enabled, async compute disabled, no UI, fixed step,
and VSync disabled. Requested and reported render/framebuffer extents are 2360 × 1264,
windowed. No changes to `Data/engine.cfg` or authored assets are involved.

GPU Trace uses `tools/profile_hybrid_gi.py`, 45 seconds startup delay, 30 captured frames,
and unaltered clocks. Each capture retains the executable and shader hashes, exact
application/Nsight argument arrays, setup copy/hash, package hash, actual runtime state,
original `.ngfx-gputrace`, and original table exports. Shared model and environment
hashes are in [scene-inputs.json](../build/transparency-20261108/scene-inputs.json).
The `.ngfx-gputrace` files and raw GI captures were deleted in an 8 November build
cleanup; the table exports, summaries and hashes remain, and the deleted paths are listed
in `build/cleanup-20261108`.
RTSS was running; the launcher supplies its existing layer-disable environment overrides.
Closing RTSS from this session was attempted before the UV captures, but Windows
denied access to the process. This remains a limitation of the performance evidence.

The export lookup now discovers the directory containing `FRAME.xls`, including
`BASE_UNLOCKED`, and rejects missing or ambiguous exports. It was verified against the
historical export (34.68025 ms frame, 19.906 ms forward), a `BASE` fixture, and an ambiguous
two-directory fixture.

Temporary attribution changes are saved as patches under
[build/transparency-20261108](../build/transparency-20261108). They are deliberately incorrect
images used only for diagnosis and must not be retained as optimizations.

## Initial repeated measurements

| Configuration | Forward A, ms | Forward B, ms | Frame A, ms | Frame B, ms |
|---|---:|---:|---:|---:|
| Current baseline | 16.596 | 14.363 | 29.959 | 27.439 |
| E3: forward shadows disabled | 9.859 | 8.966 | 23.076 | 22.097 |
| E4: forward GI fallback disabled | 13.030 | 11.735 | 26.312 | 24.906 |
| E2: alpha test and constant colour | 0.292 | 0.290 | 13.145 | 13.200 |
| E1: depth-only replay | 0.241 | 0.241 | 13.332 | 13.070 |
| E5: leaf BLAS geometry marked opaque | 11.806 | 11.748 | 20.958 | 20.846 |

Each value is the median of 30 GPU Trace frames. These pass durations must not be
summed to infer frame time. The baseline spread is material: report ranges and repeat
the baseline around subsequent optimization measurements. GPC clock medians were
3026.03 and 3009.14 MHz, insufficient by themselves to explain the timing spread.
The shadow toggle demonstrates substantial forward shadow cost, but a precise additive
decomposition is premature.

E1 writes depth for the leaf cards without an alpha test and disables colour writes.
E2 retains alpha sampling/blending and substitutes constant RGB. Both are diagnostic
floors, not isolated additive terms: removing shading can eliminate unused varyings
and change register allocation, and E1 changes the depth history. The opaque-BLAS
probe targets the assembly material `trees/LeafSpring`, verified in the launch log as
834,740 canopy plus 519,830 ground-cover triangles. The unprefixed source-material name
would not match the runtime assembly.

E5 also reduces `ProbeTrace` from about 4.89 ms to 1.97 ms, `ProbeSpecularTrace` from
0.75 ms to 0.41 ms, and `SceneLighting` from 1.68–1.84 ms to 1.21 ms. This confirms
that foliage ray processing affects several passes. It does **not** predict an equal
gain from conservative opacity micromaps: falsely opaque cards terminate rays earlier,
as well as bypassing shader coverage tests.

The first baseline's `ForwardTranslucent` metrics were: SM throughput 31.60%, RT-core
activity 7.25%, pixel warp occupancy 19.24%, pixel register allocation 77.11%, L1TEX
sector hit rate 88.65%, L2 sector hit rate 95.23%, world/vertex-attribute throughput
0.68%, screen throughput 1.13%, CROP 0.30%, and ZROP 0.25%. These are the exported
metric definitions, not proof of a specific occupancy limiter. They point toward
shader cost rather than geometry or blending saturation.

The first occurrence row in `GPUTRACE_REGIMES.xls` contains aligned per-frame samples;
later occurrence rows repeat their individual sample across frame columns. Duplicate
metric columns are checked before collapsing them. Raw metric names and samples are
retained in each capture's `forward-metrics.json`.

## Plan corrections and pending work

- The G-buffer shader currently computes `SurfaceNormal` before its alpha rejection;
  the plan's claim that it already rejects before normal evaluation is stale.
- Using GLSL `demote` requires querying/enabling the corresponding Vulkan feature and
  a supported shader path. The tested experiment queried/enabled that optional
  feature and retained a separate ordinary SPIR-V module for unsupported devices. After demotion,
  `gl_HelperInvocation` is undefined; use `helperInvocationEXT()` in subsequent lighting
  guards ([GLSL extension contract](https://docs.vulkan.org/glslext/latest/glslext/ext/GLSL_EXT_demote_to_helper_invocation.html)).
- All E1–E5 experiments have two completed captures. An interrupted E2 repeat directory
  without a summary is excluded; the completed repeat is `e2-alpha-b-retry`.
- The user confirmed automatic split selection with a runtime override, micromap baking
  at load with a local cache, and deferral of Phase 6 LOD/impostor work.
- Performance, full-view image comparisons, fringe-mask/Mitsuba checks, blend-ordering
  tests and ray-query integration validation remain required for implementation changes.

## Attribution and CPU movement results

| Work | Observed or estimated cost, ms | Interpretation |
|---|---:|---|
| Geometry/raster floor | 0.241 | E1, both runs; stripped shading/varyings |
| Alpha sampling and blending above that floor | about 0.050 | E2 minus E1; different depth behavior |
| Remaining material/direct/environment work | about 6.00–6.05 | Residual estimate, not an independently timed component |
| Forward GI fallback | 2.63–3.57 | Paired baseline minus E4 |
| Forward shadows | 5.40–6.74 | Paired baseline minus E3 |

The residual is `E3 + E4 - baseline - E2`. This table is a diagnostic approximation;
shader compilation, occupancy, caching and interactions between toggles prevent an exact
additive attribution. The raw repeated pass measurements above are authoritative.

The standalone ZenApp motion run used 600 stationary frames followed by 600 frames
with `CameraInput.look = (0.01, 0)` each frame, matching the earlier scripted yaw.
After excluding startup frames 1–12, all 588 stationary samples reused the cache.
All 600 moving samples rebuilt it. Median `ForwardDrawCache::Update` CPU duration was
**0.0003 ms stationary / 28.4556 ms moving**; median sorted-index update duration was
**0.0004 / 0.62305 ms**. The moving sort mean was 28.5511 ms. Each index stream contains
16,352,520 bytes. Upload duration here is CPU staging/update work, not a GPU transfer
duration. GPU-bound frames can hide much of this CPU cost; it must not be equated to
the earlier approximately 6 ms editor frame-interval difference.

Evidence: [CPU summary](../build/transparency-20261108/cpu-summary.json),
[motion log](../build/transparency-20261108/motion.log),
[command](../build/transparency-20261108/motion-command.json),
[executable/shader hashes](../build/transparency-20261108/motion-inputs.json).

## Filtered coverage changes the priority

Diagnostic counting variants used explicit early fragment tests, so these counts are
**depth-passing non-helper fragments**, not all rasterizer fragments. Their storage
atomics and synchronization are excluded from performance measurements. The existing
forward ray-count variant was extended temporarily to count candidate coverage tests.
Counts were stable at readiness and through frame 32:

| Coverage | All BLEND fragments | Leaf material only | Leaf with forced mip-0 sampling |
|---|---:|---:|---:|
| Zero | 682,571 | 622,284 | 3,585,571 |
| Partial (`0 < alpha < 0.99`) | 5,811,237 | 4,819,052 | 257,517 |
| Core (`alpha >= 0.99`) | 39 | 39 | 1,598,287 |
| Total | 6,493,847 | 5,441,375 | 5,441,375 |

The leaf's filtered partial fraction is **88.5631%** and its core fraction is only
**0.000717%**. There are 1,577,780 filtered leaf fragments at or above alpha 0.5. The
source-image texel statistics do not describe the target view's filtered coverage.
Mip-0 sampling above is a diagnostic at the same screen locations, not an image change
to retain. The forward pass issues **5,730,437 hardware shadow queries** with
**2,783,506 non-opaque candidate tests**, or **0.48574 candidate tests per query**.
Opaque intersections do not invoke the shader coverage test.

Evidence: [coverage summary](../build/transparency-20261108/coverage-summary.json),
[counter log](../build/transparency-20261108/counts-mips.log),
[temporary instrumentation patch](../build/transparency-20261108/counters.patch).
The first counter/motion patch is retained separately as `counters-first.patch`.
Both instrumentation runs restored their starting sources and rebuilt ZenApp afterward.

Consequently, Phase 2's proposed 0.99 core is **not the largest expected gain in this
view**: almost nothing qualifies. Automatically selecting this leaf from base-level
texture statistics alone would be incorrect policy. Lowering the threshold or changing
alpha mips would change the image and needs its own parity/reference evidence.

The next priorities are Phase 1 shader specialization (register/material cost), early
coverage rejection and UV-output reduction; then sorting to remove the measured CPU
work. Cluster ordering needs particular care because the actual view is almost entirely
fringe, rather than the thin fringe assumed in Phase 3. Phase 2 remains gated by filtered
coverage. Ray work remains relevant, especially the millions of forward shadow queries
and foliage candidate handling in the probe passes. Keep all image-affecting choices
behind the plan's image comparisons.

## Phase 1 implementation measurements

The basic forward-PBR specialization disables absent extended lobes at pipeline
creation. The batch selector checks both the material feature flag and the actual
coefficients, since runtime edits can leave the authored feature flag unchanged.
It preserves sorted draw order and retains the general shader for extended materials.

| Configuration | Forward A, ms | Forward B, ms | Frame A, ms | Frame B, ms |
|---|---:|---:|---:|---:|
| Basic specialization | 8.184 | 7.114 | 21.280 | 19.985 |
| Basic plus early alpha demotion | 8.362 | 7.436 | 21.403 | 20.347 |
| Basic, early demotion, compact UVs | 8.322 | 7.361 | 21.372 | 20.430 |
| Retained basic + compact UVs, sort tolerance | 8.298 | 7.364 | 21.523 | 20.426 |

These use the same ZenApp configuration and retained provenance as Phase 0. The
specialization substantially reduces forward time, but does not meet either target.
The comparable first captures report pixel warp occupancy of 19.24% baseline and
32.30% specialized. The corresponding second captures contain implausibly near-zero
PC-sampler pixel occupancy despite similar rendering, so those samples are not used
to claim an occupancy change. Allocated register totals are not per-thread register
counts. A separate `VK_KHR_pipeline_executable_properties` probe of the actual
ZenApp forward pipelines reports **96 fragment registers for basic PBR versus 168
for general PBR**, with 48 vertex registers in both. Both select the UV2 shader and
report 29 fragment scalar inputs. The driver reports implausible local-memory sizes
above 64 GiB even with fully initialized query structures; those values are not used.
The probe, commands, executable/shader hashes, runtime state and logs are retained as
`pipeline-stats.patch` and `stats-{basic,general}*`; production sources were restored.

Early demotion moves coverage before normal/material evaluation and uses
`helperInvocationEXT()` afterward. It has not demonstrated a speedup in this view;
both repeated medians are slightly slower than specialization alone. A follow-up
which explicitly shared the alpha/base-color fetch also measured 8.307 ms forward,
without a demonstrated gain. Early demotion and its optional device capability/module
plumbing were therefore removed from the production change. The experiment is saved
in `early-and-uv-experiment.patch`; the feature can be reconsidered for a workload with
more fully empty quads. SPIR-V inspection confirmed
the ordinary module has no demote capability and the optional module has the demote
and helper-query instructions.

Validation so far: Release ZenApp and both integration targets build; 12 targeted
blend/cache/G-buffer tests pass, including lit and unlit basic-versus-general pixel
comparisons. The ray-query suite reports 92 passing tests, four skipped tests,
12 disabled tests, and zero failures/errors. No memory leaks were reported. All 11
full-view baseline comparisons match byte for byte: eight sweep views, a repeated
center view, the outside view and the decal-main view. Maximum and mean absolute
8-bit RGB differences are zero over every complete 2360 × 1264 image; see
[parity.json](../build/transparency-20261108/early-images/parity.json).

UV packing specializes both shader interfaces to one `vec4` for scenes with two UV
sets, reducing material varyings from 59 to 19 scalar components. Texture bindings
select their UV set on the fragment side. Scenes with additional sets retain the
general per-binding interface; no authored UV data is truncated. Forward, direct
G-buffer, indirect G-buffer and receiver variants share this specialization.
The new textured regression compares the compact path with the general path using
both UV1 and UV2, and covers metallic/roughness and specular/glossiness workflows.
Release builds, standalone SPIR-V validation, the four textured regression variants,
12 existing blend/cache/culling variants and the ray-query suite pass. The retained
basic-plus-UV change, after removing early demotion, also matches all 11 baseline
images byte for byte: [parity.json](../build/transparency-20261108/phase1-images/parity.json).

The sorting stopgap now reuses order for translation/roll and for rotations below
0.05 degrees. The rotation anchor changes only when a sort happens, so many small
rotations cannot accumulate unlimited ordering lag. Scene/material revisions still
invalidate the cache immediately. In the same 600-frame stationary / 600-frame scripted
yaw protocol, moving rebuilds fell from 600 to 27. Mean sort CPU time fell from
28.551 ms to 1.278 ms; moving median was 0.0001 ms and mean upload CPU time 0.0282 ms.
The remaining rebuilds still take roughly 28 ms, so the movement-spike goal is not
complete. See [motion-comparison.json](../build/transparency-20261108/motion-comparison.json).
The temporary timing patch and executable/setup/shader fingerprints are retained in
`motion-phase1*`; the sources and ZenApp were restored after the run.

## Phase 4 implementation in progress

The device advertises both EXT and KHR opacity micromaps. The initial implementation
uses [VK_KHR_opacity_micromap](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_opacity_micromap.html),
which represents micromaps as acceleration structures and shares their build commands
and barriers. Capability selection checks the extension and its complete feature chain.
The RHI/render graph retain micromap inputs and dependencies, including nested
TLAS → BLAS → micromap reads. The four-state hardware traversal regression passes in
all four execution modes, including unknown-state callback fallback and copied nested
input arrays. The full ray-query suite then passed 96 tests, skipped four, and reported
no failures or memory leaks (12 tests remain disabled).

A load-time input probe found just **20 distinct ordered UV/vertex-alpha patterns**
in each of the two leaf primitives (834,740 and 519,830 triangles). Exact key equality,
not hash equality alone, was used. The patterns can share micromaps through per-triangle
indices. The conservative GPU baker is being connected to the existing LOD-zero alpha
bounds code used by distance fields. Scene baking, persistent cache, shader variants
and performance validation remain open.

The conservative baker now passes textured and untextured coverage comparisons at
subdivision levels 1, 2, 4 and 6, with linear/nearest filtering, repeat/mirrored/clamp
addressing, transformed UVs and specular/glossiness alpha. The micromap shader variants
are selected across the hardware ray-query passes by the enabled capability. The full
suite with those variants reports 100 passing tests, four skipped and 12 disabled;
the 24 selected raster/cache tests also pass.

Scene integration now records GPU baking and micromap/BLAS/TLAS construction in the
render graph, retains immutable dependencies, and caches packed states after GPU
completion. A checksum and fingerprints of coverage inputs, sampler, texture content,
baker binary and device/driver reject stale or corrupt cache data. Unsupported inputs
retain the ordinary candidate test. The scene snapshot regression now exercises the
production micromap path through material edits, deformation, removal and reload; all
12 targeted scene/micromap cases pass, including cold bake and warm cache loading.

The first complete ZenApp validation run baked both leaf primitives at level 10 with
20 patterns and 5 MiB of packed states each, with no validation errors or reported
leaks. Their identical pattern payload uses the same persistent cache entry. Across
the 20 uniformly sampled barycentric micromaps, 61.495% of states are transparent,
34.699% opaque and 3.806% unknown. These are **baked state fractions**, not runtime
candidate or pixel fractions.

All 11 full-view image comparisons match the Phase 1 outputs byte for byte (which
themselves match the baseline): [parity.json](../build/transparency-20261108/micromap-images/parity.json).
The repeated standalone ZenApp captures retain the same setup, arguments, clock
policy and provenance as the earlier measurements:

| Configuration | Forward A/B, ms | Frame A/B, ms | ProbeTrace A/B, ms |
|---|---:|---:|---:|
| Phase 1 | 8.298 / 7.364 | 21.523 / 20.426 | 4.837 / 4.846 |
| With micromaps | 7.560 / 6.917 | 18.241 / 17.607 | 3.131 / 3.161 |

`SceneLighting` falls from 1.812/1.727 ms to 1.566/1.461 ms and
`ProbeSpecularTrace` from 0.750/0.754 ms to 0.550/0.554 ms. The separate counter run
reports the identical 5,730,437 forward shadow rays, with candidate callbacks reduced
from 2,783,506 to **158,253 (94.315%)**. Raster coverage counts remain identical.
Counter instrumentation was removed and ZenApp rebuilt after the run; evidence is
retained as `micromap-counts*` and `micromap-counters.patch`.

The forward pass remains above the 4 ms target, and the first frame median is slightly
above 18 ms. The remaining phases are still needed; these results do not complete the goal.

After reviewing dependency enumeration, the complete ray suite again passes 100 tests
(four skipped, 12 disabled) and the selected raster suite passes 20. The scene
regression covers changing vertex alpha, discarding an unsubmitted bake, restoring
coverage and cache reuse. A deliberately corrupted cache payload is rejected and
recreated byte for byte; see `micromap-cache-recovery.json`. No leaks are reported.

## Opaque core and fringe implementation

ZenAssetProcessor commit `a42624fec8e8a02cc355563863abd5a88f5ce71f` was tested,
committed and pushed from its standalone repository on
`codex/transparency-alpha-statistics`, then fetched into the engine submodule.
Its opt-in `collectAlphaStatistics` / `--texture-alpha-statistics on` records
exact zero/one texels and homogeneous zero/one bilinear footprints per logical mip.
Texture v5 and Metadata v9 retain the optional data; older packages remain readable.
The analysis preserves pixel storage and authored alpha modes. Reader, cooking,
texture-processing and scene-package suites pass, including two-mip round-tripping.

The engine carries the metadata into runtime textures, with reader-based load-time
analysis for older packages. `rendering.transparency` accepts `coreMode` (0 automatic,
1 disabled, 2 force) and `coreCutoff` (default 0.99). The optional setup object preserves
old setup compatibility, and the editor exposes the same controls.

Automatic selection is deliberately conservative: unit material/vertex alpha, a basic
deferred-supported material, and less than 2% partial coverage among nonzero footprints
at **every accessible mip**, rather than an estimate from camera distance. Unknown
runtime-generated mips remain blended. Trilinear chains require fully opaque levels,
because per-level histograms alone cannot establish cross-level correlation. This is
stricter than the plan's view-dependent mip selection and avoids making a material
eligible from mostly transparent source texels. Force bypasses the statistics gate,
but retains shader-support restrictions. No authored material is changed.

The selected core uses masked direct/indirect G-buffer and hybrid-receiver draws.
The forward fringe rejects coverage at or above the cutoff, tests LESS and keeps
depth writes disabled. Shadow-map core coverage is opaque; the partial fringe keeps
the existing stochastic shadow policy. Renderer-owned material-buffer metadata carries
the cutoff, and committed material/vertex changes refresh eligibility and draw caches.
Scenes whose opaque materials require the general forward path keep the split disabled.

The initial 16 raster/cache cases pass in all four execution modes. The new split
fixture includes zero, partial and opaque texture coverage; direct and GPU-culled
core draws match its original blend output within one 8-bit step. Eligibility updates,
force/disable switching and the unchanged BLEND mode are checked. Six runtime setup
tests pass. Full-view, fringe-mask, forced-split performance and reference checks are
still in progress; Phase 2 is not yet marked complete.

All eleven automatic-mode images are byte-identical to the micromap results:
`core-auto-images/parity.json`. Forced splitting changes only 72 center-view pixels,
with maximum channel difference 52/255 and whole-frame mean 0.000101/255; the outside
view remains identical. This localized difference still needs reference examination.
The additional mip/receiver regressions pass eight cases: stored and load-time alpha
statistics, nearest versus bilinear footprints, partial lower mips, alpha factors,
and candidate-cache rebuilding when the split is disabled.

The fresh repeated center-view traces do not support forcing the leaf split:

| Configuration | Forward A/B, ms | Frame A/B, ms | OffScreen A/B, ms |
|---|---:|---:|---:|
| Automatic | 6.629 / 6.872 | 18.861 / 17.769 | 3.171 / 3.215 |
| Forced core | 6.847 / 6.808 | 20.037 / 19.510 | 3.591 / 3.593 |

The capture order was automatic A, forced A, forced B, automatic B, with the same
ZenApp configuration and capture protocol. ProbeTrace varied (4.462/3.223 ms automatic,
4.480/4.475 ms forced), so these are not an additive frame-cost decomposition. The
forward pass gains nothing measurable, while the G-buffer pays approximately 0.4 ms.
This agrees with the Phase 0 measurement of only 39 depth-passing opaque-core leaf
fragments. The plan's expected large foliage gain is not present in this view. The
automatic policy remains enabled, and these leaves retain ordinary blending.

The temporary coverage-mask captures are complete and the production source/build
is restored. All eleven instrumented images exactly match automatic mode. No forced
split difference occurs outside depth-passing nonzero translucent coverage. In the
center view, 37 of 39 core pixels change (mean 7.188/255, maximum 52/255), while 35
fringe-only pixels change (maximum 5/255, mean 0.00001245/255 over that region).
See `core-mask-images/masked-parity.json` for every view. The masks include decals as
well as leaves, so the 6,493,847 total fragments differ from the leaf-only Phase 0 count.

The existing decal support path already exported and reused deferred shadow visibility
for nearby, roughly coplanar blended layers. Split materials now require the receiver
to belong to the same node; their draw-cache entries preserve node identity accordingly.
Ordinary decals retain their existing merging and support rules.

The same-node restriction applies only to exported shadow visibility; diffuse GI
keeps the existing receiver-support rule. The split fixture also checks that equal
node transforms retain separate identities while the split is active. Eight split
and equivalent-node raster cases pass across all four execution modes.

## Cluster-ordering image gate

A temporary 64-triangle prototype spatially ordered each large BLEND primitive by
Morton key and chose one of six axis-aligned interior orders for the view. It retained
the existing submission path to isolate ordering error before implementing GPU
scheduling. The prototype was removed and the production executable rebuilt.

The center view changes 88,307 pixels (2.96% of the framebuffer), including 13,188
pixels differing by more than 4/255. Maximum channel difference is 63/255 and whole
frame mean is 0.04708/255. Four sweep views, the outside view and the decal-only view
remain exact; other foliage views also change. See `cluster64-images/parity.json`
and `cluster-prototype.patch` for the complete experiment.

This approximation is rejected as the default. The assumption that only a narrow
fringe remains after Phase 2 is false for these filtered leaves. Spatial clusters
can still supply conservative culling bounds, but the GPU path must preserve the
existing per-triangle ordering for this content. Phase 3 remains in progress.

## Mitsuba check at forced-core receivers

The accepted compact capture is `core-reference-v3`: all 39 selected coordinates
are stamped into the GPU output and verified before tracing. Two earlier capture
attempts instrumented the pixel-gather shader while this setup used screen probes;
their output is rejected. The final probe capture source and executable were restored
after readback. These are receiver diagnostics, not performance captures or replacement
full-frame parity results.

Mitsuba 3.9.1 `cuda_ad_rgb` independently loads all five source glTFs, applies the
assembly normalization and traces 32,365,549 triangles with 24 analytic lights.
At each receiver it estimates one-bounce diffuse transport with 8,192 samples, source
base-color/metallic textures, vertex colors, emissive factors and alpha-tested triangle
visibility. Environment lighting is disabled by this setup. The new optional material
inputs in `ground_truth_mitsuba.py` preserve its ordinary loader behavior. Its installed
mesh-attribute API accepts host sequences, so those attributes now use NumPy arrays
instead of scalar-by-scalar CUDA readback. Alpha continuation, primitive identity and
vertex-color interpolation gates pass on CUDA.

| Quantity, linear irradiance / pi | Reference | Deferred probes | Forward voxel fallback |
|---|---:|---:|---:|
| Mean over 39 receivers and RGB | 0.007454 | 0.002207 | 0.015707 |
| Signed mean error | — | -0.005248 | +0.008252 |
| RMS error | — | 0.006400 | 0.026728 |

The independent half-estimate noise RMS is 0.000726. The probe estimate is closer
in this diagnostic; the voxel fallback has a large yellow contribution from the
emissive candle geometry. Neither estimate is unbiased. Captured receiver normals
are shared inputs, secondary normal maps and multiple bounces are excluded, and
source texture samples can differ from the engine's compressed textures. This is
evidence about the localized GI-source difference, not a full-material path-tracer
parity claim. Inputs, code hashes and per-receiver arrays are in
`core-reference-v3/reference.json` and `reference.npz`.

## Insignificant-light cutoff experiment

A temporary forward-only cutoff skipped shadow evaluation when every channel of a
light's unshadowed contribution was at most 0.1% of the layer's ambient contribution.
All eleven views differ by at most one 8-bit step; the center changes 790 pixels
with whole-frame mean 0.00008861/255. However, forward shadow rays only decrease
from 5,730,437 to 5,720,872 (0.167%), and candidate callbacks from 158,253 to 158,242.
Coverage counts remain identical. This does not address the remaining multi-millisecond
cost, so the approximation is not retained. No GPU-time saving is claimed from the
counter result. Source/build restoration and complete evidence are in
`light-cutoff*`, including `light-cutoff-images/parity.json`.


## Exact GPU ordering and conservative cluster culling

The cluster-order approximation is replaced by stable per-triangle GPU ordering.
A 4-bit radix sort preserves equal-depth scene order, with keys generated using the
CPU path's pairwise float arithmetic. Camera movement uploads node depth rows rather
than the 16.35 MB index stream. Submitted sorted keys are retained across camera
translation, roll and rotations below the same 0.05-degree threshold; a declined
frame does not publish its keys. Unsupported devices, advanced materials, mixed
core depth policies and more than 2,097,152 triangles retain the CPU path.

Each committed scene also groups Morton-ordered triangles into 64-triangle world
bounds. GPU frustum and max-depth tests select clusters, then a stable prefix scan
compacts their triangles without changing depth order. One indexed indirect draw
reads node/material identities and source vertices from the triangle records. The
max-depth pyramid preserves final odd rows and columns, background holes, near-plane
crossings and conservative depth/clip margins. GPU sorting/culling uses the existing
`--gpu-culling` override.

All eleven standalone ZenApp images in `gpu-cull-images` exactly match automatic
mode before Phase 3. The validated unculled GPU center capture is also exact.
Twenty integration cases pass across inline/threaded and async-off/auto modes:
stable unsigned keys and ties through the two-million-key limit, CPU/GPU blend pixels,
packed UVs, opaque-core raster parity, and conservative culling fixtures with 17 x 13
depth, edge background holes, invalid bounds and near-plane crossings. The shared frustum predicate and final bounds clamp bring the targeted regression
run to 40 passing cases (`gpu-final-tests.xml`). Performance results follow below.

The completed same-node forced-core sweep is `core-reuse-force-images`. Relative to
automatic mode its center changes 3,399 pixels (maximum 63/255, mean 0.004959/255);
the outside view remains exact. This includes the deliberate refusal to inherit
another node's shadow visibility, so it is distinct from the earlier forced-core
measurement before that restriction. The automatic Sponza policy still rejects the
split for these filtered leaves.


## Phase 3 timing

Repeated stationary GPU Trace captures (`gpu-sort-a`, `gpu-sort-b`) report
ForwardTranslucent **6.73875 / 6.78588 ms**, total GPU frame **19.26995 / 19.34760 ms**.
ProbeTrace is 4.45616 / 4.46085 ms and OffScreen 3.16768 / 3.19994 ms. The closest
preceding automatic-mode capture with similar probe work measured forward 6.62928 ms,
frame 18.86115 ms and ProbeTrace 4.46212 ms. Culling and compaction add about 0.174 ms
on the graphics queue (prefix 0.109, Hi-Z 0.042, emission 0.020, cull/scan 0.002).
There is no measured stationary forward shading gain from Phase 3.

The same scripted yaw (`look.x = 0.01`, 600 stationary then 600 moving frames) records
27 reorders. Their CPU preparation median is **0.0407 ms**, maximum **0.0723 ms**,
versus the earlier approximately 28 ms rebuilds. Average preparation over all moving
frames is 0.002614 ms; the stationary warmed median is 0.0008 ms with no reorder.
Each reorder uploads **2,224 bytes** of node depth rows, 60,048 bytes over the moving
interval, instead of rebuilding/uploading 16,352,520 bytes per reorder. These are
CPU preparation timings, not GPU sort duration or whole-frame latency.
`motion-gpu-analysis.json` retains the samples' aggregation; the source patch,
executable/shader/setup hashes, exact arguments and reported state are alongside it.
The temporary timing/yaw instrumentation was removed and ZenApp rebuilt.

## Phase 5 spatial lighting-volume gate

Before implementing volume storage, a temporary shader evaluates the proposed
1/8-screen x 64-log-depth grid's eight trilinear neighbors directly. Both shadow
visibility and diffuse GI are evaluated at the cell centers. It retains each actual
fragment's normal and material response, isolating spatial discretization before
adding an angular irradiance encoding or cache scheduling. It is an image-quality
probe, **not an optimized volume implementation or a performance measurement**.

The center view changes **572,989 pixels** (19.21% of the framebuffer): maximum
104/255, mean 0.48905/255, with 133,437 pixels differing by more than 4/255. The
magnified difference image shows foliage lighting changes throughout the canopy.
This fails the quality gate before storage/scheduling work can be justified for this
scene. The probe is removed and surface lighting remains in production. Evidence is
`volume-spatial-gate`, including the exact patch, input identity and parity report.
Low-resolution translucency is also unsuitable for this foliage-edge workload, as
specified by the plan; particle-specific volume/low-resolution modes remain future
content work. Phase 6 remains deferred by the user's decision.

The 4 ms forward and 18 ms total-frame targets are **not met** in the latest repeated
capture. The retained optimizations preserve the automatic-mode images and remove
the measured CPU ordering spike; they do not turn the extensively filtered leaf
fringe into an opaque deferred surface. Rescaling BLEND alpha mipmaps to force that
classification would change this filtered coverage, so no such asset conversion is
retained.


## Final verification

Release builds succeed for ZenApp, ConeVoxelGIIntegrationTest, RayQueryIntegrationTest,
RuntimeLaunchTest and ZenEditorUI. The targeted ordering/culling/raster suite has 40
passing cases. The final ray-query run has 100 passing cases, 4 capability skips and
12 disabled cases; all 17 launch tests pass. Formatting passes for 110 changed C++/
GLSL files, and the exception-policy check passes over 560 owned C++ files.
The final validated standalone ZenApp center capture (`gpu-final-smoke`) is pixel-exact
against automatic mode, with no reported validation error. No temporary lighting,
counting, timing or yaw probe remains in production source after its recorded restore.

A GPU Trace of the original scripted yaw has median frame 19.47815 ms, p95 19.9548 ms
and maximum 20.5253 ms over 30 frames. The conditional sorting ranges are absent from
Nsight's exported label table, so per-pass attribution for that changing command
layout is marked unverified in its summary. It does not establish a GPU sort duration.


A second diagnostic alternates between two views about 0.057 degrees apart, forcing
an order rebuild on every captured frame and keeping the pass layout consistent.
All 30 frames contain eight histogram, scan and scatter ranges. Their median summed
GPU durations are 0.11403, 0.01101 and 0.35914 ms respectively; key generation is
0.05608 ms. Thus radix work takes about **0.484 ms**, plus **0.056 ms** for depth keys.
The forced-reorder frame median/p95/maximum is 20.5079/21.1397/21.3973 ms. This is a
stress configuration, not the original slow-yaw frame distribution. The uninstrumented
ZenApp was restored afterward; patch and complete identity are in `gpu-sort-stress*`.

## Review fix

Review found that BLAS builds declared each micromap's per-triangle index buffer as a
transfer source. The render graph then ordered its staging upload for transfer reads,
while Vulkan synchronizes every device-address build input, including that buffer, at the
acceleration-structure build stage with `VK_ACCESS_SHADER_READ_BIT`
([vkCmdBuildAccelerationStructuresKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBuildAccelerationStructuresKHR.html)).
It is now declared as an acceleration-structure build input, like the vertex and index
buffers. RTSS was closed for these checks. Release ZenApp and both integration targets
rebuilt; the ray-query suite passes 100 cases (4 capability skips, 12 disabled), the
targeted ordering/culling/raster run passes 44 cases and all 17 launch tests pass, with
no validation errors or leaks. A validated ZenApp center capture
(`review-fix-smoke`) is byte-identical to `gpu-final-smoke`. Synchronization validation,
which would report the original access mismatch, was not enabled.

## GI-only lighting-volume gate

The Phase 5 spatial gate moved both shadow visibility and diffuse GI into the proposed
grid, so it could not show which one failed. A second temporary probe moves only the
forward voxel-GI fallback of BLEND layers to the same 1/8-screen x 64-log-depth grid.
Like the first probe, it evaluates the eight trilinear cell centers directly, using each
fragment's own normal. Shadow visibility stays per fragment. It is an image-quality
probe, not a volume implementation or a performance measurement. RTSS was closed, no
other session was using the checkout, and the production shader and ZenApp were
restored and rebuilt afterward. Evidence, including the exact patch and runtime state,
is in `volume-gi-gate` (script `prototype_volume_gi.py`).

| Center view against automatic mode | Shadows + GI in volume | GI only in volume |
|---|---:|---:|
| Changed pixels | 572,989 (19.21%) | 471,196 (15.80%) |
| Pixels over 1/255 | 314,191 | 197,652 |
| Pixels over 4/255 | 133,437 | 59,486 |
| Maximum / mean, 8-bit | 104 / 0.489 | 26 / 0.202 |
| p99, 8-bit | 10 | 5 |

The GI-only difference is smooth and spread over the whole canopy. The sharp speckles
of the combined probe were shadow-visibility errors. It still changes 4.5 times as many
pixels by more than 4/255 as the rejected 64-triangle cluster ordering (13,188), so it
does not pass the image gate used so far. The other ten views were not captured,
because the center view already decides that.

The comparison is against the current voxel fallback, which is itself biased: at the 39
core receivers its RMS error against Mitsuba was about four times the probe result's.
This gate therefore shows that the volume changes the image, not that it is less
accurate. The cost case is also weaker than expected. At this resolution the grid has
295 x 158 x 64 = 2,982,880 cells, about one per screen pixel, against 5.44 M leaf
fragments in this view. A dense volume with six cones per cell would remove at most
about half of the forward cone work. A larger saving needs sparse cells (only those
covered by blended fragments) or updates spread over several frames, both of which add
further approximation.

### Mitsuba check of the GI-only volume

The gate above measured change, not error. This check scores both estimates against an
independent reference at the same leaf points.

A temporary ZenApp capture forced the opaque core at a 0.5 cutoff, so the visible leaf
layer becomes a G-buffer receiver with a known position and shading normal. At every 8th
pixel the capture shader evaluated, from the same voxel radiance, the forward fallback
(`DiffuseVoxelLighting` at the point) and the GI-only volume (the eight cell centers of
the grid above, with the point's own normal). It also recorded the probe reconstruction
for context. The shader rebuilds cell positions with the hybrid inverse view-projection,
which places points 0.002759 units along -z from their stored positions at every pixel
(maximum deviation 4e-6). It corrects each cell by that constant shift. Its view depth
matches the true depth to 0.0004%. A Mitsuba 3.9.1 `cuda_ad_rgb` ray cast onto each
point's own triangle identified 11,097 of the 46,610 points as `LeafSpring`, and 2,000 of
those were drawn with a fixed seed. Mitsuba then traced one-bounce diffuse transport at
those points with 4,096 samples each, using the reference model of `core-reference-v3`.
Sources and ZenApp were restored afterward. Evidence is in `volume-gi-reference`
(`capture_volume_reference.py`, `reference_volume.py`).

| Linear irradiance / pi, 2,000 leaf points | Mean | Signed bias | RMS error | Relative RMS |
|---|---:|---:|---:|---:|
| Mitsuba reference | 0.00497 | — | — | — |
| Forward voxel fallback (current) | 0.01349 | +0.00852 | 0.02663 | 5.35 |
| GI-only volume | 0.01242 | +0.00744 | 0.02181 | 4.38 |
| Deferred probes (context only) | 0.00313 | -0.00184 | 0.00430 | 0.87 |

Reference noise RMS is 0.00065. The volume is closer to the reference than the fallback
at 58.3% of points, and its RMS error is 18% lower. The image change measured by the gate
is therefore not a loss of accuracy. Both voxel estimates, however, are about 2.5 times
too bright on leaves, with errors five times the reference mean. The probe result, which
traces hardware rays, has one-fifth of their error. The 39-receiver check found the same
fallback bias (+0.00825).

So the volume's spatial discretization is not what limits translucent GI quality here;
the voxel cone source is. If a translucency GI volume is built, filling its cells with
hardware rays as the probes do, rather than voxel cones, would address cost and accuracy
together.

Limits: only the front-most leaf layer with filtered alpha of at least 0.5 is sampled;
deeper canopy layers are not. The probe evaluates each cell with the point's own normal,
which overstates the angular resolution of a stored volume. The reference has one bounce,
source textures and captured shading normals.
