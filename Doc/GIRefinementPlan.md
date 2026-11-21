# GI refinement plan after the RTXGI review (2026-11-14)

**Next direction (2026-11-15):** the user set the target to native 4K GI at 60 fps on the RTX 5080
at base clocks. Per-pixel tracing measures 93.2 ms there, so GI moves to a cached architecture: see
the [native 4K cached GI plan](NativeGIPlan.md).

This plan takes techniques from NVIDIA's RTXGI SDK 2.9.0 (`E:/Dev/RTXGI`) that address ZenEngine's
open GI problems: the fast-pan gate, lighting response and the HW tier's trace cost. It builds on
the [render pipeline hardening plan](RenderPipelineHardeningPlan.md) and keeps its gates and its
amended Phase 4 timing budget. Phase 0 instruments, Phase 1 opt-ins, and a first Phase 2 cache candidate
are implemented; none of the refinement options has been promoted to a default. Qualification and
remaining work are recorded in the [denoiser ledger](HybridGI/Denoiser.md#gi-refinement-2026-11-14)
and [cache ledger](HybridGI/RadianceCache.md). The user's decisions of
2026-11-14 are recorded under [Decisions](#decisions-2026-11-14).

**What was reviewed.** RTXGI 2.9.0 contains:
- SHaRC, a spatially hashed world-space radiance cache;
- NRC, a neural radiance cache;
- NRD 4.18 with the ReLAX, REBLUR and SIGMA denoisers;
- a path-tracer sample and the new IndirectDiffuse sample, a deferred renderer that uses SHaRC or
  NRC for the indirect diffuse term and NRD to denoise it.

**License.** All of it is under the NVIDIA RTX SDKs license. This plan re-implements published
techniques in ZenEngine's own GLSL and copies no SDK source into the engine. Linking an SDK, even in
an offline tool, was declined ([Decisions](#decisions-2026-11-14)).

## Starting point

Accepted RT baseline (`build/checks/rt-accepted-baseline-20261114`): New Sponza, 3840x2160, 1920x1080
GI, four diffuse samples per GI texel, ReSTIR GI, ReLAX and RT pipelines.

| Pass | ms | What it does |
|---|---:|---|
| `HybridTrace` | 8.66 | Each diffuse sample traces one GI ray, then shades the hit exactly with at most one more ray, to the resampled shadowed light; none when the light-occluder cache marks that light blocked. The baseline has environment lighting off, so its no-guide trace compiles out the hit's sky ray. With environment lighting on, a sky ray is added: up to three traces per sample |
| `ReSTIRTemporal` / `ReSTIRSpatial0` / `ReSTIRSpatial1` | 0.54 / 3.45 / 3.50 | Reuse visibility rays, with reordering |
| `ReceiverVisibility` | 4.66 | Exact per-light visibility at output resolution |
| Eight denoise/upsample passes | 6.95 | Budget 7 |
| Whole frame | 33.0 | Budget 33.018 |

Open problems this plan targets:

- **Fast pan.** 0.2837% of pixels exceed 4/255 at fast-pan pose 32; the gate requires 0.1994%.
  - Where it fails: the error sits in newly revealed receivers at the screen edge. Their histories
    are under eight frames (mostly two), their reprojection was accepted and they have ordinary
    reconstruction support.
  - The bright patches span about the 32-texel ReSTIR spatial radius.
  - Already tried, all plateauing at 0.28–0.30%: wider young preblur, a wider and longer repair,
    nearby mature support and unique boundary taps. Screen-space spatial methods alone have not
    closed this gate.
- **Lighting response.** It is driven by CPU response flags, which tighten the history clamp to one
  sigma while set. The retained result is 0.0807% at recovery frame 16, with higher low-level mean
  error. Changes that set no flag, such as moving occluders or animated emission, keep the
  four-sigma clamp.
- **HW trace cost.** The RT pipeline saves the pixel's accumulators on the RT stack at every trace
  (320 bytes in the primary raygen), so the number of traces is what costs. The replaced passes are
  4.9 ms slower than with ray queries.
- **One bounce.** Both tiers are single-bounce. Compute's voxel radiance cache is single-bounce, and
  `Doc/ComputeGI/ImplementationPlan.md` lists multi-bounce feedback as a follow-up.

## What RTXGI does that ZenEngine does not

| Area | RTXGI | ZenEngine now | Plan |
|---|---|---|---|
| Radiance at secondary hits | SHaRC: hits read a world-space hash cache, after a sparse update pass and a resolve pass. Exact shading runs only on a cache miss or a short segment | Exact shading at every hit: a light ray and a sky ray | [Phase 2](#phase-2-world-space-radiance-cache-for-hw-secondary-hits) |
| Disocclusion and lighting change | The world cache holds converged radiance wherever paths went, on screen or not | Screen-space history only | [Phase 3](#phase-3-cache-driven-fixes-for-motion-and-lighting) |
| Bounces | Cache back-propagation over up to four path vertices, plus cache resampling | One bounce | [Phase 4](#phase-4-multi-bounce-decided-after-phase-3) (decided after Phase 3) |
| History resampling | Bicubic (Catmull-Rom without corners) when all 12 taps are valid, bilinear fallback | Bilinear | [Phase 1](#phase-1-denoiser-refinements) |
| Anti-lag | Data-driven: accelerate where the clamp fired; reset part of the history when the deviation exceeds spatial plus temporal sigma | CPU response flags | Phase 1 |
| History confidence | Optional per-pixel confidence inputs relax accumulation and edge stopping | None | Phase 3, from cache gradients |
| History fix | Samples the accumulated signal; neighbours weighted by 1 + history length; stride grows for young history | Samples the current frame's preblur pyramid | Not planned: a mature-support variant was measured negligible (0.2832%) |
| Validation | Overlay showing history length, reprojection and blur state | Separate debug views and ad-hoc replays | [Phase 0](#phase-0-instruments) |
| Normal detail of diffuse GI | REBLUR SH: a luminance L1 SH resolved with the full-resolution normal | Bounce is denoised as RGB at GI resolution, so receiver normal-map detail finer than the filter footprint is lost; sky keeps it through a normal-dependent guide | [Phase 6](#phase-6-optional-normal-detail-in-bounce-lighting) (optional) |
| Light sampling at hits | RIS over 8 candidates | One resampled shadowed light plus the light-occluder cache | Keep ours |
| Shader execution reordering | On the multi-bounce path rays | Measured slower on the primary trace; kept for ReSTIR visibility | Measure on the cache update pass |
| Ray origin | Integer-ULP offset, with a float offset near the origin | Integer-ULP offset without that branch | Phase 0 check |

## Phases

The order is Phase 0, 1, 2, 3, then 4 if it is approved after Phase 3, then 5; Phase 6 is optional. Every change ships
behind a setting until it passes its gates. The hardening plan's Phase 5 (new defaults, deleting the
old chain) can proceed once Phases 1 and 3 close the pan gate.

### Phase 0: instruments

No image change. With every view off, static images stay byte-identical to the accepted build.

1. **Denoiser validation view.** For each GI texel, show and dump:
   - slow and fast history age per signal;
   - reprojection footprint (bicubic, bilinear or none) and rejection reason (identity, plane,
     normal, geometric, roughness, outside view);
   - how far the history clamp moved the slow history, in sigmas;
   - the repair source (own history, pyramid level, cache seed after Phase 3).

   `tools/gi_noise_probe.py` reports these per error block, so the corner-block analysis becomes
   routine rather than a one-off replay.
2. **Trace breakdown.** Extend the counts variant to report, per GI texel: GI rays, hit light rays,
   hit sky rays, reflection rays, and (after Phase 2) cache queries and cache hits.
3. **Ray-origin check.** Add a native test for self-intersection from surfaces near the world
   origin; New Sponza sits around it. `HybridTraceOrigin` lacks the near-origin float offset of the
   method it uses, and RTXGI 2.9 lists an origin-offset fix. Change it only if the test shows self
   hits, and gate the change on A/A.
4. **Multi-bounce reference.** Start a Mitsuba CUDA multi-bounce render early, for the same view and
   receivers as the one-bounce reference (`bounce/cuda/bounce-reference.npz`). Approved on
   2026-11-14 (decision 2): it measures how much light the single-bounce tiers miss, which is the
   data the Phase 4 decision needs, and it takes long to render.

### Phase 1: denoiser refinements

No cache needed. Each item is measured alone against an A/A pair, on the HW, Compute and editor
presets.

1. **Bicubic history.** Resample the slow and fast histories with 12-tap Catmull-Rom (corners cut)
   when all taps pass the existing identity, plane and normal tests, and bilinearly otherwise.
   Repeated bilinear resampling blurs history during motion; ReLAX and REBLUR both avoid it this
   way. The cost lands in `GIDenoiseTemporal` (0.76 ms now).
2. **Data-driven anti-lag.** Where the history clamp moves a signal's slow history by more than k
   times (fast-neighbourhood sigma + fast-history temporal sigma), shorten that signal's age by a
   fraction and accelerate it. This catches lighting changes that set no CPU flag. The flags stay as
   an extra hint. No new pass: it lives in the clamp in `GIDenoiseHistoryFix`.
3. **More samples for young receivers.** This item is ZenEngine's own, not from RTXGI. Receivers
   whose reprojected history is under two to four frames take extra diffuse samples in
   `HybridTrace`, for example twice as many.
   - The revealed band is contiguous, so warps stay uniform, and static frames pay nothing.
   - It targets the screen-edge band that no spatial method reached.
   - Budget: at most +0.5 ms on `HybridTrace` during the 1°/frame pan.
4. **Anti-firefly in temporal accumulation**, only if the spark gate regresses. Built on 2026-11-15
   in `GIDenoiseHistoryFix`, for texels under four frames old, after the moving-spark split traced
   the sparks there. It is on by default ([measurements](HybridGI/Denoiser.md#anti-firefly-2026-11-15)).

**Gates:**
- The hardening Phase 2 gates: pan error down at least 50% at both speeds, static flicker, static
  RMS and sparks.
- The eight denoise passes at or under 7 ms, and the static frame at or under 33.018 ms.
- Lighting response: recovery frame 16 not worse than 0.0807%, and mean error not worse than
  0.3973/255.

**Result (2026-11-14): no option qualifies.** The user then made all three defaults anyway
(decision 5).
- Bicubic history breaks the denoise and frame budgets for a 2–3% error gain.
- Anti-lag changes nothing measurable.
- Young receivers cut slow-pan and dolly error by 5–13%, but costs +2.86 ms of `HybridTrace`
  during the pan against a +0.5 ms budget.

The pose-8/32/64 pan gates turned out to be noise-limited: moving the same run by one frame changes
a single pose by up to 2x. See
[Phase 1 qualification](HybridGI/Denoiser.md#phase-1-qualification-2026-11-14).

### Phase 2: world-space radiance cache for HW secondary hits

Approved on 2026-11-14 (decision 1) as a HW-tier setting, off by default. It becomes the default
only if every gate below passes.

A hash-grid cache in ZenEngine's own GLSL, used where `HybridTrace` shades a secondary hit today.
The first version caches exactly what exact hit shading estimates now: direct and emitted light at
the hit, one bounce. The image is therefore comparable with the existing one-bounce reference before
any new lighting is added.

**Storage**
- Three buffers with the same entry count:
  - keys: 64-bit, or a compact 32-bit key where the device lacks 64-bit buffer atomics, chosen by
    capability;
  - per-frame accumulation: fixed-point RGB and a sample count, updated with 32-bit atomics;
  - resolved data: packed half RGB, sample count, and accumulated and stale frame counts.
- 40 bytes per entry: 40 MiB for 2^20 entries, 160 MiB for 2^22.
- Capacity comes from free VRAM and the scene bounds, not a scene-tuned constant. The HW preset
  already commits 9.6 GB at 4K, and the 8 GB run moved 930 MiB to host memory, so the cache must
  shrink first under pressure. Occupancy is reported.
- No minimum size (decision 3). When free VRAM leaves no room, the cache is not allocated, every hit
  is shaded exactly as today, and the chosen size (zero included) is logged.

**Key.** World position quantized at a level set by the log of the distance to the camera, plus a
3-bit normal direction, with a linear probe window of 8.

**`GICacheUpdate`** (raygen, sparse)
- One jittered GI texel per 4x4 block each frame: about 130,000 paths, sampled at different texels
  every frame.
- The path root is the G-buffer surface. Each hit is shaded with the existing exact hit lighting and
  added to its entry, demodulated by the hit's reflectance.
- At depth 1 that is at most about 0.4 million traces, about 1.6% of the trace's current maximum.
- The split of hit shading around its visibility rays (`hit_shading_split.py` in
  `build/checks/rt-opt/patches`) can be reused here.

**`GICacheResolve`** (compute, one thread per entry)
- Blend new samples into the resolved data, up to a frame cap derived from `historyFrames`.
- Evict entries left stale for N frames.
- Recover entries that moved within the probe window.
- While the camera moves, blend new entries from the adjacent level.
- When luminance has fallen for 32 frames in a row, reset to the current frame's samples.
- Response flags reset the affected entries' sample counts.

**Query.** In the primary trace's closest-hit, return the cached radiance times the hit's
reflectance, plus emission, and skip the light and sky rays, when all of these hold:
- the segment is longer than the cell diagonal;
- for reflection rays, the lobe's cone footprint at the hit exceeds the cell size;
- the hit is not within a few cells of a local light;
- the entry has samples.

Otherwise shade exactly, as today. The local-light rule matters in New Sponza: 91% of the floor's lamp
bounce comes from hits within 25 cm of a lamp, and Compute already re-traces such hits exactly for
the same reason.

**ReSTIR.** Initial candidates take their radiance from the query. Reservoirs and the reuse
visibility rays are unchanged.

**Debug views:** coloured hash cells, occupancy, query hit rate and traces per texel (Phase 0 item 2).

**Expected effect.** This is an upper bound, not a measurement. If most hits are answered by the
cache, traces per diffuse sample fall from up to three toward one. (Review correction: that holds
only with environment lighting on. The accepted baseline has it off, so a cache hit saves at most
the one shadow ray, and only when the light-occluder cache did not already skip it.) Shadow rays are cheaper than
closest-hit rays, so the time saved is less than two thirds. The cache also replaces the variance of
a single shadow and sky sample at each hit with a cell average, which should shrink the bright
outliers that ReSTIR spreads into patches.

**Tests**
- Native: key level and quantization, probe collisions, resolve arithmetic, stale eviction.
- The cache is cleared on scene reload and on history reset. RTXGI 2.9 fixed exactly this.
- With zero capacity the cache passes are not recorded and the image matches the cache-off build.
- Integration: on the calibration scene, cache-on converges to the same mean as exact shading within
  a stated tolerance, in all four submission modes, with validation on.
- Pass-list pins updated deliberately: `GICacheUpdate` and `GICacheResolve` join the HW list.

**Gates**
- Static RMS against the reference and sparks no worse beyond A/A noise.
- One-bounce bias against the CUDA reference (all receivers, floor, lamps) no worse than today's HW
  pixel result (−6.1% / −9.3%) beyond its error bars.
- Pan error no worse.
- Cost: `HybridTrace` plus `GICacheUpdate` plus `GICacheResolve` below today's `HybridTrace` on
  repeated warm launches. Otherwise the phase is rejected.
- The 8 GB run renders with no errors and reports the cache size it chose.

### Phase 3: cache-driven fixes for motion and lighting

Items 1 and 2 read the cache, so a device that allocates none (decision 3) keeps Phase 1's
behaviour. The gates are measured on the accepted-baseline machine.

1. **Seeding disocclusions.** A receiver with no history takes the cache's indirect estimate at its
   own surface as its starting slow history, weighted as a few frames, then accumulates normally. The
   bias decays as its own samples arrive.
   - The update pass must keep the root vertex's propagated (indirect) light apart from its local
     direct light, with sky and bounce kept separately to match the denoiser's signals.
   - That adds about 16–24 bytes per entry, or a separate root table.
   - Its layout is decided in this phase, against the memory budget.
2. **Lighting gradients as confidence.** For a stratified subset of texels, `ReSTIRTemporal` looks up
   the cache at the previous reservoir's hit point and compares it with the stored sample radiance.
   - Re-shading that point exactly would cost a light ray and a sky ray; the cache makes the check a
     memory read.
   - The relative change becomes a per-texel confidence that shortens the slow history, as ReLAX's
     confidence inputs do.
   - It covers changes no flag reports.
3. **ReSTIR for young receivers.** Only if patches remain after items 1 and 2: re-tune the spatial
   radius or the M cap for receivers without temporal history.

**Gates:**
- Fast pan at or under 0.1994% at pose 32, and at least a 50% reduction at both speeds and all poses.
- Lighting response as in Phase 1, plus a new moving-occluder sequence.
- Static flicker no worse.

### Phase 4: multi-bounce (decided after Phase 3)

Multi-bounce adds light, so it is a new lighting feature, which the hardening plan put out of scope.
Decision 2 (2026-11-14): deferred. Phase 0 renders the multi-bounce reference now. After Phase 3,
the single-bounce bias against it is reported and the user decides whether to build what follows.
Nothing below starts before that.

- **HW.** Allow back-propagation over two to four path vertices. Also allow cache resampling, so an
  update path stops at a populated entry and feeds its radiance back: bounded path cost, approaching
  infinite bounces.
- **Reference.** The Phase 0 multi-bounce CUDA render. Bias against it must improve on the
  single-bounce result against the same reference, per region (floor, lamps, sun). No new leaks at
  lantern openings or thin walls.
- **Compute (investigation only).** Populate the same cache from Compute's G-buffer roots, with field
  hits shaded from the voxel radiance cache. Report whether it could replace per-frame radiance
  injection: in dynamic scenes, re-voxelization costs about 33 ms a frame. The output is a report and
  a decision, not a change.
- **Exposure.** The image gets brighter, and ZenEngine has no exposure control. Tone mapping is left
  unchanged and its effect is reported.

### Phase 5: cost levers from lower per-sample variance

1. **Fewer diffuse samples.** Try two or three per GI texel instead of four. Every pixel trace saves
   its accumulators on the RT stack, so the trace scales with the sample count. If the denoiser needs
   it, add hit-distance reconstruction for texels that end with no valid hit, as NRD's probabilistic
   sampling mode does.
2. **Reordering on `GICacheUpdate`**, measured with and without; its multi-bounce rays are incoherent.
3. **Stack and recursion.** Re-measure RT stack sizes (`--pipeline-statistics`). If cache misses are
   rare, evaluate depth-1 recursion, with misses shaded by a follow-up pass over a compacted list.
4. **Freeze a new accepted baseline** once the phase passes, the same way as 2026-11-14: three or more
   RTSS-free warm launches, with uniformly slow launches re-run.

**Gates:** every quality gate holds; the frame and replaced-pass budgets go down, not up.

### Phase 6 (optional): normal detail in bounce lighting

Carry a luminance L1 SH (dominant direction) alongside RGB for bounce through the denoiser, and
resolve it with the output-resolution normal in `HybridReconstruct`, as REBLUR's SH mode does. This
restores receiver normal-map detail in bounce lighting that is finer than a GI texel and the filter
footprint. (Hit surfaces already skip normal maps since `c3cdaf05`; that is unrelated.) Gates: static RMS and the image
review improve; the eight passes stay at or under 7 ms or the user accepts a stated increase; memory
is reported.

## Not adopted

| Item | Reason |
|---|---|
| NRC | Needs Tensor Cores and NVIDIA's closed library, and is marked experimental. ZenEngine picks tiers by capability and stays vendor-neutral |
| DLSS Ray Reconstruction | Vendor-specific, and full-scene upscaling is out of scope |
| Linking NRD or SHaRC code | RTX SDK license and an HLSL/D3D-centred integration. ZenEngine's own denoiser already meets its cost budget. An offline NRD A/B harness was declined too ([decision 4](#decisions-2026-11-14)) |
| SIGMA with stochastic direct lighting | Changes direct lighting, which this plan does not touch. A separate plan if `ReceiverVisibility` (4.66 ms) becomes the priority |
| NRD-style history fix | The corner pixels' reprojection was accepted and they have support; a mature-support variant changed 0.2837% to 0.2832% |
| Slope-scaled disocclusion threshold | Plane tests use the geometric normal; the corner analysis shows accepted reprojection, not false rejection. Revisit if the validation view shows plane rejections at grazing angles |
| Auto-exposure, AgX/ACES | Not GI |

## Decisions (2026-11-14)

| # | Question | Decision | Effect on the plan |
|---|---|---|---|
| 1 | Phase 2's world-space radiance cache for HW secondary hits | Approved, as a HW-tier setting, off by default | It becomes the default only if every Phase 2 gate passes. Phase 3 items 1 and 2 build on it |
| 2 | Multi-bounce (Phase 4) | Render the reference now, decide the feature later | Phase 0 renders the multi-bounce CUDA reference. The feature is decided after Phase 3, from the measured single-bounce bias |
| 3 | Cache memory on an 8 GB card | Shrink to zero, with exact shading as the fallback | No minimum size. With no room, the cache is not allocated and hits are shaded exactly, as today. Phase 3's seeding and gradients are then off on that device |
| 4 | NRD's ReLAX as an offline A/B reference | Declined | No SDK code is linked anywhere. The denoiser is judged by the CUDA reference, the gates and the Phase 0 validation view |
| 5 | Phase 1 options after their qualification failed (decided after Phase 1) | Bicubic history, data-driven anti-lag and young receivers on by default; the ReLAX-style denoiser is the only GI denoiser and the old one is removed | The 7 ms denoise, 33.018 ms frame and +0.5 ms pan-trace budgets are exceeded by choice. See the [denoiser-only path](HybridGI/Denoiser.md#denoiser-only-path-2026-11-14) |

Still open: whether to build Phase 4, decided after Phase 3.

## Execution status (2026-11-14)

- Phase 0 instruments and the finite-depth CUDA reference are implemented. Native tests and
  validation-equivalent shader paths pass; a byte-identical 4K comparison to the old executable
  was not claimed from shader hashes alone.
- Phase 1 is finished, and no option qualifies.
  - Compute and editor were not run, because no option passed HW.
  - The user then turned bicubic history, anti-lag and young receivers on by default and removed the
    old denoiser (decision 5). The ReLAX-style passes are now the only GI denoiser.
  - Options-off moving sparks (30.9 px/frame) are above the 13.75 limit. That limit was measured with
    ray queries and the previous denoiser, before the RT pipeline and `relaxDenoiser` became the
    accepted HW path. The increase is not from Phase 1, and its cause is not isolated.
- Phase 2's first cache candidate is implemented and **rejected by its cost gate**. Final trace,
  update and resolve total 9.512 ms versus 8.324–8.339 ms for paired cache-off trace launches.
  The constrained 8 GiB fallback and native integration checks pass. See the
  [cache ledger](HybridGI/RadianceCache.md) for exact runs, candidate revisions and unrun gates.
- Review (2026-11-14): the rejection stands.
  - The cache cut RT-core work by 27%, but this pass is limited by SM-side work. The cache also
    lowered occupancy by 8%, and closest-hit lanes that miss still make their warp issue the
    shadow trace.
  - Two corrections. The 33.018 ms frame budget does not apply to these captures, which use a
    different camera. The plan's claim of three traces per sample holds only with environment
    lighting on.
  - The full test suites and a nine-configuration Vulkan validation matrix pass, covering ray
    queries, Compute and environment lighting on.
  - Details are in the [cache ledger review](HybridGI/RadianceCache.md#review-2026-11-14).
- The user requested stopping after the current phase. Work stops at the Phase 2 evaluation;
  Phase 3 and dependent Phase 5 work are not implemented. Phase 4 remains unapproved and Phase 6
  remains optional. No default or accepted baseline is changed.

## Risks

| Risk | Mitigation |
|---|---|
| Cached light leaks across shadow edges inside a cell | Normal in the key; exact shading near local lights and on short segments; bias gate per region |
| Collisions or eviction churn during fast motion | Capacity from the VRAM budget, occupancy view, stale-frame limit; pan gate |
| Cache lags lighting changes | Response flags reset entries; fade acceleration; Phase 3 gradients; light-path gate |
| No 64-bit buffer atomics | Compact 32-bit key, chosen by capability |
| VRAM pressure at 8 GB | Budget-derived capacity with no minimum, exact shading when nothing fits; 8 GB run in each phase |
| Correlation between cached radiance and ReSTIR reuse | Sparks and pan gates; Phase 3 item 3 |
| Per-launch driver compile variance hides small timing changes | Repeated warm launches, re-run uniformly slow launches, as in the accepted baseline |

## Measurement protocol

- Every run uses the canonical `ZenApp.exe` with `--setup`, and captures use
  `tools/profile_hybrid_gi.py`: warm launches at base clocks, with RTSS closed.
- Candidates are screened with `tools/gi_quick_gates.py` (since 2026-11-14), about 2.5 minutes per
  candidate:
  - One quality launch per candidate plays the static, pan, dolly, spark and light segments on one
    path and writes only the measured frames (`--capture-frames`).
  - Converged references are rendered once per preset and cached.
  - Each candidate gets one Nsight capture with a 20 s start delay, plus an optional capture during
    a 1°/frame pan.
  - The A/A comes from delaying the options-off run by one and two frames. Fixed-step repeats are
    bit-identical, so they are not an A/A.
  - Motion is scored by the median over eight poses.
  - HW runs first; Compute and editor run only for finalists.
- The multi-launch `tools/gi_noise_probe.py` suite (about 30 minutes per candidate and preset) and
  `tools/gi_noise_gates.py` are retained for archived comparisons only. The frame budget still
  comes from the accepted RT baseline.
- References come from the CUDA ground-truth tools.
- Shader experiments use private modules restored by hash, because `Data/SpvShaders` is shared with
  other sessions.
- Each phase records its runs and dispositions in `Doc/HybridGI/Denoiser.md`, or in a cache ledger
  for Phases 2–4.
