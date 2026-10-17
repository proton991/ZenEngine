# Nsight optimization implementation

Started 2026-10-15. Closed 2026-10-17 with P8: the retained candidates are in the final build, which passes the static and HW RT motion gates; the remaining reruns stopped by user decision are listed in the [P8 close-out](P8Closeout.md). Originally: **in progress; no candidate is qualified yet.** This work implements and evaluates every candidate in the Compute and HW RT lists in [P8.md](P8.md#compute-optimization-candidates). The P0 quality limits remain unchanged. Baseline revision: `5e074acb7a6728f0ced316ed317941a8c8b99ab4`.

## Candidate ledger

Overlapping candidates share one implementation. A rejected experiment must retain its implementation and measured reason in the evidence archive; an unimplemented candidate is not complete.

| P8 candidate | Implementation choice | Status |
| --- | --- | --- |
| Compute 1: packed sky search | Use the packed indirect queue on both providers | Implemented; preliminary timings and 28-case comparison complete |
| Compute 2: nearest-first traversal | Short-stack BVH2, preserving triangle and alpha acceptance | Three slower variants rejected; original layout restored |
| Compute 3: traversal divergence | Separate internal-node traversal from leaf intersection | Slower in the isolated hall comparison; original loop restored |
| Compute 4 / HW 6: guide-learning barriers | Separate 1 KiB power scratch removes one initialization barrier; a separate guide-finalization experiment targets the post-trace barrier | Initialization change implemented; separate-pass shaders prepared/validated, runtime evaluation pending |
| Compute 5 / HW 5: trace occupancy | Shared guide rows, recomputed rows, and an all-HW shader variant | All three rejected: shared rows regress; recomputation and specialization have no consistent speedup |
| Compute 6: ray budget | Four rays only after a full configured history and low relative variance on unchanged receivers, outside the bounce responsive window, with exit hysteresis; otherwise retain the existing exploration policy | Revised 2026-10-16 (review fixes below); requalification required |
| Compute 7 / HW 1: filter variance | Compute initial variance once per pixel; preserve early-history center variance | Implemented; preliminary timings and 28-case comparison complete |
| Compute 7 / HW 2: filter bandwidth | Store roughness in the unused fourth position-history component | Static and motion/settling gates passed; remaining qualification pending |
| Compute 7 / HW 3: filter tiles | Cache small-step overlapping filter taps in shared memory | Steps 1/2 retained provisionally; step 4 rejected by isolated pass timing |
| HW 4: sky-search cost | Cache the geometry-only denominator for unchanged receiver images; evaluate current lighting every frame | Implemented; initial 28-case regression passed |
| HW 7: history formats | Remove redundant geometric-normal channels, reconstructing encoded normals at FP16 precision from receiver history | Static and motion/settling gates passed; remaining qualification pending |
| HW 8: reduced-resolution tracing | First prototype masked lanes inside the full-resolution dispatch. Reimplemented 2026-10-16 as a quarter-size Compute dispatch with an upsample and a compacted fallback list (below) | Opt-in (`voxel_gi_diffuse_resolution=half`): 1.7-1.8x faster Compute at 1080p, but fails the frozen P0 limits on 5 of 10 shipping views; full resolution stays the default |
| HW 9: adaptive HW rays | Same settled-history rule with a stricter HW variance threshold | Static screening passed; full qualification underway |
| Compute: Lumen-style near/far trace | Exact triangles for a short near field, then the 64³ voxel occupancy grid; also the voxel grid as empty-space skipping with triangle confirmation (below) | Rejected 2026-10-17: the voxel far field is 27-39% too dark against the triangle references, and the exact near field saves only 1.4-1.6x |

## Evidence and acceptance

Local generated evidence is under `build/p8-nsight-optimizations/`. `baseline-archive/` preserves the starting executable, SPIR-V modules and configuration. Baseline and candidate measurements use the same frozen top/hall Sponza scenes, RTX 5080, Release/performance build, 1080p, 64³, validation/VSync disabled, threaded RHI and async compute. Initial comparisons use 120 warmup and 600 measured frames; qualifying comparisons must repeat static and moving-light workloads on both providers.

No benchmark may run concurrently with another GPU measurement or a configuration-mutating capture runner. Source-only work can proceed while a baseline runs; the active shaders and executable must stay fixed until it finishes.

Required final evidence: shader compilation and SPIR-V validation, relevant native rendering/history tests, triangle oracle for traversal changes, 22 fixtures and 20 shipping views at unchanged limits, motion/settling, mixed routing, bounce/reflection checks, and independent CUDA ground truth. Performance reports must distinguish full-frame measurements from overlapping pass intervals. Precision, sample-count and resolution changes are quality-sensitive and cannot be accepted solely because their timings improve.

## Implementation results

### Packed search and initial variance

`HybridFilterPrepare` computes the same initial variance from the stored histories once per pixel. It preserves storage rounding and the first filter's separate local variance for newly reset centers. Later filter iterations still propagate their own variance. Both providers use the packed indirect sky-search queue. Since the 2026-10-16 review fixes, the final filter produces the queue's elements and the search consumes only those; the support pass no longer clears the full per-pixel buffer, and the tiled shader is removed.

The first native run exposed a test shader registry missing the new preparation shader; the registry was updated. The rebuilt suite then passed 582 RenderCore tests (7 disabled) and 24 ConeVoxelGIIntegration tests. Hybrid shader modules pass SPIR-V validation. All 28 Compute fixture/Sponza comparisons against the archived software-triangle output pass the existing optimization tolerances (`packed-variance-quality/parity.json`). This is targeted regression evidence, not the full shipping/ground-truth sweep.

Initial 1080p/64³ measurements, 120 warmup + 600 measured frames, GPU medians in ms:

| Configuration | Compute top | Compute hall | HW RT top | HW RT hall |
| --- | ---: | ---: | ---: | ---: |
| Current-implementation baseline | 42.857 | 41.633 | 11.504 | 13.543 |
| Packed search + initial variance | 42.800 | 40.962 | 11.454 | 13.233 |
| Plus 64-entry near-first stack | 96.909 | 67.711 | 14.619 | 16.954 |
| Plus 8-entry stack and overflow escape walk | 56.439 | 51.504 | 11.673 | 13.808 |

Evidence directories: `baseline-1080`, `packed-variance-1080`, `near-first-1080`, `short-stack-1080`. Throughput and tails vary appreciably; these first pairs do not establish a qualified speedup. The baseline mean rates are 22.08/22.70 FPS Compute and 86.25/70.30 FPS HW RT (top/hall).

### Traversal experiments

Near-first traversal tests both children and visits the nearer intersected box first. Internal-node traversal and leaf intersection have separate loops. Triangle intersection, alpha acceptance and final hit reconstruction are unchanged. Both the 64-entry stack and the 8-entry stack with a stackless overflow walk pass the brute-force GPU oracle in all four execution modes: 1,625 decided rays per mode, zero mismatches. Both are slower than the starting traversal and are not accepted optimizations.

The measured shader hashes for both variants match all four timing manifests. The 64-entry variant is preserved as compiled SPIR-V in `near-first64-archive/`, with the shared executable in `packed-variance-archive/`; its original GLSL snapshot was not retained. The 8-entry variant has its source and compiled modules in `short-stack-archive/`. This source-archive limitation does not change the measured rejection, but the two variants must not be presented as interchangeable source snapshots.

The third candidate placed sibling roots next to each other, retaining the 32-byte node size and complete stackless overflow traversal. It also passed all four oracle modes, but the hall Compute median rose to 61.883 ms. It was rejected and archived in `adjacent-archive/`; the original CPU layout is active again.

Separating node and leaf loops without changing the original layout was also slower. In a paired hall ablation with the same shared-guide/filter changes, restoring the original traversal reduced Compute's GPU median from 98.601 to 77.973 ms. Neither loop organization nor near-first traversal is an accepted optimization on this GPU.

### Guide storage, barriers and shader footprint

The first shared batch combined separate traversal loops, shared guide rows, filter input tiles and roughness packing. Native checks passed (582 RenderCore, 24 ConeVoxelGIIntegration and 16 RayQueryIntegration tests), but its 1080p GPU medians were 89.770/98.601 ms Compute and 14.567/16.848 ms HW RT (top/hall). It was not retained as a batch; source, shaders and executable are preserved in `shared-archive/`.

Removing shared guide rows as well as the separate traversal loops reduced the hall HW median to 13.505 ms; Compute measured 50.647 ms with considerable variation. Shared rows therefore remain rejected. A smaller 1 KiB power-reduction scratch still permits the learning bins to initialize with the guide and removes one workgroup barrier. Its first HW pair is effectively unchanged (13.652/11.643 ms hall/top). This is a structural change, not a demonstrated speedup.

The initialization change alone does not evaluate the proposed separation of guide learning from traversal. A further experiment is prepared under `guide-finalize-experiment/`: four 8x8 trace groups share each original 16x16 guide tile, accumulate into the existing next-guide buffer with integer compare-and-swap, and let a separate pass average the guide. An initialization pass clears the accumulation buffer. Guide resolution, sampling policy and the power-summation order are preserved; learned-value accumulation remains non-associative. This removes the production trace's final workgroup barrier without allocating a full-resolution contribution buffer. All four trace variants and the two new pass shaders compile and pass SPIR-V validation. The experiment is isolated from the active qualification build; native tests, image checks and matched timings remain required before its disposition can be decided.

A temporary `VK_KHR_pipeline_executable_properties` probe obtained native compiler statistics. The measured private-row variants reported 126 registers per Compute thread and 128 for all-HW, with 11,024/18,192 bytes shared memory respectively. Recomputing guide rows reduced Compute to 117 registers, but did not establish an overall improvement. The driver's local-memory statistic returned an implausible 64 GiB-scale value even after zero-initializing the query structures; it is not interpreted as a byte footprint. Probe source and logs are archived under `stats-probe/` and `stats-*`; the instrumentation was removed and the renderer rebuilt before qualification. API requirements follow the [Vulkan executable-statistics reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPipelineExecutableStatisticsKHR.html).

The all-HW specialization removed software traversal and mixed-provider branches from the common all-hardware shader. Its initial hall/top medians, 13.642/11.639 ms, and subsequent comparisons did not establish a consistent speedup. It was rejected, its wrapper shaders and executable archived in `budget-screening/archive/`, and the original mixed-provider shader routing restored.

### Filter and history storage

The tile experiment staged positions, receiver identities/geometric normals, packed shading normals and variance for steps 1/2/4. Their footprints were 6,336/11,264/25,344 bytes; the driver reported 105/109/104 registers. Isolated pass timings below justify retaining steps 1/2 provisionally; step 4 and larger steps use direct texture reads. Surface rejection and filter summation order remain unchanged.

Position history keeps FP32 coordinates and stores roughness in its formerly unused fourth component. Normal history is reduced from RGBA16F to RG16F: geometric normals already exist in receiver history, so temporal reconstruction restores encoded normals at FP16 precision before applying its rejection tests. This removes 4 bytes per pixel per history side (15.82 MiB at 1080p), while lighting means and moments remain FP32. Vulkan leaves `packHalf2x16` rounding implementation-defined, so this does not establish bit identity with image-store conversion on every driver; compatibility is judged by the unchanged image/motion gates. See the [Vulkan precision rules](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html#spirvenv-precision). Both providers pass the static and motion/settling gates recorded below; remaining qualification is still pending.

The geometry-cache histories below consume that same 15.82 MiB, so these two changes have no net persistent-history saving. Initial variance adds one transient RGBA32F image (31.64 MiB at 1080p). In the `baseline-1080` versus `budget-mixed-600` measurements, peak VMA committed/device-local allocations increased from 2,697.12/2,601.12 MiB to 2,730.88/2,634.88 MiB on both providers. This batch trades about 33.76 MiB of committed memory for fewer repeated filter reads; it is not a total-memory reduction.

### Static sky-search geometry cache

The dense search now separates its denominator into geometry-only weights plus the current lighting-range correction. For an unchanged camera, geometry and surface revision, it reuses only the geometry term. Unsupported tiles then require no per-tap receiver fetches; lit taps, alpha and variance still use this frame's inputs. Motion, surface/geometry edits, resize, disabled filtering and failed submissions invalidate or reinitialize the cache. Two R32F histories cost 15.82 MiB at 1080p. No previous lighting result is reused.

All 28 Compute fixture/Sponza comparisons passed with the first geometry-cache/normal-history batch (`geometry-quality/parity.json`). Its manifest identifies the actual executable and shader hashes. A source-restoration timestamp error left recomputed guide rows and the temporary compiler-statistics instrumentation in that executable; those results are scoped to that intermediate batch. They are not qualification of the restored private-row version. The affected files were explicitly touched and rebuilt, and the absence of probe output was checked before the next run.

The rebuilt structural batch (`structural-current`, preserved in `structural-archive`) uses private rows, the one-barrier reduction, all-HW specialization, cached filter steps 1/2, packed roughness, reduced normal history and the sky geometry cache. Initial GPU medians are 39.637/42.783 ms Compute and 12.715/11.483 ms HW RT (hall/top). Compute tails remain variable and well above the target. In the isolated hall pass measurements, cached steps 1/2 reduced medians from 1.137/1.853 to 0.915/1.533 ms, while cached step 4 increased 1.777 to 2.227 ms; step 4 therefore uses direct reads again.

The temporary driver-statistics probe also measured the rejected shared-row/separate-traversal batch at 178 registers and 26,384 bytes shared memory per Compute group. Recomputed rows reached 117 registers but did not provide a consistent frame-time improvement. Both are archived, and the Vulkan probe changes have been removed and rebuilt.

### Reduced-resolution experiment

The prototype traces diffuse/bounce at one rotating receiver per planar 2x2 block only when identities and normals agree and positive previous sky means differ by no more than 25%. Edges, thin geometry, initially dark pixels, startup and reference accumulation retain full-resolution tracing; specular/reflection rays always remain full resolution. Shared output reconstruction preserves the selected receiver's sky, bounce and guide, and guide learning accounts for four represented receivers.

Both providers pass the Papermill hall/top composed-diffuse P0 gates against fresh 65,536-ray hardware triangle references (`half-screening/report.json`). This four-view screening is not complete shipping qualification. GPU medians are 50.061/54.949 ms Compute and 12.775/11.099 ms HW RT (hall/top), compared with 39.637/42.783 and 12.715/11.483 for the preceding structural batch. Compute regresses substantially; HW improves only in the top view. The added reconstruction/quality trade-off is not retained for that limited benefit. The implementation, executable and shaders remain in `half-screening/archive/`, with timings in `half-resolution/`.

### Settled-history ray budget

The budget experiment uses four diffuse rays only when a positive sky history has reached 32 frames (the configured history length since the 2026-10-16 review fixes, which also add a bounce guard and hysteresis) and its variance is at most 6.25% of squared mean for Compute, or 1% for HW RT. All other receivers retain the existing four/six-ray exploration policy. Reset frames and dark receivers with a positive guide retain six rays. Reference accumulation is unchanged. Both budgets retain the five-sample guided sequence stride, so changing budget cannot repeat an earlier frame's samples.

The first four Papermill screening cases passed composed-diffuse P0 limits (`budget-screening/report.json`). With the all-HW specialization removed, the 120-warmup/600-frame `budget-mixed-600` pair measured GPU medians of 38.140/40.871 ms Compute and 11.211/12.834 ms HW RT (top/hall); mean rates were 24.52/22.33 and 88.38/77.50 FPS. These preliminary numbers still leave Compute far below 60 FPS.

Review found that same-pixel moments could classify a newly exposed surface as settled during ordinary camera motion, before temporal reprojection rejected its history. The source now permits the reduced budget only when the camera, geometry and surface revision are unchanged. The same validity check controls the geometry-weight cache; moving frames retain the original exploration policy. The history regression test now exercises motion followed by a stationary frame, in addition to failed-frame recovery, resize, settings and scene resets. The guard built and passed the native tests and both-provider rendered motion/settling checks below.

### Qualification runner updates

`validate_hybrid_acceptance.py` now expects triangle visibility, ray-hit bounce and adaptive sampling on both providers. Its default converged reference uses 256 samples × 256 frames, preserving 65,536 rays while avoiding the previously observed Compute watchdog timeout. Native motion, settling and light-change captures use the same split, and settling can run on Compute. Bounce and reflection tools gate both triangle providers. The mixed-backend cache check now overrides only the sky-cache provider, preserving cone bounce for that isolation test; it validates both providers' reference provenance.

Reference validation checks the accumulated history length as well as the fixed sample budget, disabled filtering/adaptation and finite output. This also covers native companion captures, whose global frame index does not identify their accumulation window. Acceptance runs can reuse explicitly archived static references only after checking their executable and shader provenance. Reference-capture timeouts allow 30 minutes; motion/settling sequences allow 60 minutes.

The first HW reflection sweep failed its independent lobe and light-change gates (`reflection-final/hardware/reflection-results.json`). The longer reference window exposed FP16 rounding of the running specular history: with the same 65,536 indexed rays, maximum absolute escape-fraction error was 2.831% at 256 × 256 frames, versus 1.490% at 1,024 × 64 (`reflection-reference-probe/report.json`); the frozen limit is 2%. Reference mode now allocates FP32 specular history and selects a matching temporal shader; ordinary rendering keeps its FP16 formats. Entering/leaving reference mode recreates the changed format and resets history. The first implementation incorrectly used the cached shader's 16×16 dispatch sizing for the new 8×8 reference shader during its initial responsive window, leaving some pixels with only 248 accumulated frames. A temporary FP32 counter change did not fix that coverage error and was removed. Shader and dispatch selection now agree, and the history test checks reference dispatch dimensions as well as format transitions. The failed captures in the preceding directories remain preserved.

The corrected build passes all 665 enabled native tests (nine disabled), reference-shader SPIR-V validation and the dedicated HW reflection gates (`reference-precision-v3/report.json`, `reflection-fp32-v3/hardware/reflection-results.json`). Maximum absolute escape-fraction error is now 0.821% against the unchanged 2% limit. All five gated light changes and eight reflection-motion checks pass; worst light-change absolute bias/RMS/P99 are 0.512%/3.735%/17.665%, with no unexpected zeros. Below-cutoff smooth-surface noise and the emissive comparison retain their existing P6 diagnostic status. The executable SHA-256 is `14b700758ac02db14ceb576c17ac5fc2f2746fff0887f480ebbf34fb8ca8909d`; all 195 pre-existing shader modules are byte-identical to the preceding guarded build, with only the reference temporal module added. Fresh captures also pass all 22 static fixtures, 20 shipping views and 59 mixed-provider cases (`quality-fp32/report.json`, `mixed-fp32/report.json`). Worst composed-diffuse absolute bias/RMS/P99 are 0.278%/4.031%/16.651% for HW RT and 0.275%/4.167%/17.222% for Compute, with no unexpected zeros. Earlier motion and bounce evidence below retains its original `733b…` executable provenance; `remaining-qualification.json` explicitly records the inherited sky-motion evidence and reruns the other remaining stages on the corrected build.

Compute also passes the complete dedicated reflection gate (`reflection-fp32/voxel/reflection-results.json`): the independent lobe's maximum absolute error is 0.821%, all five light-change gates pass, and all eight motion checks pass. Its worst light-change absolute bias/RMS/P99 are 0.512%/3.735%/17.665%, with no unexpected zeros. For both providers, reflection-motion acceptance checks lag of at most one pixel and, for smooth surfaces, history of at most four frames. Moving-frame per-pixel image errors remain reported diagnostics and are not all within the static image limits.

The final measurement runner uses two rounds with reversed baseline/candidate order, identical configuration hashes, 120 warmup frames and 600 static or 1,200 moving-light frames. It also records actual issued ray counts at frames 64 and 720 for each provider and frozen view. Its original executable, shaders and configuration are restored after measurement. These are prepared checks, not completed results.

The pre-motion-guard candidate passed 582 RenderCore, 24 ConeVoxelGIIntegration, 16 RayQueryIntegration and 43 editor tests (nine disabled across the suites), plus hybrid SPIR-V validation. Logs and XML are `final-*.log` / `final-*.xml`. Its executable, sources and shaders are pinned in `final-quality/archive/`. Its 22 fixtures and 20 shipping views all pass against fresh 65,536-sample own-tier references (`final-quality/report.json`).

On 2026-10-16, the preceding guarded build also passed all 665 enabled native tests, hybrid SPIR-V validation, 22 fresh fixtures and 20 fresh shipping views (`guarded-*.xml` / `guarded-*.log`, `guarded-quality/report.json`). Its executable SHA-256 is `733b3a8afae0ed529a7db70f80e58578b95f9556d07d5152504f9d06fc12bc9b`. The shipping comparisons reuse the preceding static references with explicit provenance checks; the motion guard does not change the reference estimator. Across both required regions and all ten views per provider, worst composed-diffuse results for the guarded build are:

| Provider | Absolute bias | RMS | P99 | Unexpected zeros |
| --- | ---: | ---: | ---: | ---: |
| HW RT | 0.278% | 4.030% | 16.658% | 0 |
| Compute | 0.276% | 4.167% | 17.214% | 0 |
| Frozen limit | 2% | 8% | 20% | 0 |

All 59 mixed-provider cases pass on the guarded build: 28 static routing cases, 30 provider-switch cases and sky-cache isolation (`mixed-final/report.json`). The isolated Compute/HW cache means are equal (ratio 1.0).

HW RT passes the Papermill top/hall motion suite (`motion-final/hardware/report.json`): all 16 gated sky-settling cases and eight continuous-motion reprojection cases. Worst settling absolute bias/RMS/P99 are 0.253%/3.354%/12.314%, with no unexpected zeros. Maximum reprojection error is 0.002872 pixels against the 0.01-pixel limit, with zero receiver-identity mismatches. Continuous-motion image errors are reported diagnostics; the frozen shipping-image limits apply after settling.

Compute passes all 12 top/hall sky-settling gates and eight continuous-motion reprojection cases (`motion-final/voxel/report.json`, completed 2026-10-16). Worst settling absolute bias/RMS/P99 are 0.242%/3.353%/12.298%, with no unexpected zeros. Maximum reprojection error is 0.002872 pixels against the 0.01-pixel limit, with zero receiver-identity mismatches.

Both providers pass the dedicated bounce gates on the corrected build (`bounce-fp32/{hardware,voxel}/bounce-results.json`): two 128³ fixtures, ten gated light-change/continuous-light-motion cases, thin-wall leakage, independent CUDA bounce bias and two frozen Sponza composed-diffuse comparisons per provider. Both providers' worst gated light-change absolute bias/RMS/P99 round to 0.340%/3.687%/16.914%, with no unexpected zeros. Independent bounce bias is at most 4.775% against the 10% limit; unlit-side leakage is 0.00650% of the lit side against the 1% limit. Compute's Sponza composed-diffuse worst absolute bias/RMS/P99 are 0.178%/2.153%/7.578%, with no unexpected zeros. Sponza bounce-only errors still exceed shipping thresholds in some regions, including one unexpected zero in the hall; they remain reported diagnostics under the existing P5 composed-diffuse decision, not additional passed gates. The preceding guarded build's HW evidence remains preserved in `bounce-final/hardware/bounce-results.json`.

HW RT passes all 18 independent sky comparisons against the immutable Mitsuba CUDA dataset (`ground-truth-final/hardware/report.json`). Fresh 256 × 256 references have validated accumulation windows. Across the all-receiver and floor regions, maximum absolute bias is 0.105%, excess RMS is 1.788%, and 8×8-block excess RMS is 0.351%, against unchanged 1%/3%/3% limits. Six hall views use the existing block-based acceptance because the truth's per-pixel noise exceeds 3%. The comparison retains the existing matched-primary-surface scope: 1,101 top-view and 4,718 hall-view primary mismatches are reported separately, with no missing truth hits; the distance breakdown remains diagnostic.

Compute's independent CUDA sky sweep and matched performance results remain pending on the corrected build. The separate guide-learning experiment remains isolated and requires its own runtime screening after these runs finish. Before that experiment, an additional thin-wall light-change probe will enable the environment on both providers: the dedicated P5 fixtures disable it, so they do not exercise the new settled adaptive budget. The probe checks captured history for eligible unchanged receivers and applies the unchanged bounce and composed-diffuse limits to the existing five light-change cases.

### Review fixes, 2026-10-16

A review of the candidate source against the P8 lists found four issues, fixed together:

- **Bounce shares the diffuse rays.** A receiver is no longer settled while ray-hit bounce is in its responsive window (`hybrid.bounce.z`, eight frames after a lighting or radiance-cache change). Previously a light change could cut settled receivers to four rays exactly when bounce needed the samples. Reflections use the separate specular rays and need no guard. Under a continuously moving light the window never closes, so those workloads keep the full budget.
- **Budget hysteresis.** Four rays raise per-frame variance by 6/4. A receiver that used the reduced budget in the previous frame now leaves it only above 1.5× the entry threshold, so the reduction alone cannot switch it back and forth. A new `Budget` history (R8_UINT, one byte per pixel per side, 3.96 MiB at 1080p for both sides) records the previous frame's decision. It is full-size only while adaptive sampling is active and is read only for unchanged receivers.
- **Configured history length.** The settled test requires the configured history length (`voxel_gi_history_frames`, 32 by default) instead of a hard-coded 32, which never settled when that setting was smaller.
- **Tiled-search leftovers removed.** The unused tiled sky-search shader and its program registration are deleted. The support pass now resets only the four dispatch arguments. The final filter declares the pixel list as produced elements and the search consumes only those, so the render graph validates the queue without clearing the full per-pixel buffer.

Verification: the performance build compiles; all hybrid trace, filter, search, support and temporal SPIR-V modules validate. All 665 enabled native tests pass (582 RenderCore, 24 ConeVoxelGIIntegration, 16 RayQueryIntegration, 43 editor; nine disabled). Static Papermill Sponza at 960×540, with synchronization validation on both providers, reports only the existing presentation hazard. Issued sky rays per pixel keep the full budget at frame 8 (5.69 hall / 7.33 top on both providers). At frame 100 the reduced budget still engages: 5.66 / 5.85 Compute, 5.69 / 7.30 HW RT. Compute top stays at 5.83–5.85 from frame 64 to frame 720, with no budget oscillation (`build/p8-nsight-optimizations/budget-fix-smoke/`).

The bounce guard and hysteresis change which receivers use four rays, so they are quality-sensitive. The executable and the four trace modules changed, so the guarded and FP32 evidence above no longer describes the current build for the budget candidate. Its static, motion/settling, bounce, light-change and performance results must be rerun before it is accepted. The other changes in this batch leave rendering unchanged.

### Half-resolution Compute diffuse, 2026-10-16

The rejected prototype (above) chose one leader per 2x2 block but kept all 256 threads per tile, so three of four lanes idled inside each warp and nothing was saved. The reimplementation is selected with `voxel_gi_diffuse_resolution=half` (`auto` selects it only for the Compute trace; the default is `full`). It applies only with valid history, temporal accumulation, no reference accumulation and the software trace variant.

- **Trace (`hybrid_trace_half.comp`).** One 8x8 workgroup per 16x16 guide tile and one thread per 2x2 block. Each block's leader rotates every frame (diagonal first). It draws its own sample index (frame / 4), so each receiver traces a complete stratified sequence over four frames; a substitute leader uses that phase's seed. The leader's rays learn the guide for every receiver of its block. The pass writes every receiver's normal-dependent reconstruction guide and publishes its tile's pooled guide for the fallback pass.
- **Upsample (`hybrid_diffuse_upsample.comp`).** Each receiver averages the leaders of the surrounding 3x3 blocks on the same surface and plane (shading-normal dot of at least 0.9, power-32 weighted as in the spatial filter; geometric dot of at least 0.99). The previous sky must be similar: both dark, or both lit within 25% or three standard errors of the history means. Sky transfers as a ratio to each receiver's guide, as the spatial filter does; bounce is averaged directly. Receivers without a compatible leader are appended to one frame-wide list.
- **Fallback (`hybrid_trace_fallback.comp`).** One thread per listed receiver, dispatched indirectly, tracing at full resolution with its tile's published guide (no learning). A per-tile layout left mostly empty warps: about 3% fallback receivers cost 7.41 ms, 11% of a full trace.
- **Specular.** Leaders always trace their own. A receiver at least 0.3 rough whose roughness is within 15% of its leader's (and on the same surface) shares the leader's specular result; others trace their own, distributed over the workgroup from a shared job list so no thread traces a whole block alone.
- **Occupancy.** The full-resolution trace keeps the 384 guide-cell directions in shared memory. With a quarter as many threads per tile this limited occupancy to 9-11% (about 10.8 KiB per 64-thread group); the half and fallback variants recompute the directions with the same function instead.

Shaders validate; all 666 enabled native tests pass (`HybridHistoriesPublishOnlySuccessfulFramesAndResizePerView` opts into half resolution and checks the trace, upsample and indirect fallback passes; a settings test covers the new key). Static Sponza on both providers with synchronization validation reports only the existing presentation hazard.

**Performance.** Base-clock Nsight GPU Trace, 1920x1080, Compute, frame / trace passes in ms (`build/hybrid-gi-nsight/half/`):

| Step | Top frame | Top trace + fallback | Hall frame | Hall trace + fallback |
| --- | ---: | ---: | ---: | ---: |
| Full resolution | 71.73 | 64.50 | — | — |
| First quarter-size dispatch (per-tile fallback) | 92.44 | 48.60 + 36.03 | — | — |
| Recomputed guide directions (occupancy) | 57.65 | 28.35 + 20.28 | — | — |
| Relaxed leader rules, dark receivers share dark leaders | 44.68 | 28.50 + 7.41 | 41.75 | 26.73 + 4.60 |
| Frame-wide fallback list | 39.06 | 27.68 + 3.47 | — | — |
| Rough specular sharing, 15% roughness tolerance | 37.07 | 26.09 + 3.31 | 35.60 | 22.70 + 1.81 |
| Workgroup-distributed specular jobs | 34.52 | 22.83 + 3.50 | 31.48 | 19.91 + 1.84 |

At boost clock (120 warmup + 600 frames, `build/p8-nsight-optimizations/half-fps-1080-v7/`), Compute measured 39.90 / 40.59 FPS (GPU median 24.65 / 23.72 ms) top/hall, against 23.92 / 22.39 FPS at full resolution earlier the same day. HW RT is unchanged at 87.13 / 76.35 FPS. At 1080p the half path issues 1.64 / 1.48 sky and 0.65 / 0.45 specular rays per pixel, against 5.76 / 5.62 and 0.94 / 0.98 at full resolution.

**Quality.** Against the archived 65,536-ray Compute references (`build/p8-nsight-optimizations/half-quality/`, `half-ab/`), composed diffuse, all receivers (floor where it fails):

| View | Full resolution RMS / P99 | Half resolution RMS / P99 | Result |
| --- | ---: | ---: | --- |
| papermill-top | 1.63% / 5.39% | 2.77% / 9.63% | Pass |
| papermill-hall | 2.07% / 7.58% | 3.70% / 13.16% | Pass |
| kloppenheim-hall | 3.85% / 16.71% | 6.75% / 27.01% | Fail (P99) |
| qwantani-hall | 3.14% / 11.60% | 8.81% / 15.18% | Fail (RMS) |
| hotel-top floor | 1.86% / 7.64% | 8.23% / 34.23% | Fail (floor RMS and P99) |
| hotel-hall | 4.17% / 17.22% | 10.31% / 29.29% | Fail |
| carpentry-hall | 3.89% / 15.56% | 9.12% / 28.49% | Fail |

The half-resolution column uses the 25% history test without the three-standard-error allowance, which scored better in the hall (with it, papermill-hall measured 4.42% / 14.69%); the source keeps the allowance, which reduces hall fallback. A/B captures isolate the causes:
- **Normal-mapped surfaces.** A strict 0.9999 shading-normal test restores papermill-hall P99 to 11.7%, but sends 58% of top-view receivers to the fallback. The guide-ratio transfer recovers most of the difference at the 0.9 threshold.
- **Dim, noise-limited regions.** On the deep hotel floor the error is noise rather than blurred edges: shared leader estimates become spatially correlated noise that the spatial filter cannot remove, from a quarter of the rays.
- **No setting both passes and saves time.** Sending receivers whose history relative standard error exceeds 3% to the fallback brings all views but hotel-hall inside the limits (hotel-hall P99 22.39%). That variant costs 70.55 / 72.37 ms base-clock frames, no faster than full resolution, because most hall receivers fall back.

Full-resolution Compute is already within 3 points of the 20% P99 limit in the dim hall views, so a four-fold ray reduction in noise-limited regions cannot meet the frozen limits on these scenes. The half path therefore remains opt-in, and the default stays full resolution. With the default, all 22 fixtures and 20 shipping views pass (`default-quality-after-half/report.json`). Worst composed-diffuse all-receiver results are bias 0.218%, RMS 4.180% / P99 17.213% Compute and RMS 4.036% / P99 16.684% HW RT, within 0.031 points of RMS of the FP32 build.

### Lumen-style near/far Compute trace, 2026-10-17

The experiment tested whether Compute can follow Unreal Lumen's software path: precise geometry for a short near field, a coarse volume beyond it. The prototype replaced only the Compute ray in `TraceFeature`; the trace, sampling, guide and filters were unchanged. Variants, selected by glslang defines on `hybrid_trace.comp`:

- **A*n*.** Software triangle query up to *n* voxels (0.47 m each in Sponza at 64³), then a DDA through the voxel albedo occupancy (the legacy voxel-provider march) from that distance. A far-field hit uses the existing radiance-cache lookup by cell and entered face.
- **A*n*s.** As A*n*, but an occupied run at the start of the far field is not a hit. This is the start-cell skip that leaked at wall bases in the legacy provider; the near field covers part of it.
- **B*n*.** The grid only skips empty space: each run of occupied cells is tested against the triangles it spans, which keeps triangle visibility where the grid is conservative.

**Performance.** Base-clock Nsight GPU Trace, 1920x1080, Compute, top / hall, frame and `HybridTrace` medians in ms (`build/p8-nsight-optimizations/nearfar/screen1/`, `screen2/`):

| Variant | Frame | HybridTrace |
| --- | ---: | ---: |
| Baseline (full-length triangle rays) | 72.47 / 72.32 | 65.13 / 62.76 |
| A0 (voxels only) | 10.54 / 10.50 | 3.02 / 3.08 |
| A1 (0.47 m near field) | 36.12 / 32.70 | 28.12 / 22.24 |
| A2 | 44.89 / 41.21 | 37.42 / 28.85 |
| A4 | 54.24 / 47.96 | 46.87 / 38.50 |
| A4s | 55.55 / 51.59 | 48.19 / 42.38 |
| A8 | 61.85 / 56.43 | 54.51 / 46.44 |
| B0 | 236.00 / — | 228.68 / — |

The B0 hall trace did not complete within the Nsight session. Everything in the trace pass except triangle traversal costs about 3 ms. A triangle query only half a voxel long already costs 35-43% of a full-length one: most of the cost is the fixed descent from the BVH root to the receiver's surface, not the ray's length. B0 repeats that descent for every occupied run, so it is 3.5x slower than the baseline. The BVH partitions objects, not space, so a per-cell table of start nodes would miss triangles from neighbouring subtrees and is not a valid shortcut.

**Quality.** 960x540 Compute captures against the archived 65,536-ray triangle references (`build/p8-nsight-optimizations/nearfar/quality1/quality.json`), composed diffuse, signed bias:

| Variant | Top: all / floor | Hall: all / floor |
| --- | ---: | ---: |
| A4 | -2.99% / -36.98% | -38.91% / -37.23% |
| A4s | -1.36% / -31.75% | -30.28% / -31.53% |
| A8s | -1.19% / -29.95% | -27.38% / -29.93% |
| A1s | -3.04% / -33.57% | -33.71% / -32.35% |
| A0s | +1.19% / -33.08% | -28.91% / -31.08% |
| Frozen limit | ±2% | ±2% |

Every variant fails by an order of magnitude. Half-metre occupied cells thicken the arcades, roof edges and hanging banners, and block about a third of the sky reaching the floor and hall even behind an exact 3.7 m near field. Lumen avoids this with signed distance fields, which place surfaces within a cell. Binary occupancy cannot. A finer occupancy grid would shrink the thickening; it was not tested, but closing a 30% bias to the 2% limit would need far more than the 2-4x finer cells the presets allow.

The prototype source and harness are archived in `nearfar/archive/`; `hybrid_trace.glsl` is restored and all four trace modules recompile byte-identical to the shipped SPIR-V. A voxel far field would need an SDF or comparably exact surface representation, and the descent cost points to traversal itself (wider nodes, fewer dependent loads per ray) rather than ray length as the remaining Compute lever.
