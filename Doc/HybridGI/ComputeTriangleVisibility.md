# Compute triangle visibility — 2026-10-14

Compute now traces the full scene's triangles through a software BVH. This replaces voxel occupancy as the visibility geometry for environment lighting, specular occlusion, the sky cache, ray-hit diffuse bounce, reflections, and direct shadows. Hardware RT uses the same surface origin, alpha-mask evaluation, hit-point reconstruction, sampling budget, and radiance cache. No ray-query or acceleration-structure feature is needed by Compute.

This explicitly supersedes the earlier D2 compute-tier limitation and the October 11 backend description. The user's requested scope was removal of Sponza's fixed floor blocks with near-matching Compute/HW RT output. Earlier voxel-reference acceptance and performance reports remain historical evidence, not qualification of this new traversal.

## Cause and correction

The old ray began inside the floor's occupied voxel and treated the entire cell as a blocker. In the frozen Sponza hall diagnostic at 64³, 32,508 of 32,509 zero-sky floor pixels had occupied starts. Increasing voxel resolution reduced the footprint without removing the cause.

`SoftwareSceneRayQuery` builds world-space triangle bounds from committed scene geometry. A depth-first BVH with escape links supports stackless shader traversal, nearest-hit queries, and alpha rejection without a fixed traversal-depth limit. Disabled instances are excluded. Scene changes rebuild the buffers; failed preparation refuses stale geometry; failed execution and scene replacement invalidate them. Existing deferred buffer destruction protects submitted readers. Buffer sizes are checked against device limits.

Both providers now use raster FP32 receivers and triangle surface origins. The occupied-voxel filter boundary is no longer applied to triangle visibility. Auto bounce/reflections use ray hits on both backends; explicit legacy cone and shadow-map settings remain available. Both backends use the same adaptive four/six-ray policy. This changes the previous hardware four-ray policy, so previous hardware timing qualifications do not transfer.

Two additional boundary issues were exposed by comparison fixtures. Closest hits now reconstruct their surface position from barycentrics instead of `origin + t * direction`, preserving constant triangle coordinates at radiance-cell boundaries. Sky-cache owner points projected onto an exact triangle edge are inset by a barycentric minimum of `1e-5`, preventing transform rounding from placing the two providers on opposite sides of that edge. This resolved the enclosed-room cache differences without opening closed surfaces. New radiance caches also retain a cleared previous texture until a successful injection exists, avoiding an uninitialized snapshot copy.

## Verification

Windows/MSVC performance build, RTX 5080, 64³ voxel radiance cache. Compute captures run with `--disable-rt`; metadata confirms both ray queries and acceleration structures are disabled. Disassembly of the compute trace/cache/injection and software shadow shaders contains no hardware query capability, acceleration-structure type, or ray-query instruction.

The reusable `tools/validate_compute_ray_parity.py` passes 26 material/geometry/lighting fixtures and Sponza hall/top. Fixtures cover closed and single-sided shells, alpha UV sets/transforms/vertex alpha/specular-glossiness/wrap modes, mirrored surfaces, narrow openings, thin poles, forward receivers, blend/transmission blockers, exterior geometry, HDR environments, direct lights, bounce and reflections.

At 960×540 after 64 frames, environment lighting plus bounce:

| Sponza view | Combined RGB RMS difference | P99 difference | Largest extra zero-sky cluster |
| --- | ---: | ---: | ---: |
| Hall | 0.112% | 0.404% | 1 pixel |
| Top | 0.041% | 0.203% | 0 pixels |

These are relative to the HW RT covered-receiver mean. The regression checks every lighting signal at 1% RMS / 2% P99. Near-black signal normalization has a floor of 10% of the corresponding total lighting mean, limiting absolute differences to 0.1% / 0.2% of that mean. This avoids meaningless large percentages for nearly zero bounce outside the cache. Isolated stochastic misses are allowed; an extra zero-sky cluster larger than four connected pixels fails. These thresholds are specific to backend parity, not changes to archived independent-reference acceptance limits.

`validate_hardware_shadows.py --software` passed 11 independent comparisons against its 2026-10-14 Embree CPU oracle (since 2026-10-15 the oracle runs on Mitsuba CUDA; these comparisons have not been rerun), including point/spot/directional lights, finite endpoints, masks, forward/tilted surfaces and moving geometry/light snapshots. Every comparison has zero visibility mismatches; injection comparisons and scattering checks pass. Native tests: 581 render-core, 43 editor-rendering and 24 voxel-GI integration cases pass. Editor coverage includes per-feature routing, runtime switches, failed resources and inline/threaded execution.

The pure-compute `motion_roof` capture also checks 16 transform updates and 16 vertex-deformation updates at 64×40. Every frame advances the geometry generation and resets incompatible history; receiver positions match the corresponding frozen high-sample pose. The image changes with the roof displacement. One-frame sky RMS against the converged software reference is at most 7.62% / 7.51% for transform/deformation, respectively. Evidence: `motion-compute/motion-checks.json` and its pose captures.

Generated evidence is under `build/compute-triangle-fix/`: `parity-final/parity.json`, `shadows-compute/results.json`, native test logs/XML, and the captures. The older failed `parity` and `parity-stable` runs retain the boundary issue evidence. The parity runner explicitly reports the previously reproduced `SYNC-HAZARD-PRESENT-AFTER-WRITE`; other Vulkan errors fail. This change does not fix that separate presentation issue.

## Cost and limits

The initial triangle implementation was a quality correction. In its instrumented 960×540 captures, median GPU frame time over frames 16–62 was 78.59 ms Compute versus 6.91 ms HW RT in the hall, and 57.07 versus 6.91 ms from above. Validation was enabled; these historical measurements are full-frame times, not GI-exclusive timing. That implementation used 64 bytes per triangle plus 48 bytes per node. The optimization below supersedes its compute traversal cost and node layout. The BVH still rebuilds on geometry changes while any enabled feature uses Compute. Since 2026-10-15 an all-HW RT configuration binds placeholders instead and neither builds nor depends on it. Broader P8 and total-memory qualification remain open.

The voxel radiance cache still limits bounce/reflection detail, and transmission/scattering retain the shared existing per-surface environment path. This work verifies one GPU with RT disabled; it does not claim cross-vendor or MoltenVK qualification.

## Compute traversal optimization — 2026-10-14

The optimized builder uses 16-bin surface-area splits and two-triangle leaves. This reduces child-bound overlap and wasted triangle tests. Nodes occupy 32 bytes with full FP32 bounds; triangle data remains 64 bytes. The shader computes ray reciprocals and projection axes once per ray, vectorizes the box test, and reconstructs the accepted nearest surface once after traversal. Bounds remain conservative, zero-direction slabs remain explicit, and triangle/alpha acceptance uses the same arithmetic and material rules. After 32 skewed builder levels, median splitting bounds CPU recursion; this does not limit traversal or discard triangles.

The sample budget, adaptive four/six-ray policy, render/cache resolution, histories, filters, bounce, reflections, and shadows are unchanged. A near-first traversal experiment was slower and was discarded. Smaller nodes do not establish a total-memory reduction: two-triangle leaves increase the node count, and measured VMA device-local peak stayed at 1,143.19 MiB.

Comparable full-frame timings on RTX 5080, driver 617.42, Windows/MSVC performance build, Sponza at 960×540 and 64³: 32 warmup frames followed by 300 measured frames, fixed-step camera/environment, validation disabled, VSync disabled, RHI thread and async compute enabled, and `--disable-rt`. Before/after configuration hashes and resolved settings match. The baseline executable and shader bundle were archived before optimization and rerun after the optimized measurements.

| Sponza view | Before GPU median | After GPU median | Speedup | Before / after GPU P95 |
| --- | ---: | ---: | ---: | ---: |
| Hall | 77.79 ms | 15.23 ms | 5.11× | 87.19 / 16.94 ms |
| Top | 44.60 ms | 16.78 ms | 2.66× | 71.45 / 18.93 ms |

Earlier 180-frame baseline runs measured 75.29 / 56.23 ms in hall/top, respectively; baseline variation is retained in the evidence. The table uses equal 300-frame runs. These measurements qualify static Sponza traversal on this GPU; they do not measure CPU BVH construction cost or performance for large deforming scenes.

Quality checks compare the optimized captures to both the archived Compute and HW RT captures. All 28 cases pass each comparison. The new `--compute-baseline` mode checks identical sampling/history/filter settings and uses tighter per-signal thresholds of 0.2% RMS / 1% P99, with the same near-black normalization described above.

| Sponza view | Combined RGB RMS vs prior Compute | P99 | Extra zero-sky pixels |
| --- | ---: | ---: | ---: |
| Hall | 0.1075% | 0.4052% | 0 |
| Top | 0.0385% | 0.1955% | 0 |

The largest per-signal RMS difference is 0.1481%. Rerunning the unchanged baseline hall itself produced 0.1212% combined RMS / 0.4631% P99 difference from its archived capture, so these small differences are within observed repeat-run variation; output is not claimed bit-identical. Both optimized Sponza views also have zero extra zero-sky pixels against HW RT.

Final verification: 581 RenderCore, 43 EditorRendering, and 24 ConeVoxelGIIntegration tests pass (nine existing disabled tests remain disabled). All 11 independent shadow comparisons pass with zero visibility mismatches, including injection and moving snapshots (Embree CPU oracle; see the 2026-10-15 follow-up). The 16 transform and 16 deformation updates also pass: generations advance, history resets, receiver positions match, and combined RGB RMS against the previous Compute frames is at most 0.0111% / 0.0098%. Their single-frame sky RMS against converged pose references remains at most 7.62% / 7.51%. All 12 BVH shader modules pass SPIR-V validation and match the 32-byte node layout; the ten pure software modules contain no hardware query capability. The editor and demo are rebuilt.

Evidence is under `build/compute-ray-perf/`: `verification.json`, `repeated-baseline-{hall,top}/summary.json`, `final-{hall,top}/summary.json`, `quality/{parity,hardware-parity}.json`, `baseline-repeat-quality.json`, `shadows/results.json`, `motion/motion-checks.json`, `shader-checks.json`, and native test XML/logs. The existing presentation hazard is still reported separately by the image checks.

## Follow-up fixes — 2026-10-15

Review fixes, verified on the RTX 5080 with the Windows/MSVC performance build:

- **HW RT independence.** `VoxelGIRenderer::UsesSoftwareQueries()` reports whether an enabled feature resolves to Compute. Only then is the software BVH prepared. Otherwise `SoftwareSceneRayQuery::PrepareUnused()` binds one-element placeholders with a zero node count. A failure to build the BVH can no longer drop an all-HW RT frame to PBR, and animated all-HW RT frames no longer rebuild the BVH on the CPU.
- **Triangle-query oracle.** `RayQueryIntegrationTest.SoftwareTraversalMatchesBruteForceTriangleOracle` traces 2,048 rays on the GPU through `SoftwareQuery`. It compares them with a double-precision brute-force CPU intersection over the authored triangles. The scene covers 160 overlapping random triangles instanced twice (once rotated), a disabled overlapping instance, a mirrored instance, a quad with a shared diagonal, exponentially skewed centroids, a zero-alpha mask in front of a blocker and a vertex-alpha mask. Rays near an edge, the alpha cutoff, the maximum distance or a distance tie are excluded from strict checks. In all four execution modes, 1,625 rays are decided with zero mismatches in hit/miss, triangle identity, distance, barycentrics, hit position and normal facing: 1,347 hits, 278 misses, 118 continuations through rejected alpha candidates and 174 rays cut by a finite maximum. All 128 shared-diagonal rays hit the quad. With every instance disabled, and with the placeholders, every ray misses.
- **Occupied-start remnants removed.** The sky guide's alpha has been constant since triangle visibility. The filter, sky-search and support passes no longer test it, and the support bounds buffer stores six normal buckets per 8×8 tile instead of twelve.
- **Verification.** 16 RayQueryIntegration, 588 RenderCore (7 disabled), 43 EditorRendering (2 disabled) and 24 ConeVoxelGIIntegration tests pass. `LightingBackendsSwitchIndependently` now also checks that the BVH is held exactly when a Compute feature is enabled. Sponza renders at 960×540 for 64 frames with synchronization validation on both backends. Apart from the existing presentation hazard there are no validation messages. All-HW RT issues zero software rays.
- **Not rerun.** Compute/HW RT image parity, the CUDA shadow oracle and benchmarks were not rerun after these changes; the independent CUDA ground truth was, below. The earlier timing and quality figures above remain those of the 2026-10-14 build.

## CUDA ground truth — 2026-10-15

Both tiers were compared only against Mitsuba `cuda_ad_rgb` references: no tier-reference, cross-backend or Embree comparison enters these results. Each reference passed its analytic fixtures first. Build: Windows/MSVC performance, RTX 5080, 960×540, 64³ sky captures with normal maps stripped.

**Sky, all 18 frozen views (9 environments × top/hall).** The stored `dataset-p4-corrected` renders (32,768 samples per pixel, all `cuda_ad_rgb`) were compared with fresh captures by `ground_truth_sweep.py --skip-shipping-gate --reference-samples 256`. The converged tier reference uses 256 rays × 256 frames (65,536 rays per pixel, the plan's amended split). Compute's 1024-ray frames take about 5.5 s and tripped the 2 s Windows GPU watchdog; 256-ray frames take 1.32–1.40 s.

| Tier | Converged reference vs truth | Rung-3 verdict | Shipping (64 frames) vs truth |
| --- | --- | --- | --- |
| Compute | bias −0.03% to −0.10%; excess RMS 0.00–1.79%; 8×8 excess ≤ 0.35% | 18/18 pass (7 hall views on 8×8 blocks, where the truth's per-pixel noise is 2.2–11.1%) | bias −0.14% to −0.30%; excess RMS 0.54–12.43% (report-only) |
| HW RT | same to within 0.0011 pp excess per view | 18/18 pass (same 7 on 8×8 blocks) | same to within 0.013 pp excess |

The worst converged excess is qwantani-hall (1.79%, mostly far from the camera). The worst shipping excess is studio-hall (12.43%), where the truth's own per-pixel noise is 11.14%. Primary-surface mismatches, 1,101 pixels (0.21%) in top views and 4,718 (0.91%) in hall views, are excluded from the verdict and reported per the plan. An HW RT rerun with 1024 × 64 frames differs from 256 × 256 by at most 0.032 pp excess, confirming the split does not change the reference.

**Fixtures** (`validate_fixture_ground_truth.py`; 128³; CUDA one-bounce and GGX-lobe references rendered for these captures):

| Fixture | Limit | Converged reference, both tiers | Shipping, both tiers (report-only) |
| --- | --- | --- | --- |
| Point-light room, one bounce | \|bias\| ≤ 10% | −4.52% (excess RMS 6.22%) | −4.65% (6.38%) |
| Thin wall lit on one side, one bounce | \|bias\| ≤ 10% | −4.77% (9.92%) | −4.88% (10.14%) |
| Glossy floor, roughness 0.2, `S` | max \|error\| ≤ 2 pp | 1.49 pp max, 0.85 pp P99 | 7.18 pp max, 1.42 pp P99 |
| Glossy emissive floor, `S` / reflected emission `C` | `S` ≤ 2 pp; `C` report-only | 1.96 pp; `C` bias −1.43% | 7.58 pp; `C` bias −1.44% |

Compute and HW RT produce identical scores at the printed precision. Their captures differ only by float rounding (about 1e-7 relative in bounce), with ray queries and acceleration structures disabled for Compute. The −1.43% reflected-emission difference matches the P6 record; RMS rows remain report-only.

Evidence: `build/ground-truth/runs/2026-10-15-{compute,hardware}-256/{report.json,summary.md}`, `build/ground-truth/runs/2026-10-15-hardware/` (1024 × 64 cross-check), `build/ground-truth/runs/2026-10-15-fixtures-final-{voxel,hardware}/cuda-fixture-results.json` and the CUDA reference outputs beside them.

```powershell
build/venv/Scripts/python.exe tools/ground_truth_sweep.py --exe build/x64-windows-msvc-performance/bin/scene_renderer_demo.exe --scene E:/Dev/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf --dataset build/ground-truth/dataset-p4-corrected --skip-shipping-gate --reference-samples 256 --provider voxel --output build/ground-truth/runs/compute-256
build/venv/Scripts/python.exe tools/validate_fixture_ground_truth.py --exe build/x64-windows-msvc-performance/bin/scene_renderer_demo.exe --provider voxel --capture --output build/ground-truth/runs/fixtures-voxel
```

Use `--provider hardware` for HW RT. A Compute sweep takes about 100 minutes; the reference capture is about 99% of it.

## Reproduce

```powershell
python tools/validate_compute_ray_parity.py --exe build/x64-windows-msvc-performance/bin/scene_renderer_demo.exe --output build/compute-triangle-parity --scene E:/Dev/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf --allow-present-baseline
python tools/validate_hardware_shadows.py --exe build/x64-windows-msvc-performance/bin/scene_renderer_demo.exe --output build/compute-triangle-shadows --software --allow-present-baseline
python tools/validate_compute_ray_parity.py --exe build/x64-windows-msvc-performance/bin/scene_renderer_demo.exe --output build/compute-optimized-parity --compute-baseline build/compute-triangle-fix/parity-final --scene E:/Dev/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf --allow-present-baseline
python tools/benchmark_voxel_gi.py --executable build/x64-windows-msvc-performance/bin/scene_renderer_demo.exe --output build/compute-optimized-hall --config build/compute-triangle-fix/parity-final/sponza-hall-compute.config.cfg --backend compute --profile --frames 300 --warmup 32 --width 960 --height 540
```

Run both runners with `build/venv/Scripts/python.exe` (`tools/requirements-gi-quality.txt`). The shadow oracle requires Mitsuba's CUDA backend and fails without it. Engine configuration is restored transactionally by both runners.
