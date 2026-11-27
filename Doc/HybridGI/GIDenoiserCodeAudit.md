# GI Denoiser Implementation Code Audit (2026-11-26)

Code-only inspection of the uncommitted working-tree implementation of [GIDenoiserPlan.md](GIDenoiserPlan.md) against baseline commit `94eb2c4d` (66 modified files, 27 untracked files). No source files were modified and no builds or tests were run for this audit.

## Executive summary

Why no noise improvement is observed—and why several cases regress—comes down to two layers:

1. **Activation / configuration:** The entire local-response path is **off by default** (`localHistoryResponse = false`) and has **no ZenEditor UI control**. Every camera-motion option from Stages 5–7 (`worldCacheSeed`, `probeSamplingPattern`, `glossySurfaceHistory`, `roughFootprintHistory`) also defaults to off, and Stage 9 is not implemented. Launching `zen_editor.exe` or `ZenApp.exe` without explicitly passing `--gi-history-response=local` (or editing `.zenrender`) bypasses all new denoiser code.
2. **Confirmed shader/logic defects when `localHistoryResponse` IS enabled:** Even with local response active, ten concrete bugs cause moving-light noise, glossy/rough reflection spark and pan regressions, unchanged-region age collapses on scene edits, slow recovery after light edits, and positive sky-rotation energy bias.

| # | Area | Severity | Location | Core defect |
|---:|---|---|---|---|
| 1 | Activation & UI | Blocker | `VoxelGIRenderer.h:61-66`, `VoxelGISettings.cpp:108-129`, `Data/engine.cfg:12-13`, `LightingBackendControls.cpp:393-428` | `localHistoryResponse` and Stage 5–7 flags default `false`/`0`; no ZenEditor UI toggle; `"Movable"` checkbox is a no-op while `localHistoryResponse == false`. |
| 2 | Moving lights | High | `screen_probe_moving.comp:66-68, 94` | `change` is not clamped to `1.0`; spikes to `1e3–1e6` in FP32 and decays at `0.84/frame`, pinning moving bounce to `age ≈ 1` for 50–80+ frames. |
| 3 | Moving lights | High | `screen_probe_moving.comp:62-64` | Relocation check requires exact `probe.position.xyz == PreviousProbe(...).position.xyz` (no footprint reprojection), resetting moving bounce to `age = 1` during camera motion. |
| 4 | Moving lights | High | `screen_probe_trace.glsl:513-521`, `screen_probe_moving.comp:72-84`, `screen_probe_filter.glsl:88-109, 132-135` | Raw 16-ray `predicted` delta (scaled up to `3×`) added unfiltered to `prior`; `radii.z` clamped to `2.0` when `centerLuma == 0`; `age = 1` variance is `0`, hiding noise from gather slope extrapolation. |
| 5 | World cache & moving lights | High | `world_cache_lighting.comp:37-52, 138-170, 228-232, 258-280`, `world_cache_accumulate.glsl:28-29`, `screen_probe_proposal.comp:58, 150` | Movable and camera lights are hashed into global `signature`, pinning `sourceSignature.y = 0` and `sourceSignature.w ≈ 0` every frame; breaks parent-cell inheritance, forces per-frame light refresh, and overflows the shared 32-entry `lightEdits` ring into global cache invalidation (`lookupCounts.w`). |
| 6 | Rough reflections | High | `screen_probe_prefilter.glsl:89-110, 142-146, 171`, `ScreenProbeGI.cpp:492-496` | `ScreenProbeSpecularHitsSP` takes `max` change across all 16–48 rays instead of an average/share; a single ray hitting a moving-light surface wipes all 112 GGX texels to `age ≈ 1.07`. |
| 7 | Glossy reflections | High | `GlossyReflections.cpp:197-204`, `glossy_reflection_temporal.comp:202-212`, `glossy_reflection_clamp.comp:19-45`, `glossy_reflection_trace.glsl:93` | `GlossyClampSP` runs every frame against `nextFast` (which already contains the current noisy 3-ray sample); on curved surfaces neighbor weights collapse `[lo, hi]` to `[fast_i, fast_i]` and crush `moments.z` toward `4.0`, amplifying sparks (`4.67 → 34.5–53.4` px/frame) and pan outliers. |
| 8 | Diffuse locality | High | `screen_probe_trace.glsl:592-603`, `screen_probe_integrate.glsl:53-79`, `world_cache_shade.glsl:349` | `share = max(1.0, contribution / max(prior, 1e-6))` never scales down negligible grazing/distant rays and blows up on low-PDF rays; a single ray touching a floor edit or swept AABB drops probe age from `512` to `4` (or `1.07`) and spreads via 5×5 pooling. |
| 9 | Diffuse recovery | High | `history_response.glsl:11-12`, `screen_probe_integrate.glsl:178-187, 224-230`, `screen_probe_proposal.comp:47-52` | Continuous-drift `WorldHistoryCap` + `0.957603` retention pins affected probes at `age = 4.0` for all 64 frames after a one-time light edit; `observation = batches * confidence^2` vs `previousBatches` (recorded at `confidence = 1`) stalls bounce `alpha ≈ 1e-5` whenever `age > 1` and `confidence << 1`. |
| 10 | Sky rotation | Medium | `screen_probe_trace.glsl:164, 417-423`, `screen_probe_integrate.glsl:266-287`, `world_cache_resolve.glsl:87` | Rays are importance-sampled from the **new** sky, giving tiny PDF at the **old** sun; large negative MC deltas get clipped by `max(previous + delta, 0)` while `alpha ≈ 1/256`, injecting +5–7% positive energy bias (Jensen's inequality) and 16-ray noise. |

---

## Detailed findings

### 1. Default-off settings, missing Editor UI, and inactive camera-motion stages

- **Defaults:** `ZenCore/Include/Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h:61-66` initializes:
  - `localHistoryResponse{false}`
  - `probeSamplingPattern{0}`
  - `glossySurfaceHistory{false}`
  - `roughFootprintHistory{false}`
  - `worldCacheSeed{false}`
- **Presets and `Data/engine.cfg`:** `Data/engine.cfg:12-13` sets `voxel_gi_preset=optimal` with no `voxel_gi_history_response=local`. `ApplyOptimalDefaults` (`ZenCore/Source/Graphics/RenderCore/V2/VoxelGISettings.cpp:108-129`) preserves `settings.cone.localHistoryResponse` (`false`) and ignores the Stage 5–7 flags unless `localHistoryResponse` is already `true`. `ApplyVoxelGIPreset` (`VoxelGISettings.cpp:485-536`) never enables `localHistoryResponse`.
- **ZenEditor UI:** Neither `ZenUI/ImGui/Source/LightingBackendControls.cpp:393-428` nor `ZenEditor/ImGui/Source/Panels/RenderSettingsPanel.cpp:452-530` exposes `localHistoryResponse` or any of the other new options. The only UI addition is `ImGui::Checkbox("Movable", &light.movable)` at `RenderSettingsPanel.cpp:281`, which is gated out at `ZenCore/Source/Graphics/RenderCore/V2/WorldRadianceCache.cpp:220-228` whenever `localHistoryResponse == false`.
- **Static-light camera motion:** Stages 5–7 camera-motion experiments (`worldCacheSeed`, tile/STBN `probeSamplingPattern`, `glossySurfaceHistory`, `roughFootprintHistory`) are all off by default (recorded as failing their gates in [GIDenoiserPlan.md](GIDenoiserPlan.md#L28-L31)), and Stage 9 is unimplemented. During plain camera motion with static lighting, nothing active reduces baseline diffuse probe noise.

---

### 2. Unclamped `change` locks moving-light bounce at `age = 1` for 50+ frames

**Files:** `Data/Shaders/VoxelGI/screen_probe_moving.comp:66-68, 94`

```glsl
float mean    = HybridLuminance(observed[0][3].rgb);
float change  = .5 * absoluteChange[0] / max(max(mean, stats.x), 1e-6);
float age     = WorldHistoryCap(stats.w, .266, min(16.0, stats.z + 1.0));
...
probeLighting[base + 4u] = vec4(moments, age, max(change, .84 * stats.w));
```

Unlike `screen_probe_integrate.glsl:160` (`min(1.0, ...)`), `change` is never clamped to `1.0` before being stored in FP32 `stats.w`.

- When a moving light leaves a surface, goes behind an occluder, or a probe has just relocated (`mean == 0`, `stats.x == 0`, while `delta = -before` makes `absoluteChange[0] > 0`), dividing by `1e-6` produces `change` on the order of `1e3–1e6`.
- Even with nonzero `mean`, `delta` is evaluated on 16 rays and multiplied by `batchCount` (up to `3.0`) in `screen_probe_trace.glsl:515-520`, so single-ray spikes easily yield `change >> 1`.
- Because `stats.w` decays by `0.84` per frame, a `50,000` spike stays above `1.0` for `ln(50000) / ln(1/0.84) ≈ 62` frames, pinning `WorldHistoryCap` at `age ≈ 1.0` throughout.

**Fix direction:** Clamp `change` to `[0, 1]` and normalize by `max(max(mean, stats.x), absoluteChange[0])` (or include prior magnitude before subtracting) so light disappearance produces `change <= 1.0` instead of `O(1e6)`.

---

### 3. Relocation discards moving-light history on every camera step

**File:** `Data/Shaders/VoxelGI/screen_probe_moving.comp:62-64`

```glsl
bool history = probe.source.x != 0xffffffffu && screenProbe.control.z != 0.0
            && WorldMovingHistoryComplete(ProbePastEnvironment(probe))
            && probe.position.xyz == PreviousProbe(probe.source.x).position.xyz;
```

Settled diffuse (`screen_probe_integrate.glsl:116-121`) uses `ProbeHistoryFootprint` to reproject relocated probes from coplanar neighbors. `screen_probe_moving.comp` drops history completely whenever `probe.position.xyz` changes. While walking or panning—especially with the camera-attached light ball—relocating probes reset to `age = 1` continuously.

**Fix direction:** Reproject relocated moving-light history via `ProbeHistoryFootprint` (the same way settled diffuse does), or fall back to compatible neighbor moving records rather than resetting to `age = 1`.

---

### 4. Unfiltered 16-ray moving delta injection and spatial-filter under-filtering

**Files:**
- `Data/Shaders/VoxelGI/screen_probe_trace.glsl:513-521`
- `Data/Shaders/VoxelGI/screen_probe_moving.comp:72-84`
- `Data/Shaders/VoxelGI/screen_probe_filter.glsl:88-109, 132-135`
- `Data/Shaders/VoxelGI/screen_probe_gather.glsl:247-250`

Three issues combine on the moving-light signal:

1. **Unfiltered delta injection:** Only one batch (`batch == movingBatch`, 16 rays) evaluates `MovingLighting` and `delta`, multiplying both by `batchCount` (up to `3×`). `screen_probe_moving.comp:72-84` adds the raw 16-ray `predicted[0][ray]` directly to `prior` (`mix(prior + predicted, observed, 1/age)`). Even at the 16-frame ceiling, 100% of a noisy 16-ray Monte Carlo delta enters `prior` every frame, accumulating random-walk variance on top of the 16-frame cap.
2. **Spatial filter collapses to radius 2 on dark center draws:** `screen_probe_filter.glsl:132-135` executes `if (statistics[2].x == 0.0) { radii.z = min(radii.z, 2.0); }` after overwriting `statistics[2].x` with `centerLuma` at line 98. For a compact moving light (e.g. `cameraLight` with `range = 0.12`), any probe whose 16 rays miss the lit patch gets clamped to a 2-tile spatial radius and cannot borrow smooth moving bounce from nearby lit probes.
3. **Zero reported noise at `age = 1`:** `probeLighting[base + 4u].w` stores `change` rather than within-batch ray variance, so `screen_probe_filter.glsl:88` substitutes temporal variance `max(0.0, y - x * x)`. At `age = 1`, `y == x * x`, so `gatherNoise.z == 0.0`. Lines 105–109 therefore add `0` to `gatherNoise.y` and skip `age = min(age, statistics[2].z)`, causing `screen_probe_gather.glsl:247-250` to extrapolate spatial slopes at full confidence over raw 1-frame noise.

**Fix direction:** Track within-batch variance (or a nonzero variance floor when `age <= 2`) for signal 2 so gather and filtering know its true uncertainty; avoid clamping `radii.z` to `2.0` when neighbors within a small check are lit or `age` is young; damp or pool `predicted` instead of adding raw 16-ray deltas at full weight.

---

### 5. `world_cache_lighting.comp` still hashes movable / camera lights into global `signature`

**Files:**
- `Data/Shaders/VoxelGI/world_cache_lighting.comp:37-52, 138-170, 228-232, 258-280`
- `Data/Shaders/VoxelGI/world_cache_accumulate.glsl:28-29`
- `Data/Shaders/VoxelGI/world_cache.glsl:57-60`
- `Data/Shaders/VoxelGI/screen_probe_proposal.comp:58, 150`
- `Data/Shaders/VoxelGI/world_cache_lighting.glsl:42-47`

`world_cache_lighting.comp:138-170` loops over `0..lightInfo.x` (including `cameraLight` and `movable` lights with `coneShadow.w != 0`) and hashes all of their parameters into `signature`. Whenever any movable light or camera light moves:

- `sourceSignature.y` (`stableFrames`) resets to **`0`** every frame (line 258).
- `sourceSignature.w` (`bounceConfidence`) resets to **`~0`** every frame (lines 271–280).

Downstream damage:

1. **World-cache parent inheritance disabled:** `world_cache_accumulate.glsl:28-29` requires `worldCache.schedule.x - worldCells[parent].lastSample <= min(sourceSignature.y, sourceSignature.z) + 1u`. With `sourceSignature.y == 0` (or `sourceSignature.z == 0` during animation), a new cell can only inherit from a parent sampled on the exact previous frame, breaking sparse parent warmup as the camera moves.
2. **Shared 32-slot `lightEdits` ring overflows into global cache invalidation:** Moving lights append an entry to `lightEdits[32]` every frame (`world_cache_lighting.comp:36-53, 206`). After 32 frames (< 1 s), moving-light entries evict older entries every frame. If any settled light edit was in the ring, evicting it sets `editLog.z = lost.stamp.x` (line 43), which advances `lookupCounts.w = lostFrame` (lines 228–232) and invalidates all settled world-cache cells older than that frame via `WorldHistoryUsable` (`world_cache.glsl:59`).
3. **Unnecessary per-frame proposal work and crushed confidence:** `screen_probe_proposal.comp:150` sees `sourceSignature.y == 0` and runs `ProbeRefreshLights` on every reused probe every frame; any probe with `held > 0.005` also gets `stableFrames = 0` (`screen_probe_proposal.comp:58`), `retention = 0.957603` (`screen_probe_integrate.glsl:186`), and `sourceSignature.w ≈ 0` inside `ProbeObservationConfidence` (`world_cache_lighting.glsl:45-46`).

**Fix direction:** Exclude split movable/camera lights (`worldCache.lightCount.w != 0u && light.coneShadow.w != 0.0`) from the settled `signature`, `stableFrames`, and `bounceConfidence`, and separate moving-light pose history from the settled `lightEdits` ring buffer (or track settled edits independently so moving lights cannot evict them).

---

### 6. Rough reflections wipe all 112 texels from a single ray hit

**Files:**
- `ZenCore/Source/Graphics/RenderCore/V2/ScreenProbeGI.cpp:492-496`
- `Data/Shaders/VoxelGI/screen_probe_prefilter.glsl:89-110, 142-146, 171-173`

When `HasMovingLights()` or `predictEnvironment` is true, `ScreenProbeGI` dispatches `ScreenProbeSpecularHitsSP` (`screen_probe_prefilter.glsl` without `PROBE_STATIC_HITS`):

```glsl
float change = 0.0;
for (uint j = 0u; j < sampleCount; ++j)
{
    change = max(change, response[j]);
}
...
acceptedAge = WorldHistoryCap(previous.y, .266, min(1.0 + previous.x, probe.normal.w));
```

Unlike diffuse (which averages across all rays), rough prefiltering takes the **maximum** across all 16–48 rays. If even one ray hits a surface lit by a moving light (where `delta` is scaled by up to `3×` on `movingBatch`) or grazes a surface edit, `change` hits `1.0`, dropping `acceptedAge` to `1.0 + 0.266^2 = 1.07` frames on the next frame and wiping the entire 112-texel GGX atlas of that probe every frame.

**Fix direction:** Weight per-ray specular change by each ray's energy/PDF share (or average per roughness lobe / hemisphere) rather than taking the max across all rays, and avoid letting split moving-light deltas wipe settled rough-reflection history.

---

### 7. `GlossyClampSP` destroys glossy history on curved surfaces and amplifies sparks

**Files:**
- `ZenCore/Source/Graphics/RenderCore/V2/GlossyReflections.cpp:197-204`
- `Data/Shaders/VoxelGI/glossy_reflection_temporal.comp:196-212`
- `Data/Shaders/VoxelGI/glossy_reflection_clamp.comp:19-45`
- `Data/Shaders/VoxelGI/glossy_reflection_trace.glsl:93`
- `Data/Shaders/VoxelGI/ray_hit_lighting.glsl:221-228`

In local mode (`hybrid.reflection.w == 2u`), `GlossyTemporalSP` disables its old completed-history lag check (line 208) and instead dispatches `GlossyClampSP` on every non-mirror glossy pixel every frame:

1. **Current noisy sample leaks into its own clamp:** `GlossyTemporalSP` writes `nextFast = mix(fast, current, 1 / min(age, 4))` into `glossyCurrent[i].fast`, and `GlossyClampSP` immediately clamps `slow` to the 3×3 neighborhood of `glossyCurrent[j].fast` using `moments` variance. This directly violates the invariant documented at `glossy_reflection_temporal.comp:204-206` (*"Testing the new sample against the old variance promotes a first bright Monte Carlo sample into a lighting change, shortens its history and amplifies it into a visible spark"*).
2. **Curved-surface collapse:** On curved surfaces (such as the roughness-0.18 first-floor columns, which [GIDenoiserPlan.md](GIDenoiserPlan.md#L113-L133) identified as 19,974 of 21,780 fast-pan glossy outliers and a 7–11× spark increase from `4.67` to `34.5–53.4` px/frame), `DenoiseSurfaceWeight * DenoiseLobeWeight > 0.5` rejects neighboring 2×2 glossy texels. `[lo, hi]` collapses to the single pixel `[fast_i, fast_i]`, and young/low-variance pixels hit `deviation = 0.005`, repeatedly pulling `slow` toward noisy 4-frame `fast` and slashing `moments.z` toward `4.0`.
3. **Max across 3 rays on moving lights:** `glossy_reflection_trace.glsl:93` takes `change = max(change, hitChange)` across the 3 GGX rays, so a single ray hitting a moving-light surface sets high `previousChange` and shortens glossy history on the next frame.

**Fix direction:** Do not run `GlossyClampSP` unconditionally against current-frame `fast` when neighbor support is weak (or compare against *previous* completed `fast`, require minimum neighbor count, and gate clamping on actual change evidence rather than every frame on curved motion).

---

### 8. `share = max(1.0, contribution / prior)` over-triggers resets in unchanged regions

**Files:**
- `Data/Shaders/VoxelGI/screen_probe_trace.glsl:413-416, 592-603`
- `Data/Shaders/VoxelGI/screen_probe_integrate.glsl:53-79`
- `Data/Shaders/VoxelGI/world_cache_shade.glsl:346-350`

In `ScreenProbeAccumulate`:

```glsl
float contribution = HybridLuminance(incoming) * max(dot(probe.normal.xyz, sampleRay.xyz), 0.0)
                   / max(HYBRID_PI * sampleRay.w, 1e-20);
float share            = max(1.0, contribution / max(prior, 1e-6));
response[ray][signal] += float(evidence) * share / 255.0;
```

- Because `share` is floored at **`1.0`**, a ray contributing `< 0.1%` of the probe's irradiance that hits an edited material or intersects a moved object's swept AABB (`evidence = 255`, including sky misses at line 413–416) still injects `1 / 16 = 0.0625` into `change`.
- If a single low-PDF ray has `contribution > prior`, `share` scales **up** above `1.0` and saturates `evidenceSum / (batches * 16)` to `1.0`.
- Plugging `0.0625` into `WorldHistoryCap` drops probe age from `512` to **`4.0`** (and `1.0` drops it to **`1.07`**), and `ProbeNeighborhoodChange` spreads that drop across a 5×5 probe neighborhood (40×40 pixels).
- `world_cache_shade.glsl:349` similarly forces `max(0.25, share)` on every `surfaceChange` ray into a world-cache cell.

This directly matches the measurements in [GIDenoiserPlan.md](GIDenoiserPlan.md#L93-L101) and [L309-L310](GIDenoiserPlan.md#L309-L310), where unchanged pixel `(2350, 176)` dropped from age `511` to `0.99` on the floor edit and unchanged-region RMS rose 3.3×–13.4× on floor, object, and skin edits.

**Fix direction:** Weight ray evidence by the ray's actual bounded energy share (allowing small-contribution rays to produce `share << 1` rather than flooring at `1.0`, while only using a prior-based fallback when a light/emitter *disappears* and current `contribution == 0`).

---

### 9. Continuous-drift `WorldHistoryCap` + `0.9576` retention + confidence unit mismatch stall post-edit convergence

**Files:**
- `Data/Shaders/VoxelGI/history_response.glsl:5-13`
- `Data/Shaders/VoxelGI/screen_probe_integrate.glsl:178-187, 224-230`
- `Data/Shaders/VoxelGI/world_cache_lighting.glsl:42-47`
- `Data/Shaders/VoxelGI/screen_probe_proposal.comp:46-59`

Two compounding defects hurt recovery after a discrete light edit (explaining why lamp recovery at +128 frames regressed from `0.212` to `0.315` RMS in [GIDenoiserPlan.md](GIDenoiserPlan.md#L123,L348)):

1. **64 frames pinned at `age = 4.0` after a one-time impulse:**
   - `WorldHistoryCap(change, 0.266, limit)` uses `max(4.0, pow(0.25 * (sigma/change)^2, 1/3))`—the continuous per-frame drift formula—which already hits its `4.0`-frame floor at `change ≈ 0.02`.
   - While `sourceSignature.y < 64u`, `screen_probe_integrate.glsl:186` uses `retention = 0.957603` (16-frame half-life). A one-time `1.0` edit decays only to `0.957603^64 = 0.0625` by frame 64, which keeps `WorldHistoryCap(held, 0.266)` pinned at **`age = 4.0` for all 64 frames** instead of allowing re-accumulation after the step is absorbed.
2. **Weight unit mismatch between `observation` and `previousBatches`:**
   ```glsl
   float observation = float(batches) * confidence * confidence;
   batchWeight[lane] = observation + previousBatches * ((age - 1.0) / max(statistics.z, 1.0));
   float alpha       = observation / max(batchWeight[lane], 1e-20);
   ```
   `previousBatches` was accumulated before the edit at `confidence = 1.0` (e.g., `~256`), whereas post-edit `confidence` (`ProbeObservationConfidence`) is multiplied by `sourceSignature.w` (`~0.001–0.01` right after an edit, or pinned near `0` when a moving light runs). Whenever `WorldHistoryCap` sets `age > 1.0` (e.g. `4.0`), `previousBatches * ((age - 1) / stats.z)` remains `O(1)` while `observation` is `O(1e-5)`, giving `alpha ≈ 1e-5` and locking out new bounce samples. Conversely, `screen_probe_proposal.comp:52` computes `batches / (confidence * confidence)`, inflating `acceptedBatches[1]` when `confidence` is small.

**Fix direction:** Separate one-time step decay from sustained drift (do not feed a 16-frame held impulse into the continuous-drift cube root), and keep `previousBatches` and `observation` in consistent confidence units (decaying/scaling prior weight when transitioning into low-confidence observations, or resetting `batchWeight` consistently with `age`).

---

### 10. Sky rotation introduces +5–7% positive energy bias and 16-ray noise

**Files:**
- `Data/Shaders/VoxelGI/screen_probe_trace.glsl:164, 417-423, 604-615`
- `Data/Shaders/VoxelGI/screen_probe_integrate.glsl:144-166, 266-287`
- `Data/Shaders/VoxelGI/world_cache_shade.glsl:154-192`
- `Data/Shaders/VoxelGI/world_cache_resolve.glsl:84-88`

On an environment rotation step (e.g., `0° → 45°`, intensity `1.0 → 0.5`):

- Environment rays are importance-sampled from the **new** rotated environment (`SampleEnvironmentDistribution` uses current `sceneUbo.environment.y` at `screen_probe_trace.glsl:164`). Directions pointing at the **old** sun now have a very small PDF, so occasional hits there produce huge negative Monte Carlo deltas (`current - scale * past`).
- On the edit frame, `held = pastResponse.x == 0`, so `alpha ≈ 1/256`, and `screen_probe_integrate.glsl:273-282` adds that unfiltered 16-ray `skyDelta` to `previous` and clamps negative values to zero (`max(previous.rgb, vec3(0))`, `max(previous.x, 0.0)`).
- Positive fluctuations are kept while large negative spikes are clipped at `0` (Jensen's inequality), locking +5–7% positive energy bias and 16-ray noise into mature `age = 256` history. The same `max(previous + delta, 0)` clamping occurs in `world_cache_resolve.glsl:87`.

This matches the `1.067 / 1.047 / 1.025` sky-step and `1.005 / 1.032 / 1.020` indoor-sky energy overshoots in [GIDenoiserPlan.md](GIDenoiserPlan.md#L252-L253).

**Fix direction:** Do not apply a raw low-PDF additive delta for large environment rotations without shortening `age` (or sampling a mixture of old+new sky PDFs so the negative delta has bounded variance), and avoid clamping per-frame noisy deltas into a high-age running mean.

---

## Verified non-issues (ruled out during inspection)

Checked and confirmed clean so future work does not re-investigate them:

- **Build & shader freshness:** `build/x64-windows-msvc-release/bin/ZenApp.exe` and `zen_editor.exe` post-date all C++ source files, and all `Data/SpvShaders/VoxelGI/*.spv` files post-date their `.spv.d` source/header dependencies.
- **Buffer & uniform layouts:**
  - `WorldCacheLightingBytes` (`WorldRadianceCache.h:35-37`) matches `world_cache_lighting.glsl:16-38` std430 layout exactly, and `seedCounts` sits at `WorldCacheLightingBytes - sizeof(uvec4)`.
  - `WorldCacheFrameData` (`WorldRadianceCache.h:25-33`) matches `world_surface_edits.glsl:9-18` std140 layout.
  - `ScreenProbeGI::Uniforms` (`ScreenProbeGI.h:96-110`) matches `screen_probe.glsl:8-24` std140 layout.
  - `WorldCellState` (`20` bytes, `WorldRadianceCache.cpp:18`) matches `world_cache.glsl:12-19`.
- **Probe lighting buffer stride:** `ProbeMovingBase` (`screen_probe.glsl:26-35`) appends the 5 moving-light vectors *after* `grid.w * 12`, keeping the settled stride at `12` in `screen_probe_integrate.glsl`.
- **Runtime `localResponse` toggle clear:** Toggling `localHistoryResponse` calls `InvalidateHistory("settings")` (`HybridGIRenderer.cpp:1068`), setting `reset = true` and `m_uniforms.control.y = 0`, so `ScreenProbePrefilter` overwrites `m_specular[m_current]`'s age slot before it is ever read from the 4-frame history ring.
