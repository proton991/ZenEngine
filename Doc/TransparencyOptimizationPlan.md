# Forward transparency optimization plan

Written 8 November 2026. It follows [SponzaCenterFPSProfiling20261107.md](SponzaCenterFPSProfiling20261107.md), which measured `ForwardTranslucent` at **19.906 ms of a 34.680 ms** median GPU frame. That trace was standalone ZenApp at 2360 × 1264 on an RTX 5080 with hardware ray queries, using the slow center view of the five-model Sponza assembly.

## Goal

- Bring `ForwardTranslucent` in that view to **≤ 4 ms**, and the GPU frame to **≤ 18 ms**.
- Remove the movement penalty (≈ 6 ms of editor frame interval) that coincides with re-sorting.
- Leave authored assets unchanged and pass the image-parity checks in [Validation](#5-validation-protocol).

These are targets. Phase 0 shows whether they are reachable and changes the order of the later phases.

## 1. What the frame is doing

### 1.1 Workload

A scan of the source glTFs finds only two BLEND materials in the assembly:

| Material | Model | Triangles | Notes |
|---|---|---:|---|
| `LeafSpring` | trees | **1,354,570** (834,740 canopy + 519,830 ground cover) | Double-sided, alpha factor 1. Base-level texture alpha: 93.58% zero, 6.18% one, 0.24% partial. 0.90 source vertices per triangle; two UV sets |
| `dirt_decal` | main | 8,140 | Alpha factor 0.35. Rests on receivers and already reuses their shadow-ray results |

So 99.4% of blended triangles are leaves. The leaf is a cutout material that happens to be authored as BLEND.

### 1.2 Costs confirmed in code

| # | Cost | Where |
|---|---|---|
| C1 | Every BLEND triangle becomes its own draw item. On any view change the renderer transforms 1.36 M centroids, radix-sorts them, rebuilds a 16.35 MB index stream, compares it with the previous one, rebuilds the batches and uploads the stream again. | [ForwardDrawCache.cpp:107-133](../ZenCore/Source/Graphics/RenderCore/V2/ForwardDrawCache.cpp#L107), [:177-218](../ZenCore/Source/Graphics/RenderCore/V2/ForwardDrawCache.cpp#L177), [DeferredLightingRenderer.cpp:1348-1363](../ZenCore/Source/Graphics/RenderCore/V2/DeferredLightingRenderer.cpp#L1348) |
| C2 | Translucent draws are not culled. GPU frustum culling only feeds the G-buffer, and Hi-Z occlusion culling does not exist yet. Every leaf triangle is submitted every frame. | [frustum_cull.comp:31-66](../Data/Shaders/SceneRenderer/frustum_cull.comp#L31), [DeferredLightingRenderer.cpp:1572-1611](../ZenCore/Source/Graphics/RenderCore/V2/DeferredLightingRenderer.cpp#L1572) |
| C3 | Per-triangle sorted indices remove vertex reuse. Work goes from 0.90 to 3 vertex invocations per triangle (≈ 1.22 M → ≈ 4.06 M). Each invocation writes 59 interpolated scalars, including 11 `vec4`s that pack 22 per-slot UVs, even though the leaf mesh has only two UV sets. | [offscreen_vertex.glsl:75](../Data/Shaders/SceneRenderer/offscreen_vertex.glsl#L75), [:135-147](../Data/Shaders/SceneRenderer/offscreen_vertex.glsl#L135) |
| C4 | The fragment shader reads the full surface (normal, tangent basis, every material texture) before it rejects zero-alpha texels. | [forward_material.glsl:203-224](../Data/Shaders/SceneRenderer/forward_material.glsl#L203) |
| C5 | Each blended layer is lit separately. Leaves do not rest on a receiver, so each layer runs the voxel cone-traced GI fallback. Each layer also evaluates shadows for every light with a nonzero contribution, which means ray queries under the hardware provider. Deferred shading does this once per pixel; the forward pass does it once per layer. | [forward_material.glsl:243-259](../Data/Shaders/SceneRenderer/forward_material.glsl#L243), [:342-359](../Data/Shaders/SceneRenderer/forward_material.glsl#L342), [scene_shadows.glsl:11-60](../Data/Shaders/ShadowMapping/scene_shadows.glsl#L11) |
| C6 | Leaves are non-opaque BLAS geometry. For every candidate hit, a shadow ray through a canopy runs a shader coverage test: it reads 3 indices, 3 UVs and 3 vertex colours, then fetches the 4096² leaf texture at LOD 0. A ray from inside a canopy toward the sun crosses many cards. The same cost applies to `SceneLighting`, `ProbeTrace` and `ProbeSpecularTrace`. | [hardware_ray_query.glsl:78-90](../Data/Shaders/VoxelGI/hardware_ray_query.glsl#L78), [scene_ray_geometry.glsl:57-92](../Data/Shaders/VoxelGI/scene_ray_geometry.glsl#L57), [SceneRayQuery.cpp:257-261](../ZenCore/Source/Graphics/RenderCore/V2/SceneRayQuery.cpp#L257) |
| C7 | The forward shader is an uber-shader. Sheen (64 taps), volume scatter (64 taps), refraction, clearcoat, anisotropy and retroreflection share one program. Branches are dynamic, but registers are allocated for the worst case. | [forward_material.glsl:95-196](../Data/Shaders/SceneRenderer/forward_material.glsl#L95) |
| C8 | Layers blend into RGBA16F. | [DeferredLightingRenderer.cpp:1247](../ZenCore/Source/Graphics/RenderCore/V2/DeferredLightingRenderer.cpp#L1247), [:1626-1639](../ZenCore/Source/Graphics/RenderCore/V2/DeferredLightingRenderer.cpp#L1626) |

### 1.3 What the existing measurements do and do not show

- **Lighting isolation (editor, whole frame):** turning shadows off saved 7.7 ms, indirect diffuse off saved 3.7 ms, and both together saved 11.5 ms. These switches also change `SceneLighting` and the GI passes, so they put an upper bound on forward lighting cost. They do not measure it.
- **Earlier depth experiments:** a depth prepass for fully opaque blended samples, and explicit early fragment tests, gave no measurable gain. The prepass hides leaf layers behind the nearest opaque leaf texel, but the visible leaf layer is still lit in the forward pass, and the prepass draws all 1.35 M triangles a second time. Either hidden-layer shading is not the main cost, or the second geometry pass cost about what it saved. The trace cannot tell which.
- **Movement penalty:** it coincides with C1, but nobody has timed it.

Phase 0 therefore has to split the 19.9 ms into geometry, rejected fragments, lit fragments and shadow rays before any large change is committed.

## 2. How other engines handle this

| Topic | Unreal Engine 5 | Unity HDRP | Godot 4 | Filament, Bevy, three.js |
|---|---|---|---|---|
| Foliage | **Masked** (alpha-tested) through the depth prepass and deferred base pass. Nanite supports masked foliage from 5.1. HLOD and impostors at distance | Opaque with alpha clipping | Alpha Scissor, Alpha Hash, alpha antialiasing (A2C), and a "Depth Pre-Pass" transparency mode | Filament `masked` (optionally alpha-to-coverage); Bevy `AlphaMode::Mask` / `AlphaToCoverage`; three.js `alphaTest` / `alphaHash` / `alphaToCoverage` |
| Sort granularity | **Per primitive**, by bounds, with Translucency Sort Priority and `r.TranslucentSortPolicy` | Per renderer plus sorting priority | Per object | Per renderable, entity or object |
| Translucent lighting | Per-material lighting mode: Volumetric (non)directional, Volumetric per-vertex, Surface TranslucencyVolume, or Surface ForwardShading (the expensive one). The Translucency Lighting Volume is a camera-aligned cascaded 3D grid; shadowed direct light is injected into it once per frame. Lumen adds a translucency GI volume; front-layer reflections are opt-in | Clustered forward light lists with shadow maps. Optional low-resolution transparent pass and Transparent Depth Prepass/Postpass | Clustered forward per pixel | Clustered forward |
| Alpha in ray tracing | Masked materials take any-hit shaders. Shadow rays can skip material evaluation (`r.RayTracing.Shadows.EnableMaterials`) or use simplified ray-tracing geometry | n/a | n/a | n/a |

Takeaways:

1. **No mainstream engine sends dense foliage through its sorted translucent path.** Foliage is alpha-tested in the opaque path and shaded once per pixel.
2. **Engines sort per object or draw, not per triangle every frame.** Finer ordering is either order-independent transparency or a small special case, such as per-strand GPU sorting for hair.
3. **Translucent lighting is made deliberately cheaper than opaque lighting**, through volumes, per-vertex lighting or light lists. Per-pixel ray-traced shadows on every translucent layer are unusual.
4. **Alpha testing is the known expensive case in ray tracing.** Hardware opacity micromaps (`VK_EXT_opacity_micromap`, hardware-accelerated on Ada and Blackwell GPUs) exist for exactly this.
5. **ZenEngine already tried the Godot/HDRP prepass approach.** Godot's Depth Pre-Pass and HDRP's Transparent Depth Prepass write depth for near-opaque texels and still alpha-blend the whole surface. That helps hidden layers, but the visible layer is still shaded in the forward pass. The step that matters is to move shading of the opaque part into the deferred path ([Phase 2](#phase-2--opaque-core-and-blended-fringe-largest-expected-gain)).

## 3. Plan

### Phase 0 — Attribute the 19.9 ms (required, 1–2 days)

Run everything on ZenApp with `--setup`, as AGENTS.md requires, starting from the slow center view setup (`center-sweep.zenrender.7.actual.zenrender`).

1. **Fix the profiling script.** `tools/profile_hybrid_gi.py` must find `BASE_UNLOCKED/` exports when clocks are `unaltered`; this was the export-path failure on 7 November.
2. **Capture a fresh baseline** with the current build, which includes the helper-shadow change.
3. **Read GPU Trace metrics** for the `ForwardTranslucent` range:
   - SM throughput and the warp-occupancy limiter (registers or attribute storage)
   - RT core activity
   - L1TEX and L2 hit rates
   - primitive and attribute throughput
   - ZROP and CROP throughput
4. **Add debug-only counters** (removed afterward):
   - fragments rasterized, fragments rejected at alpha 0, partial fragments, and fragments at or above the core threshold
   - shadow rays issued by `ForwardTranslucent`, by extending the existing ray-count variant (`ForwardHardwareShadowsCountsSP`)
   - mean candidate tests per shadow ray
5. **Run toggle experiments.** Run each one A vs A twice, past field readiness (frame ≥ 12). Every toggle is temporary.

   | Experiment | Change | Measures |
   |---|---|---|
   | E1 | Depth-only replay of the translucent draws | Geometry and raster floor |
   | E2 | Alpha test, then constant colour | E1 plus alpha fetch and blending |
   | E3 | Forward-pass shadows off | Forward shadow cost |
   | E4 | Forward-pass GI fallback off | Forward GI cost |
   | E5 | Leaf BLAS geometry marked opaque (wrong image) | Candidate-test cost (C6) |

6. **Time the CPU.** Time `ForwardDrawCache::Update` and the sorted-index upload during the moving phase.

**Exit:** a table that splits `ForwardTranslucent` into geometry, fragment base, GI and shadows, plus the CPU sort time. This table sets the priority of Phases 1–4.

### Phase 1 — Low-risk wins in the current path

1. **Reject alpha early.** Compute coverage first (vertex alpha × alpha factor × base-colour alpha) and reject before `SurfaceNormal`, `SurfaceBasis` and `ReadMaterialSurface`.
   - Use `demote` (`GL_EXT_demote_to_helper_invocation`) so quad derivatives stay valid for the neighbouring lanes.
   - The G-buffer masked shader already works in this order ([offscreen_surface.glsl:46-57](../Data/Shaders/SceneRenderer/offscreen_surface.glsl#L46)).
   - Expected image: identical, since these texels are already discarded.
2. **Slim the vertex outputs.** Pack one entry per distinct UV set instead of one per texture slot (22 slots). The fragment side maps each slot to its set through material data.
   - For the leaf this cuts 59 scalars to about 19.
   - The G-buffer and receiver passes use the same vertex code, so they benefit too.
   - Expected image: identical.
3. **Re-sort only when needed (stopgap).** Skip `OrderForwardDraws` when the view changed by less than an angle and distance threshold. Phase 3 replaces this.
4. **Specialise the shader.** Add a basic blended-PBR permutation, selected from material feature bits, without sheen, scatter, refraction, clearcoat, anisotropy or iridescence. Check register count and occupancy in Nsight.

### Phase 2 — Opaque core and blended fringe (largest expected gain)

For BLEND materials whose alpha is nearly binary, render the opaque part as part of the G-buffer and blend only the partial-alpha edge.

**Core:**
- Draw the BLEND triangles a second time into the G-buffer, using the existing masked shaders with a cutoff `τ_core` (≈ 0.99, tunable) on filtered coverage.
- Deferred lighting then shades them once per pixel:
  - probe-reconstructed GI instead of the voxel fallback
  - one shadow evaluation per light per pixel
  - early-Z between leaves
- They also take the indirect GPU-culled G-buffer path, batched by material.

**Fringe:**
- The existing forward pass draws only fragments with `0 < coverage < τ_core`.
- It depth-tests LESS against the core depth and writes no depth.

**Why the image stays close:**
- A BLEND fragment with alpha 1 replaces the destination, which is what an opaque fragment does. Moving it to the core only changes ordering.
- Depth resolves that ordering exactly, which the current centroid sort cannot.
- The fringe still blends in sorted order over the shaded core.
- Differences should be confined to fringe pixels and to forward-versus-deferred lighting of the same leaf, mainly the GI source.

**Why this differs from the failed prepass:** the prepass kept lighting opaque leaf texels in the forward pass. This moves that lighting out of the forward pass.

**Selection:**
- Decide per material from alpha statistics computed at import in ZenAssetProcessor: zero, one and partial fractions of base-colour alpha across mips (leaf: 93.58 / 6.18 / 0.24%), combined with vertex alpha and the alpha factor.
- Split only nearly binary materials, for example partial fraction < 2% at the mips the view actually samples. Add a runtime override in render settings.
- The authored alpha mode stays BLEND. This is a rendering strategy, not an asset change.
- Make asset processor changes in `E:\Dev\ZenAssetProcessor`, publish them, then update the submodule.

**Mip behaviour:** averaged alpha at distant mips becomes partial, which grows the fringe. Coverage-preserving alpha mips (Castaño 2010) in the asset processor keep coverage stable across mips. Measure the fringe fraction at the target view with the Phase 0 counters.

**Risks:**
- Leaves become hybrid GI receivers, which changes probe placement and `ProbeTrace` cost. GI occlusion does not change: the BLAS already treats leaves as occluders where alpha ≥ 0.5 ([material.glsl:156-160](../Data/Shaders/Common/material.glsl#L156)).
- The G-buffer gains about 1.35 M masked triangles (frustum-culled). The earlier prepass paid the same cost.
- `SceneLighting` may cost more per pixel where leaf pixels replace wall pixels, because their shadow rays cross the canopy (C6). Phase 4.1 addresses this.
- The shadow-map tier must cast the core as masked.
- `EquivalentBlendNodesPreserveSortedPixelsAndReduceNativeDraws` must still hold for decals. Add a parity test for split materials.

**Validation:**
- Run the image diff over the scene region, plus a fringe-mask diff.
- Where forward and deferred lighting of a leaf disagree, compare against the Mitsuba CUDA reference.

### Phase 3 — Sorting and culling for what remains translucent

1. **Order by cluster, not by triangle.**
   - At load, split each BLEND primitive into spatially coherent clusters of 64–128 triangles (meshoptimizer `meshopt_buildMeshlets` or `meshopt_spatialSortTriangles`). Store them in that order to keep vertex reuse.
   - Each view sorts about 10.6 k cluster keys instead of 1.36 M triangle keys. Triangle order inside a cluster stays fixed. If the image diff demands it, add 2–6 precomputed in-cluster orders selected by view direction.
   - This removes the 16 MB rebuild and upload per view and restores vertex reuse (C1, C3).
   - After Phase 2 only fringe pixels can show ordering errors, so cluster order should be enough. Decals can keep per-triangle order (8 k triangles).
2. **Drive it from the GPU.**
   - Test each cluster against the frustum and against Hi-Z built from the finished opaque depth, which exists before `ForwardTranslucent`.
   - Sort on the GPU (a radix sort in the style of AMD FidelityFX Parallel Sort) and emit indirect draws. This extends `frustum_cull.comp` and takes the sort off the CPU frame entirely (C1, C2).

### Phase 4 — Ray-traced shadows through foliage

1. **Opacity micromaps** (`VK_EXT_opacity_micromap`).
   - Bake per-triangle 4-state opacity (opaque, transparent, unknown-opaque, unknown-transparent) from the leaf alpha, in ZenAssetProcessor or at load.
   - Most candidates then resolve in hardware. Only unknown states fall back to `RayCandidateAccepted`.
   - Select it from the extension and feature query, never by device ID.
   - It benefits `SceneLighting`, `ProbeTrace`, `ProbeSpecularTrace` and `ForwardTranslucent` (C6).
   - The image is identical when the bake is conservative.
2. **Let the fringe reuse the core's shadow result.** A fringe fragment within ε of the core surface under it, on the same node, takes the core's exported visibility (`deferred_light_visibility`). This extends the existing decal path (`HybridReceiverSupports`).
3. **Skip insignificant lights on translucent layers.** In the forward pass only, skip shadow evaluation for a light whose unshadowed contribution is below a small fraction of the layer's other lighting. This changes the image, so gate it on the diff.

### Phase 5 — Cheaper lighting for real translucency

Do this only if Phases 0–4 leave the pass expensive, or when glass and particles arrive.

- **Translucency lighting volume, as in UE.** Use a camera-aligned froxel grid (for example 1/8 resolution × 64 slices) that holds shadowed direct light and GI irradiance. Each froxel takes one shadow ray per light, or reads the light-visibility cache. Translucent fragments sample the grid.
  - Lighting mode per material: Surface (current) or Volume (new).
- **Low-resolution translucency** for particle-like materials, as in HDRP and UE separate translucency. Not for foliage edges.

### Phase 6 — Geometry scale

This is a content decision; the 7 November investigation did not retain any LOD change.

- **Cypress LODs:**
  - Generate them with `meshopt_simplify` (attribute-aware) in ZenAssetProcessor, and use impostors for distant trees.
  - ZenEngine has no TAA, so use distance hysteresis instead of dithered transitions.
- **Leaf BLAS proxy:** use simplified leaf geometry for shadow rays.
- **Longer term:** cluster LOD (Nanite-like) and cluster acceleration structures.

### Not recommended now

| Technique | Why not |
|---|---|
| OIT: weighted blended, moment-based, per-pixel lists, multi-layer alpha blending | Removes sorting but not the per-layer shading that dominates. Weighted blended OIT is approximate and would change how foliage looks. Revisit for glass and particles |
| Stochastic or hashed alpha | Needs temporal accumulation; ZenEngine has no TAA, so it would be noisy |
| Alpha-to-coverage | Needs MSAA, which this pipeline does not use |

## 4. Order and expected effect

| Step | Effort | Risk | Image | Expected effect in the center view | Depends on |
|---|---|---|---|---|---|
| 0 Attribution | S | None | n/a | Sets the order below | n/a |
| 1.1 Early alpha rejection | S | Low | Identical | Texture cost of rejected fragments | E2 |
| 1.2 Slim vertex outputs | M | Low | Identical | Geometry-bound part of `ForwardTranslucent` and `OffScreen` | E1 |
| 1.3 Sort hysteresis | S | Low–medium | Brief ordering lag | Movement penalty | CPU timing |
| 2 Opaque core and blended fringe | L | Medium | Fringe and GI-source differences | **Largest.** Most per-layer forward lighting becomes one deferred evaluation per pixel; the 11.5 ms frame-level isolation result is its upper bound | 0, 1.1 |
| 3 Cluster sort and culling | M–L | Medium | Fringe order | Removes the movement penalty; culling helps partial views | 2 |
| 4.1 Opacity micromaps | M | Medium | Identical (conservative bake) | Shadow and probe ray time in every ray pass | E5 |
| 4.2 / 4.3 Shadow reuse and light cutoff | S–M | Low / medium | Small | Remaining forward shadow rays | 2 |
| 5 Lighting volume | L | Medium | Softer translucent lighting | Only if still needed | n/a |
| 6 LOD and impostors | L | Content | LOD differences | Geometry | Your decision |

## 5. Validation protocol

- **Profile ZenApp only**, with `--setup=<file.zenrender>`. Record the executable, arguments, setup and input hashes, framebuffer and render extents, fullscreen state, backend and async-compute state. Capture through `tools/profile_hybrid_gi.py`, and close RTSS before capturing.
- **Views:**
  - the slow center (−Z) view and the other seven sweep views
  - the outside Frame All view
  - a decal-heavy New Sponza main view, to protect the decal path
- **Runs:** repeat A vs A, and capture past field readiness.
- **Image checks:**
  - maximum and mean absolute difference over the scene region against the baseline
  - for Phase 2, a fringe-mask diff and the Mitsuba CUDA reference
- **Tests:** keep the blend-ordering tests and the ray-query integration tests passing, and add split-material parity.
- **Movement:** use the scripted yaw from 7 November, and report stationary and moving results separately.

## 6. Decisions confirmed on 8 November 2026

1. Phase 2's split is automatic for nearly binary BLEND materials, with a runtime override.
2. Defer Phase 6 LODs and impostors.
3. Bake opacity micromaps at load and cache them locally.

Execution evidence and outstanding work are tracked in
[TransparencyOptimizationProgress20261108.md](TransparencyOptimizationProgress20261108.md).

## References

- Unreal Engine documentation: translucent materials and lighting modes, Translucency Lighting Volume, translucency sort priority, Nanite masked materials, Lumen translucency.
- Unity HDRP documentation: Transparent Depth Prepass/Postpass, low-resolution transparency.
- Godot: [Standard Material 3D, transparency modes](https://docs.godotengine.org/en/stable/tutorials/3d/standard_material_3d.html).
- Filament: [Materials, blending and transparency](https://google.github.io/filament/Materials.html).
- Bevy: [`AlphaMode`](https://docs.rs/bevy/latest/bevy/prelude/enum.AlphaMode.html).
- glTF 2.0: [alpha coverage](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#alpha-coverage).
- Vulkan: [`VK_EXT_opacity_micromap`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_opacity_micromap.html); NVIDIA Opacity Micro-Map SDK.
- I. Castaño, [Computing Alpha Mipmaps](http://the-witness.net/news/2010/09/computing-alpha-mipmaps/), 2010.
- M. McGuire and L. Bavoil, [Weighted Blended Order-Independent Transparency](https://jcgt.org/published/0002/02/09/), JCGT 2013.
- C. Münstermann et al., Moment-Based Order-Independent Transparency, 2018.
- C. Wyman and M. McGuire, Hashed Alpha Testing, I3D 2017.
- M. Salvi and K. Vaidyanathan, Multi-Layer Alpha Blending, I3D 2014.
- P. Sander, D. Nehab and J. Barczak, Fast Triangle Reordering for Vertex Locality and Reduced Overdraw, SIGGRAPH 2007; implemented in [meshoptimizer](https://github.com/zeux/meshoptimizer).
- AMD [FidelityFX Parallel Sort](https://gpuopen.com/fidelityfx-parallel-sort/).
