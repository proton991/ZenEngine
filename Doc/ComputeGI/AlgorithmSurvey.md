# Compute-only GI: algorithm survey and recommendation

2026-10-22. Research only; no code has changed. Scope: a GI path that needs no hardware ray tracing, scales across GPU capability tiers, and may trade exact agreement with HW RT or the Mitsuba references for speed.

## Why the current Compute tier is slow

From the [P8 close-out](../HybridGI/P8Closeout.md) and the [Nsight work](../HybridGI/NsightOptimizations.md):

- Compute GI takes 37.8–40.5 ms at 1080p on the RTX 5080, against a 4 ms target. About 90% of it is software BVH traversal.
- The cost is per query, not per distance. A ray half a voxel long costs 35–43% of a full-length one, because every query descends from the BVH root ([near/far experiment](../HybridGI/NsightOptimizations.md#lumen-style-nearfar-compute-trace-2026-10-17)). Shorter rays barely help a BVH.
- Voxel-only tracing (variant A0) cut the trace pass from about 63–65 ms to about 3 ms at Nsight base clocks. It failed on quality: 0.47 m binary cells blocked about 30% of the sky reaching the floor. That is a resolution and representation failure, not proof that volumes cannot work. Lumen and Brixelizer place surfaces inside a cell with distance fields, and finer sparse cells have the same effect.
- The pipeline issues about 6.7 rays per pixel (5.7 sky + 1 specular in the hall count), then runs temporal accumulation and five à-trous passes over 160 B/pixel of history. The HW RT tier, at 10.9–12.5 ms, also misses its 3 ms target. The per-pixel gather and full-resolution reconstruction are shared by both tiers.
- The existing cone path uses one isotropic 64³–256³ volume over the whole scene, with no clipmap. It is cheap, but it leaks and did not meet the P0 limits.

So a faster tracer alone will not fix this. Every compute GI system that ships today combines three things:

1. A cheap scene representation that can be located in O(1) from the ray origin.
2. Far fewer rays, gathered at screen probes rather than per pixel.
3. Caches that let a ray stop at its first hit and read lighting there.

## Survey

### 1. Voxel cone tracing (Crassin 2011; clipmap variants)

- **Method:** prefiltered radiance and opacity mips. A cone marches through mip levels that match its footprint.
- **Shipped:** The Tomorrow Children (Q-Games, PS4, 30 Hz) used cascaded 3D textures instead of a sparse octree for speed. Voxelization was too expensive for dynamic objects, so those were handled separately. CryEngine's SVOGI shipped in Kingdom Come: Deliverance 2 (2025). Warhorse calls it an improved SVOGI ("software ray tracing") and confirmed it on consoles, which run 1080p60 in performance mode.
- **Retired:** UE4 dropped SVOGI after the 2012 Elemental demo for performance reasons, and Lumen later took its place. Godot keeps cone-traced VoxelGI for small baked areas but added SDFGI for large scenes.
- **Fit:** noise-free and cheap per cone. However, mip-averaged opacity both leaks and over-occludes. Anisotropic six-direction mips reduce leaking but cost 6× the memory. Cost scales with pixels × cones × steps.

### 2. Lumen software ray tracing (UE5)

- **Method:** a screen trace first, then each mesh's SDF for the first ~2 m, then a merged global SDF clipmap. Hits are lit from a surface cache of captured cards. A screen-probe gather is backed by a world radiance cache.
- **Cost:** at Epic scalability, about 8 ms for GI and reflections at 1080p internal on PS5/XSX (30 fps target). High scalability targets 60 fps. Requires SM6, so roughly GTX 1070 or newer.
- **Limits:** walls must be at least 10 cm thick. No world-position offset, limited skinned-mesh support, and a heavy content pipeline (offline mesh SDFs, card capture). UE 5.6 brought hardware RT's cost in line with the software mode.
- **Fit:** the architecture reference. The mesh-SDF and card machinery is too much asset tooling for a first step.

### 3. AMD FidelityFX Brixelizer GI (2024, MIT, DX12 and Vulkan)

- **Scene:** camera-centred cascades of 64³ cells. Each occupied cell gets an 8³ R8 SDF brick in a shared 3D atlas, and each cascade has a 3-level AABB tree.
- **GI pipeline (based on GI-1.0):**
  - One 8×8 octahedral screen probe per 8×8 tile. The default runs at half internal resolution, which is about 8k probes for 1080p output.
  - A radiance cache: a 256³ R11G11B10 atlas, 4³ texels per brick, fed from the previous lit frame at quarter resolution.
  - An SH2 irradiance cache per brick, propagated between bricks with time slicing.
  - A quarter-resolution specular pre-trace that stores brick IDs. The full-resolution pass then marches only inside the hit brick.
- **Cost:** about 1.7 ms on an RX 7600 XT at 1080p, about 2.0 ms on an RX 7900 XTX at 4K, and about 2.2 ms on an RTX 4080 at 4K.
- **Memory:** a 128 MB SDF atlas plus 16 MB internal buffers, plus about 110–200 MB of GI at 1080p depending on internal scale.
- **Limits:** the radiance cache only learns from surfaces that have been on screen. Small details are lost, and foliage is a known weak point.
- **Fit:** the closest match to our goals, and the best evidence that compute-only GI fits in about 2 ms on mid-range hardware.

### 4. Godot SDFGI → HDDAGI

- **SDFGI:** cascaded SDFs plus a probe grid, Forward+ only. Godot's docs call it one of the most demanding GI options, and it has no dynamic occluders.
- **HDDAGI:** [PR #119869](https://github.com/godotengine/godot/pull/119869) replaces the SDF with a hierarchical DDA over occupancy. It was still open in late August 2026. It claims a faster base cost, updates an order of magnitude faster, and fewer leaks. Known issues include DEVICE_LOST on Intel and wrong probe occlusion on DX12.
- **Fit:** it shows the move from SDFs to bitmask occupancy for cheaper updates.

### 5. World probe grids traced in compute (DDGI-style)

- **Examples:** the SDF-DDGI paper (2020), LuxGI (DDGI with an SDF fallback), and Enlisted's daGI, a regular probe field computed from voxels, triangles or both.
- **Fit:** the ray budget is fixed and independent of resolution, and each pixel only needs a trilinear lookup. Results are low-frequency, leak without per-probe visibility, and need probe placement work. A good lowest tier and far-field fallback.

### 6. Radiance cascades

- **Screen space:** used in Path of Exile 2. One anecdotal report puts it at about 0.8 ms at 4K on an RTX 4090. It cannot see off-screen geometry.
- **Split Radiance Cascades** (Freeman, Sannikov, Pitt-Francis, arXiv, July 2026): world-space probes in a sparse hash map, with rays split into cascade intervals by hit distance. A single frame of San Miguel reportedly takes 13.2 ms, of which 6.1 ms is 1-spp tracing. I have not verified the tracing backend.
- **Fit:** research. Worth watching, but not a 2026 shipping basis.

### 7. Screen-space only

- **SSILVB** (Therrien et al.): replaces horizon angles with a sector visibility bitmask, so light passes behind surfaces of assumed thickness.
- **Fit:** a cheap contact and short-range complement. It cannot be the primary GI.

### 8. Systems built around HW RT (context)

- **Examples:** AMD GI-1.0/1.1 (screen probes plus a hash-grid world cache), on-surface radiance caches (HPG 2024, 300–700 MB per viewer), Assassin's Creed Shadows (probes as the radiance cache for missed rays), idTech 8 (a cache chain ending in irradiance volumes), and Snowdrop/Avatar (a compute fallback for RT on non-RT GPUs, no published numbers).
- **Fit:** their cache structures carry over; their ray counts do not.

## Is voxel cone tracing the right core?

Not for the main compute tier.

1. **Quality.** Mip-filtered opacity averages occluders with empty space. It leaks through thin walls and darkens open areas near geometry. We already measured about 30% sky darkening from coarse occupancy. Cones through the same mips share that root cause and add leaks.
2. **Cost.** Per-pixel cones multiply pixels × cones × steps × trilinear 3D fetches. Meeting the budget requires reduced resolution, which brings back upsampling and temporal reconstruction. That gives up VCT's main advantage: no noise and no history.
3. **Trend.** Engines that started with cones moved to ray gathers over SDF or occupancy volumes with caches: UE4/UE5 to Lumen, Godot to SDFGI and HDDAGI. CryEngine still ships cones, which shows it can be tuned to console 60 fps, but at a quality ceiling we have already hit.

Cones remain useful in two places: the lowest tier, where the existing code needs no history, and prefiltered lookups for rough specular and the far field from a radiance clipmap.

## Recommended direction

Use a **screen-probe final gather over a sparse voxel-brick clipmap, with a surface radiance cache**. This is the Lumen / GI-1.0 / Brixelizer GI family, but with bitmask bricks instead of SDFs.

```
Lights + shadow maps ─► surface radiance cache (per occupied cell × face class)
                              ▲ previous-frame irradiance (multi-bounce)
Scene triangles ─► brick clipmap (4–6 cascades × 64³ cells, 8³-bit bricks)
                              │ hit lookup
G-buffer ─► screen probes ─► HZB screen trace ─► brick DDA ─► sky (importance sampled)
             (1 per 16×16 px + adaptive, 8×8 octahedral)
                 │
                 ▼
     probe temporal + 3×3 probe spatial ─► per-pixel integration ─► diffuse GI
Specular: rough → probes; glossy → ¼-res rays, same tracer; mirror → SSR, then brick DDA
Tracer is pluggable: brick DDA (Compute) or rayQuery (HW RT)
```

### A. Scene representation: brick clipmap

- **Layout:** 4–6 camera-centred cascades of 64³ cells, each cascade doubling the cell size (for example 0.25 m → 8 m, tuned per content). A cell is either empty or indexes an 8³-bit brick (64 B). Cascade 0 then resolves about 3 cm, roughly 15× finer than today's 0.47 m Sponza cells. This targets the measured thickening directly.
- **Traversal:**
  - The start cell comes straight from the ray origin, so there is no root descent, the cost that dominated the BVH.
  - A per-cell Chebyshev distance to the nearest occupied cell (one jump flood per 64³ cascade) lets rays skip empty space.
  - Inside a brick, rays step with bit-tested DDA.
  - A 64-tree hierarchy with an ancestor stack is optional; it roughly halved cycles per ray in [dubiousconst282's tests](https://dubiousconst282.github.io/2024/10/03/voxel-ray-tracing/).
- **Build:** reuse the existing compute voxelizer in three steps: mark coarse cells, allocate bricks, then set bits with atomicOr. Clipmap scrolling re-voxelizes only the new slab, and dynamic objects re-voxelize only their dirty bricks. This replaces today's whole-scene re-voxelization on any geometry change.
- **Memory:** at 10% cell occupancy, four cascades need about 7 MB of bricks plus 4 MB of cell indices. Brixelizer's SDF atlas alone is 128 MB.
- **Why bitmask before SDF:** building is plain voxelization with no distance computation, dynamic updates are local, and the bit and integer operations run on every GPU. Godot made the same move. Evaluate SDF bricks only if cascade-0 sky bias stays high.

### B. Surface radiance cache

- **Reuse:** the existing radiance injection, owner normals and six surface classes, re-keyed to bricks (occupied cell × face class, packed into R32UI).
- **Multi-bounce:** inject the previous frame's irradiance at each cell.
- **Advantage over Brixelizer:** injection does not depend on the screen, so off-screen surfaces are lit too.

### C. Diffuse final gather with screen probes

- **Probes:** one probe per 16×16-pixel tile, plus adaptive probes where interpolation fails (as in Lumen). Each probe traces 8×8 octahedral directions, importance-sampled from the previous frame's probe radiance × cosine.
- **Ray count:** about 8k probes × 64 ≈ 0.5 M rays at 1080p, plus adaptive probes. Today's pipeline traces about 14 M (6.7 per pixel), so this is 15–25× fewer rays.
- **Per ray:** a short HZB screen trace (contact detail and geometry thinner than a voxel), then the brick DDA, then the sky with the existing environment importance sampling.
- **Optional world probes:** a coarse world radiance-probe clipmap, Lumen-style. Screen-probe rays stop after N metres and interpolate it. Add this only if long rays dominate the measurements.

### D. Reconstruction

- **Steps:** temporal accumulation in probe space, a 3×3 probe spatial filter with plane and angle weights, per-pixel integration from neighbouring probes (SH2 or octahedral), and a short per-pixel temporal pass.
- **Replaces:** the five full-resolution à-trous passes and most of the 160 B/pixel of history.

### E. Specular

- **By roughness** (as in GI-1.1 and Brixelizer GI): rough surfaces read the probes; glossy surfaces get quarter-resolution traced rays with an upsample; mirror surfaces use SSR, then the brick DDA, then the cache.

### F. Pluggable tracer

The same pipeline runs with rayQuery where hardware RT is usable. The HW RT tier shares the per-pixel gather and full-resolution reconstruction that this design replaces, so it should also get faster than its current 10.9–12.5 ms. That needs measurement.

## Capability tiers

| Tier | Eligibility (capabilities only) | Diffuse | Tracer | Specular | Proposed 1080p budget |
| --- | --- | --- | --- | --- | --- |
| HW RT | Usable ray queries (`RHIRayQueryCapabilities`) | Screen probes | rayQuery | Probes + traced glossy | ≤ 2.5 ms, RTX 5080 |
| Compute | ≥ 32 KB shared memory, R32UI storage images and atomics, basic subgroup ops | Screen probes | Brick DDA | Probes + ¼-res glossy | ≤ 3 ms RTX 5080; ≤ 5 ms RX 7600 / RTX 3060 class |
| Compute-lite | Vulkan minimums (16 KB shared memory), no int64 or float atomics | World probe grid, 2 cascades, amortized updates; SSILVB contact | Brick DDA | Probes, environment | ≤ 2 ms, iGPU / Apple M1 class |
| Minimal | Anything | Existing cone tracing at reduced resolution, or environment + AO | Voxel mips | Environment | — |

- **Selection:** follows capabilities, never device or driver IDs. Among eligible tiers, the preset decides, with a user override.
- **Gap:** features cannot separate Compute from Compute-lite, because an iGPU reports the same features as a discrete GPU. The device type (integrated or discrete) or a one-time timed calibration could set the default; see the open questions.
- **Budgets:** these are proposals that make G0 concrete, anchored on Brixelizer GI's ~1.7 ms on an RX 7600 XT at 1080p.

## Portability rules for the new shaders

- Keep bitmasks as `uvec2`/`uvec4`, with 64-bit popcount done as two 32-bit popcounts. No `shaderInt64`.
- Keep workgroup shared memory ≤ 16 KB on the Compute-lite path. Vulkan guarantees only 16 KB and Apple GPUs cap at 32 KB. Today's hybrid trace uses 31.7 KB.
- Write subgroup-size-agnostic code: Apple and NVIDIA use 32, AMD 32/64, Intel 8–32, Mali 16 and Adreno 64/128.
- Use atomics only on 32-bit integers. Pack HDR values into R32UI manually, because storage writes to R11G11B10 or RGB9E5 are not universally supported.
- Avoid sparse residency, which MoltenVK does not offer. Use an explicit brick atlas with an indirection table.
- Use FP16 where `shaderFloat16` is available, with an FP32 fallback. Do not depend on async compute.

## Plan sketch

| Gate | Work | Exit |
| --- | --- | --- |
| G0 | Per-tier budgets; test views (Sponza top/hall, Papermill, hotel room); a relaxed quality metric, e.g. diffuse mean bias within ±10% of Mitsuba plus a leak/flicker review; target devices: RTX 5080, RX 7900 XT, AMD iGPU, Apple Silicon over MoltenVK | Agreed numbers |
| G1 | Brick clipmap: build, scroll, dynamic updates, DDA tracer, GPU oracle against software triangles | Floor sky bias on the near/far views within the relaxed limit (was about −30%); ≥ 5× the BVH's rays per second on the same ray set |
| G2 | Surface radiance cache on bricks, with multi-bounce feedback | Bounce comparable to the current voxel cache on existing fixtures |
| G3 | Screen-probe diffuse gather and reconstruction | Compute budget met on RTX 5080; stable under the existing motion views |
| G4 | Specular paths | Reflection fixtures |
| G5 | rayQuery tracer plug-in, Compute-lite probe grid, tier selection, runs on each target platform | Per-tier time and memory reports |

**Build or integrate:** Brixelizer and Brixelizer GI are MIT-licensed with a Vulkan backend. Use their source and sample as a reference and a timing baseline on the same GPUs. Shipping them as-is would mean mapping FidelityFX's backend onto our RHI and losing our off-screen light injection.

## Open questions

1. **Content scale:** interiors like Sponza, or open worlds? This sets cascade count and cell sizes.
2. **Dynamic content:** do skinned characters need to occlude and bounce light? Do emissive surfaces matter?
3. **Lowest platform:** Apple M1-class, Android, or desktop iGPU?
4. **Compute-tier specular:** must it have traced glossy reflections, or are probes + SSR + environment enough?
5. **Ownership:** our own implementation (recommended), or integrate Brixelizer?

## Sources

- [AMD GDC 2024: GI with FidelityFX Brixelizer](https://gpuopen.com/gdc-presentations/2024/GDC2024_GI_with_AMD_FidelityFX_Brixelizer.pdf), [Brixelizer GI docs](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/brixelizer-gi/), [Foliage in Brixelizer GI](https://gpuopen.com/learn/brix-foliage-gi/brix-foliage-gi-foliage_in_brixelizer_gi)
- [Lumen technical details (Epic)](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine), [UE 5.6 HWRT cost (Wccftech)](https://wccftech.com/unreal-engine-5-6-preview-out-now-epic-promises-vast-open-worlds-at-consistent-60hz/amp/)
- [Godot SDFGI docs](https://docs.godotengine.org/en/stable/tutorials/3d/global_illumination/using_sdfgi.html), [HDDAGI PR #119869](https://github.com/godotengine/godot/pull/119869), [HDDAGI announcement (GameFromScratch)](https://gamefromscratch.com/godot-new-global-illumination-system-hddagi/)
- [Split Radiance Cascades (arXiv 2607.20384)](https://arxiv.org/abs/2607.20384), [PoE2 timing comment (HN)](https://news.ycombinator.com/item?id=41957008), [Chalmers RC thesis](https://odr.chalmers.se/items/3ee9fb4e-1880-46c2-802a-a660e38dc9ee)
- [SSILVB (arXiv 2301.11376)](https://arxiv.org/abs/2301.11376v2)
- [SDF-DDGI (arXiv 2007.14394)](https://arxiv.org/pdf/2007.14394), [LuxGI](https://www.github.com/flwmxd/LuxGI), [Scalable Real-Time GI, Enlisted (GDC 2019)](https://www.gdcvault.com/play/1026469/Scalable-RealTime-Global-Illumination)
- [The Tomorrow Children: lighting and mining with voxels (SIGGRAPH 2015)](https://history.siggraph.org/wp-content/uploads/2022/10/2015-Talks-McLaren_The-Tomorrow-Children-Lighting-and-Mining-with-Voxels.pdf), [KCD2 SVOGI on consoles (Wccftech)](https://wccftech.com/kingdom-come-deliverance-2-dev-60fps-on-consoles-is-not-out-of-reach-game-will-support-voxel-based-gi/amp/), [KCD2 console modes (GamePro)](https://www.gamepro.de/artikel/kingdom-come-deliverance-2-clevere-grafiktechnik,3427400.html)
- [Fast voxel ray tracing with sparse 64-trees (dubiousconst282)](https://dubiousconst282.github.io/2024/10/03/voxel-ray-tracing/)
- [GI-1.1 (AMD)](https://gpuopen.com/learn/gi-1-1-glossy-reflection-rendering/), [GI-1.0 (AMD)](https://gpuopen.com/download/GPUOpen2022_GI1_0.pdf), [On-surface radiance caches (HPG 2024)](https://highperformancegraphics.org/slides24/hpg24_oscgi.pdf)
- [SIGGRAPH 2025 Advances: AC Shadows](https://advances.realtimerendering.com/s2025/content/Advances%202025%20-%20Raytracing%20the%20world%20of%20Assassin's%20Creed%20Shadows.pdf), [idTech 8 GI](https://advances.realtimerendering.com/s2025/content/SOUSA_SIGGRAPH_2025_Final.pdf), [Avatar compute RT fallback (GameStar)](https://www.gamestar.de/artikel/avatar-frontiers-of-pandora-raytracing,3371105.html)
