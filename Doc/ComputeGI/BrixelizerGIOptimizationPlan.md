# Brixelizer GI optimization plan

2026-10-28. This plan targets the performance items still open after the final-code profiling in [the profiling record](BrixelizerGIProfiling.md#final-code-nsight-gpu-trace) and [the execution record](BrixelizerGIExecution.md#open-items). It changes no quality gate of [the implementation plan](BrixelizerGIImplementationPlan.md). Every expected effect below is a hypothesis until a matched measurement confirms it.

## Targets and current gap

| Item | Target | Current measurement | Required change |
| --- | --- | --- | --- |
| Steady-state GI passes, still camera | Within 10% of the shadow-map baseline (plan, "Verification and performance targets") | Native 3.598 ms against 2.965 ms (+21%); Nsight base clocks 3.445 ms against 2.977 ms (+15.7%) | At least −0.34 ms native (to 3.26 ms); −0.17 ms under Nsight |
| Construction work per frame | At most 2 ms, worst dispatch recorded | Cold interval sum up to 3.246 ms; dynamic up to 4.954 ms, of which `FieldSnapshotGeometry` 4.167 ms | Remove the snapshot spike; bound heavy phases by measured time |
| Dynamic-geometry frame | Not a plan target; needed for usable dynamic scenes | 46.0 ms: `VoxelizationComp` 32.9 ms, 29 shadow cube faces 13.9 ms, `VoxelInjectRadiance` 2.2 ms; field passes at most 0.46 ms | Bring the frame near the still camera's cost plus the moved region's cost |
| Dynamic field latency | Not specified | About 3 s and 62 graphs per dynamic generation, roughly 65 frames at 22 FPS | Fewer frames per refresh within the 2 ms budget |
| Time until full Compute fields are ready | A few seconds; initial stalls accepted on 2026-10-29 | Release New Sponza: scene field 1.3–3.2 s; scene plus all 23 local fields 2.8–6.9 s from construction start, excluding asset load. Same-run configuration before this change: 12.0 / 20.4 s | Initial throughput path implemented; see Phase 2 for configuration, range and limits |

Where the GI trace time goes (Nsight, still camera; [compare.json](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-final/compare.json)):

- ProbeTrace costs +0.43 ms and ProbeSpecularTrace +0.27 ms against the bricks.
- The field traces issue 11–13 times the bricks' 3D texture requests. Each request depends on the previous step's distance, so the L1TEX long-scoreboard share rises to 35–39% and SM throughput falls to 42–49%. Memory throughput stays below 20%.
- SIMT efficiency is about 20% in both builds (16–18% for the bricks), so warps run about six of 32 lanes.
- Occupancy is register-limited in both builds: 30–38% of compute warps active, 65–92% of the register file allocated.
- In the steady state every probe traces one batch of 64 rays (`ProbeRayBatches`), one ray per lane. A lane-level work queue inside a probe therefore has nothing to rebalance on a still camera.

## Ground rules

- **Quality is fixed.** Sample counts, hit tolerance, field resolution and lamp-volume extents stay unchanged unless a change is reported as an explicit quality/cost tradeoff. Each kept change reruns:
  - the RenderCoreTest and RayQueryIntegrationTest suites, in Release, plus Debug for lifetime changes;
  - the four CUDA mean-bias gates (`bounce_field.py all|sun|lamps`) with their absolute-error distributions;
  - the fixture gate in `tools/validate_compute_ray_parity.py`, which has not yet run on the final code. Run it once before Phase 1 as the reference.
- **Matched measurement.** Every change gets three native repeats (`finish_workloads.py` still, camera, dynamic) and three Nsight repeats (`m7_profile.py <run> current`), against the same baseline replays. Nsight recompiles shaders on each launch, and identical SPIR-V has run 45% slower in a single repeat. Only medians of repeats count. Record each kept or rejected change in the profiling record, with links.
- **Shared checkout.** `Data/SpvShaders`, `Data/engine.cfg` and the GPU are shared by every session and agent. Only one may build shaders or run GPU work at a time.
- **Do not retry** the rejected experiments: the direct-scene special case, relaxed layout checks and transposed probe strata. Each measured no faster or slower.

## Phase 0 — measurements that set the order

None of these changes shipped behaviour. Temporary variants are built, measured and reverted.

1. **Steps per ray by feature.** Run a counting capture (`capture_hybrid_gi.py`; the metadata's `field_work` holds iterations, distance samples, unavailable rays and iteration limits per feature) on the matched still view. This gives steps per ray for sky, bounce, specular and near-light visibility, and the rate of budget retries. No current capture records this.
2. **Attribution runs.** One native and one Nsight run of each, with everything else matched:
   - (a) no lamp volumes (`BuildLocalFieldVolumes` returns none);
   - (b) near-light field visibility forced to visible in `HitLightVisibility`.

   Together they split ProbeTrace into the scene-field march, the composite-volume overhead and the nested visibility traces. The last measured volume cost (1.57 ms) predates the current trace code.
3. **Source-level hot spots.** Open [current-shaderprofile](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-final/current-shaderprofile) in the Nsight Graphics GUI (Shader Profiler, Source view). For `ProbeTraceFieldSP` and `ProbeSpecularTraceFieldSP`, record:
   - the top lines by samples and stall reason;
   - registers per thread, for each of the two register allocations seen across launches.

   The command line cannot export this.
4. **SceneLighting +0.22 ms.** Compare the pass's shader variant, bindings and counters between the builds (its L1 texture hit rate is 81% against 20%). Its shader differs from the baseline's only in preprocessor guards.
5. **Baseline coverage.** The milestone 0 executable has no `--profile-workload`. A matched baseline for camera motion needs that option backported to a build of the preserved source. Milestone 0 recorded no dynamic workload at all.

### Phase 0 results: New Sponza hall view, 2026-10-29

Temporary one-change shader variants at the approach path's hall end (PhysCamera001), Compute, 1600×900, native GPU frame median. The baseline is 6.00 ms, with ProbeTrace 1.58 ms ([attribute.py](E:/Dev/ZenEngine/build/compute-gi/approach/attribute.py); runs `attr-*`). Each variant was built, measured and reverted.

| Variant | GPU frame | ProbeTrace |
| --- | ---: | ---: |
| (a) No lamp volumes; their code is compiled out | 5.17 ms | 1.10 ms |
| (j) Lamp volumes skipped at run time; code kept | 5.40 ms | 1.21 ms |
| (e) Candidate-tree walk kept, no volume traced | 5.18 ms | 1.12 ms |
| (f) Rays still split at lamp boxes; every segment marches the scene field | 5.56 ms | 1.32 ms |
| (b) Near-light visibility at hits forced visible | 5.98 ms | 1.58 ms |
| (c) No hit shading | 5.95 ms | 1.52 ms |
| (d) a + b + c | 4.82 ms | 0.82 ms |
| (h) Candidate volumes scanned in one warp-uniform order | 6.04 ms | 1.59 ms |
| (i) Unsafe 1.6× brick steps | 5.99 ms | 1.55 ms |

- **Lamp volumes are the lever: about 0.83 ms.** About 0.23 ms is static: their code's registers and occupancy cost every ray, even when no lamp box is crossed. About 0.60 ms is run time: splitting rays at authority boxes, and marching the finer levels. The candidate-tree walk itself is free.
- **Not bottlenecks here:**
  - nested near-light visibility;
  - hit shading;
  - brick samples, so over-relaxation (step 2) and sub-brick skipping (step 3) cannot pay;
  - divergent uniform-buffer reads of volume bounds.
- **Tried and rejected; both reverted, do not retry:**
  - **Starting each cell lookup at the previous step's empty hierarchy level.** Identical images (max 1 level, inside the noise floor), but no speedup.
  - **A coarse scene-field pre-check that starts a lamp segment's fine march at the first allocated coarse cell.** ProbeTrace rose to 2.40 ms. The one-cell reference halo allocates most cells near walls, and the extra state cost more than it saved.
- **Next candidate:** a single-loop composite trace. Segments switch levels inside the cell loop, instead of each segment running its own march to completion. A warp then no longer waits at every segment boundary for its slowest lane, and the per-segment setup disappears. It targets the 0.60 ms run-time share and part of the static share. Compute captures for A/B checks need 800 frames: two runs of the same build then differ by at most 1 level.

## Phase 1 — GI trace cost (target −0.34 ms native)

Ordered by cost to try and by how directly each step addresses the measured bottleneck. Phase 0's results can reorder steps 3–6.

1. **Resume budget retries instead of restarting.** `FieldTraceRay` (`hybrid_provider.glsl`) retraces from the origin with four times the budget when the first attempt exhausts its 2,048 iterations, so the first attempt's work is lost. Instead, return the cursor at exhaustion and continue from it with the remaining allowance. The results are identical. The gain depends on the retry rate from Phase 0.1.
2. **Over-relaxed sphere tracing inside occupied bricks.** The brick loop in `DistanceFieldTraceLevel` steps by `sample − tolerance` and needs many small steps near grazing surfaces.
   - Change: step by ω·d (ω ≈ 1.6), and back off to the plain step when the previous and current spheres do not overlap (Keinert et al. 2014). Hits stay inside the same hit band.
   - Verify: compare the analytic trace tests and fixture hit distances before and after, then the gates.
   - This directly reduces the dependent 3D texture requests, which are the measured bottleneck.
3. **Sub-brick empty-space skipping.** Store a 64-bit mask per brick of the 2×2×2-interval sub-blocks whose minimum distance can reach the hit band. The emit phase writes it; for New Sponza's roughly 60,000 bricks that is under 0.5 MiB. The march then steps across masked-out sub-blocks without sampling.
   - This extends the existing whole-brick skip for bricks whose samples cannot meet the hit band.
4. **Split marching from hit shading.**
   - Change: ProbeTrace writes one hit record per ray. A new pass shades the hits with `HitRadiance`, then queues near-light visibility segments for a compact visibility pass, as the deleted brick retrace did; in the baseline that pass cost 0.013 ms. The temporal pass consumes the results.
   - Aim: each kernel holds fewer live registers, so more warps hide fetch latency, and the march no longer waits on divergent nested visibility traces.
   - Cost: about 0.5 M rays per frame at 16–32 bytes each, 8–16 MiB of transient traffic.
   - Contract: the "unavailable sample is excluded" rule must survive the split.
   - Apply the same split to ProbeSpecularTrace if the probe split pays off.
5. **Persistent ray fetching.** Once marching is ray-parallel (step 4), lanes whose ray ends take the next ray from a global queue instead of idling until the warp's longest ray ends. A small per-probe reduction then sums the per-ray results. This targets the 20% SIMT efficiency.
   - Earlier per-thread traversal-stack experiments on the RTX 5080 regressed. Accept this only on measured gain.
6. **Live-state reduction.** The march carries a `DistanceFieldLevel` (six vectors) through the loop. Instead, select the scene field or a local slot by a small index and read constants from the uniform when needed. Measure registers per thread in the GUI (Phase 0.3) before and after.
   - This is conditional: it stands alone only if steps 4–5 are not taken.
7. **Composite-volume overhead.** Conditional on Phase 0.2(a) showing large volume cost away from lamps. Every ray walks the volume node tree. Options:
   - cull candidate volumes once per probe, since all of a probe's rays share an origin;
   - test the union of the volumes before walking the nodes.

Stop when the matched native GI pass sum is at or below 3.26 ms with every gate passing. Then report any remaining steps as optional.

## Phase 2 — construction throughput and background budgets

**2026-10-29 requirement update:** the user explicitly accepts a few seconds of stalls or stutter to finish initial distance-field GI. The 2 ms target now applies to background edits; initial scene and lamp-volume construction prioritizes throughput. A preview does not satisfy full-field readiness.

**Implemented initial throughput path:** dense-cell reference windows grow from 1,024 to 8,192, the total reference buffer from 1M to 4M entries, and the triangle ceiling from 4,096 to 65,536. The existing GPU capacity checks still shrink and retry batches before allocation. The distance emitter continues processing 256-reference tiles, so the larger window changes batching rather than field resolution or distance arithmetic. Each work dispatch's advance is combined with preparation of the next dispatch, removing one serial scheduling pass per slice.

Cold dispatch limits use asynchronous GPU timestamp samples from the first heavy slice of each phase per graph. The controller targets 6.25 ms per sampled dispatch, limits adjustment to halving/doubling, and caps work by buffer capacity and the device's group-count limit. Up to 16 cycles advance per frame against a soft 100 ms frame-interval target. These are feedback targets, not hard maximums; pipeline creation and changing work density can still cause longer stalls. After initial scene/local completion, geometry edits return to 4,096-triangle / 1,024-reference windows, 256 cells and 4,096 jobs per slice, and the existing background frame controller. Declined graphs discard their unsubmitted timing samples.

Measured Release, RTX 5080, New Sponza, 1600×900, VSync off, threaded RHI, 128³ radiance cache and 64³ scene field:

| Measurement | Before | After, repeated runs |
| --- | ---: | ---: |
| Scene field construction | 12.0 s | 1.3–3.2 s |
| Scene plus all 23 local fields, from construction start | 20.4 s | 2.8–6.9 s |
| Scene construction cycles | 3,900 approximately | 480 approximately |
| Scene bricks / triangles / unavailable cells | 60,419 / 3,747,018 / 0 | unchanged |
| Scratch including geometry snapshot | 110,115,584 bytes | 133,020,416 bytes |

These times exclude the earlier asset load and include other renderer work interleaved with construction. They are elapsed times, not GPU-active time or proof of full GPU utilization. [Before log](E:/Dev/ZenEngine/build/compute-gi/cold-throughput/baseline128/run.log), [first adaptive run](E:/Dev/ZenEngine/build/compute-gi/cold-throughput/adaptive/run.log), [repeat 1](E:/Dev/ZenEngine/build/compute-gi/cold-throughput/repeat-1/run.log), [repeat 2](E:/Dev/ZenEngine/build/compute-gi/cold-throughput/repeat-2/run.log). The earlier measurements below document the incremental scheduler that this initial path supersedes.

Lighting qualification must distinguish construction from sampling. The ordinary 64-complete-frame lamps capture now ends at hybrid frame 175 rather than the earlier 1,644; `ProbeRayDirection` seeds rays with that hybrid frame. Its floor mean is −22.6%, outside the 20% gate. A diagnostic capture at the matching hybrid frame 1,644 gives lamps overall/floor −20.2%/−8.3%, within the 22%/20% gates. The ordinary all-light capture passes at −11.6%/−10.0%, and a longer capture at frame 1,636 (near the earlier 1,638) gives −11.2%/−3.6%, also within 15%/15%. Sun-only is unchanged at −4.8%/+6.8%. The longer all/lamps normalized absolute errors are 0.352/0.440 and 0.711/1.132 (overall/floor). This supports preserved field lighting with comparable sampling, but does not qualify the early lamps-only convergence or claim pixel identity. [Original short lamps capture](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/split/cold-throughput-lamps.bias.json), [matched lamps](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/split/cold-throughput-lamps-matched.bias.json), [longer all-light capture](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/split/cold-throughput-all-matched.bias.json).

Validation: Release RayQueryIntegrationTest 92/92, RenderCoreTest 728 active cases, and EditorRenderingTest 59/59 pass. Debug passes 16 field/lifetime cases and six editor startup/switch cases; the AMD Radeon integrated GPU passes 12 field cases including dense initial batching, declined submissions and alpha streaming. GPU suites report no validation errors. The new dense-cell regression checks the closest surface beyond 1,024 source references and a subsequent smaller-budget edit. Both editor configurations are rebuilt. [Validation artifacts](E:/Dev/ZenEngine/build/compute-gi/cold-throughput). A separate default 256³ radiance-cache run completes the scene field in 3.2 s and all 23 local fields in 6.9 s with zero unavailable cells or construction errors; [log](E:/Dev/ZenEngine/build/compute-gi/cold-throughput/default256/run.log).

0. **Preparation is bound by frame count, not GPU work.**
   - `DistanceFieldScene::RecordBuild` records the ten construction phases once per frame, in fixed order. The GPU scheduler (`distance_field_schedule.glsl`) runs only the current phase, so a frame finishes at most one triangle batch (quota at most 4,096). The next batch's clear and jobs phases precede emission in the recording order and must wait for the next frame.
   - Heavy phases are limited to `dispatchesPerFrame = 2`, alpha coverage streams back to the clear phase, capacity retries halve the quota, and the hierarchy builds one level per frame.
   - Result: New Sponza needs about 3,900 frames for the scene field (4.3 per minimum batch), and the lamp volumes follow one at a time.
   - The GPU-side cost per frame has a median of 0.11 ms and a p95 of 0.73 ms in the uninstrumented Release run (Codex's `finish-final/still-0` native profile), far below the 2 ms budget. Wall time is frames × frame time:
     - 5.7 ms frames in Release with VSync off (22.3 s);
     - 16.7 ms in the VSync-locked editor at 60 Hz (an estimated 65 s, not measured);
     - about 120 ms in Debug (472.8 s; [Debug log](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/prep-timing/debug/run.log)).

     In Debug, render-graph compile and execute alone take about 17 ms per frame, against 1.7 ms in Release.
   - Fix: record the phase cycle repeatedly within one frame, which is safe because a non-current phase dispatches zero groups. Choose the repetition count, and the heavy-phase dispatch count, from measured field timestamps against the 2 ms budget. This shortens Release, editor and Debug readiness alike. Build lamp volumes with the same per-frame time budget, overlapping the scene field's tail where the workspace allows.
   - **Implemented and measured 2026-10-28.**
     - **How it works.** `RecordBuild` repeats the cycle `NextCycleCount()` times per frame.
       - Each generation first runs eight frames at one cycle and takes the smoothed frame interval as its baseline.
       - It then adds a cycle per frame while the interval stays within the baseline plus an allowance, and halves the count when the interval exceeds it.
       - The allowance is the larger of `frameBudgetMicroseconds` and `frameBudgetShare` times the baseline. Compute GI uses 2,000 µs, a share of 1.0 and at most 16 cycles.
       - Each publication log line now reports its construction cycles.
       - The default settings, no budget and one cycle, keep the earlier behaviour for direct users and tests.
     - **Why the frame interval.** Two earlier controllers were measured and rejected:
       - Timing only the work dispatches with GPU timestamps measured about 2 ms at 16 cycles. The chain of about 42 barrier-separated dispatches per cycle actually made the frame 40 ms long (Nsight trace [build-trace](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-cycles/build-trace)): the work dispatches cover about 10 ms and idle gaps between them about 25 ms.
       - Allowing the chain three quarters of the current frame interval fed itself: the frame stayed at 38 ms.
     - **Results.** New Sponza, scene field and 23 lamp volumes; logs under [prep-timing](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/prep-timing):

       | Build | Scene field built | Frames | Frame during build | Lamp volumes published by |
       | --- | ---: | ---: | ---: | ---: |
       | Release, one cycle per frame (before) | 22.3 s | 3,920 | 5.7 ms | 45.5 s after launch |
       | Release, fixed 16 cycles | 9.7 s | 251 | 38.5 ms | 22.0 s |
       | Release, adaptive | 11.0–11.3 s | 990–1,013 | 11 ms | 21.6–21.8 s |
       | Debug, one cycle per frame (before) | 472.8 s | 3,923 | about 120 ms | 3 of 23 after 587 s |
       | Debug, fixed 16 cycles | 55.8 s | 253 | about 220 ms | 99.3 s |
       | Debug, adaptive | 68.3 s | 1,272 | about 53 ms | 114.4 s |

       - Every run builds the same field: 60,419 bricks, 90 retries, 3,747,018 triangles and no unavailable cells. The adaptive scene field ran 3,921–3,926 cycles, the same work as 3,920 one-cycle frames.
       - The VSync-on Release run also measured 11 ms frames, so VSync did not limit frames in this headless setup.
       - The 22:10 one-cycle Debug run probably shared the CPU with a scene-cache write, so its time per frame is not directly comparable. The adaptive run's one-cycle baseline was about 26 ms.
     - **Remaining cost.** Each cycle still records about 42 render-graph passes, most of which dispatch nothing because their phase is not current. Fewer passes per cycle would let each frame do more at the same frame cost: merge each advance with the next prepare, or use larger triangle batches.
1. **Partial geometry snapshot.** `RecordSnapshot` copies every source vertex and index (a 98 MB snapshot) for each generation, including a dynamic volume's refresh. Two ways to fix it:
   - keep the snapshot persistent and copy only the changed instances' ranges;
   - copy in byte-bounded chunks across frames.

   Target: the snapshot's worst frame below 1 ms.
2. **Time-budgeted heavy phases.** Heavy phases run a fixed `dispatchesPerFrame = 2` (`VoxelGIRenderer.cpp`).
   - Derive the count from the measured timestamps of earlier dispatches. Stay under 2 ms in the worst frame, and use the headroom when a frame has it. Dynamic refreshes now use at most 0.46 ms per frame.
   - This also shortens the 3 s dynamic-field latency.
3. **Service time versus overlap.** Use a construction-only trace with hardware events (at most about ten compute frames before overflow) to separate dispatch service time from concurrent rendering. Do this before tuning against the elapsed intervals in the native profiles.

## Phase 3 — dynamic-scene frame

The dominant costs predate the field. They are listed because they decide whether Compute GI is usable with moving geometry.

1. **Incremental radiance-cache voxelization (32.9 ms).**
   - Today: any geometry revision makes `VoxelizerBase::BeginVoxelization` clear every owner. `ComputeVoxelizer` then re-voxelizes all 3.75 M triangles in 58 dispatches.
   - Change: build dirty boxes from the changed instances' old and new bounds, as the field's stale boxes do, then:
     - clear owners and reflectance sums only inside those boxes;
     - voxelize only instances that overlap them, culled per instance on the CPU and tested per triangle in the shader.
   - Acceptance: voxel textures byte-identical to a full re-voxelization of the same geometry, verified by a new test.
2. **Static shadow-face cache (13.9 ms).**
   - Today: `SceneShadowRenderer` already re-renders only faces whose frustum a changed caster touches, but it draws the whole scene into each face.
   - Change: keep a static-caster depth copy for each face that a moving caster has touched. Each frame, copy it and draw only the moving casters.
   - Memory: 4 MiB per 1024² D32 face, about 116 MiB for the 29 faces here. Allocate on demand under a budget, and fall back to a full redraw when it is exceeded.
3. **Radiance injection (2.2 ms) and light-cache carry-over (0.86 ms).** Keep them full-volume for now. A moving occluder changes light visibility outside its dirty box, so restricting either needs its own correctness argument.

## Phase 4 — remaining evidence

- Complete Nsight transition traces: cold preparation, coverage expansion, warm switch and scene switch. Use `--max-duration-ms` below 10,000; `nsight_finish.py` currently passes 60,000–120,000, which Nsight rejects.
- An old Sponza GPU Trace.
- A camera-motion baseline (Phase 0.5).

## Expected order of work

| Step | Decided by | Risk to quality | Cost to implement |
| --- | --- | --- | --- |
| 0.1–0.4 measurements | — | none | low |
| 1.1 resume retries | retry rate | none | low |
| 1.2 over-relaxed stepping | steps per ray, Nsight texture requests | low (same hit band) | low |
| 1.3 sub-brick masks | steps per ray | low | medium |
| 2.1 partial snapshot | worst construction frame | none | medium |
| 2.2 time-budgeted phases | worst construction frame, dynamic latency | none | low |
| 1.4 march/shade split | attribution runs, register counts | medium (result contract) | high |
| 1.5 persistent fetching | SIMT efficiency after 1.4 | low | medium |
| 3.1 incremental voxelization | dynamic frame | medium (needs identity test) | high |
| 3.2 static shadow cache | dynamic frame | low | medium |
