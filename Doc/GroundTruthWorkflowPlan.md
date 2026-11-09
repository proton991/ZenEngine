# New Sponza ground-truth comparison workflow plan

Written 8 November 2026. Scene: `E:/Dev/main_sponza/NewSponza_Main_glTF_003.gltf` (3,738,878
triangles, 401 primitives).

## Goal

1. **Build a ground-truth database once:** about 20 frames with different environment maps,
   static light setups and validated camera positions. Each frame stores a configuration that the
   engine reads to reproduce the same scene. After it is built, the database never changes.
2. **Generate the engine's output** for every frame from those configurations.
3. **Compare** engine output with the ground truth.

**Budget:** the whole process, including the first database build, finishes in under 20 minutes.
A routine run against an existing database (steps 2 and 3) should take about 5 minutes.

## What exists today

| Piece | State |
|---|---|
| `tools/ground_truth_mitsuba.py` | Mitsuba CUDA reference for the sky term at every pixel. Follows the engine's receivers: skips blend, transmission and diffuse-transmission layers. |
| `tools/ground_truth_bounce.py` | Full-image one-bounce reference, uniform albedo at secondary hits. |
| `tools/ground_truth_specular.py`, `ground_truth_arbitrate.py` | Specular lobe and disputed-pixel diagnostics. |
| `tools/compare_hybrid_gi.py`, `hybrid_capture_io.py` | Read `<prefix>.hybrid.bin` / `.lighting.json` captures and compare them. |
| Old dataset workflow | `ground_truth_sweep.py` kept an immutable sky dataset: 9 environments × 2 cameras of the old Sponza. **Deleted** in `0dc3f746` together with `capture_hybrid_gi.py`. |
| ZenApp | Has `--setup`, `--frames`, `--fixed-step`, `--capture` (image only). **No** lighting-term export and **no** multi-setup batch. The renderer still fills the capture buffers (`SetLightingCapture`, `SetHybridCapture`), but only tests read them. |
| Environment maps | 8 equirect HDRs in `Data/Textures/Environments`, plus `papermill.ktx`. |

The old workflow has three problems for this goal:
- **The reference depends on the engine.** It used the engine's G-buffer positions and its captured
  environment cube. Here the dependency must run the other way: a configuration drives both sides.
- **Its capture path is gone,** and ZenApp would need one process launch per frame.
- **Full-frame shading is too slow.** The point-sample scripts shade hits with NumPy on the CPU and
  trace every light's shadow ray. They reach about 0.23 M paths/s, so a 960 × 540 frame at 1,024
  samples per pixel would take over 30 minutes.

## Design

### What is compared

Compare **lighting terms per pixel, in linear units**, not the final tone-mapped image. A final
image mixes the BRDF model, tone mapping and exposure, so a difference there does not identify
the cause. These terms follow the existing lighting contract (`Doc/HybridGI`):

| Term | Meaning | Engine source |
|---|---|---|
| Primary surface | Position, geometric normal, primitive identity of the visible receiver | G-buffer (hybrid capture) |
| `D_direct` | Analytic light irradiance / π with visibility: Σ L·V·cos / π | **New capture component** (the existing `direct` includes the BRDF) |
| `D_sky` | Escaped environment diffuse | Lighting capture `sky` |
| `D_bounce` | One-bounce diffuse | Lighting capture `bounce` |
| `D = D_direct + D_sky + D_bounce` | Total diffuse irradiance | Derived |
| Albedo × `D` | Visual check only | Derived |

Specular (`S` and reflected radiance) is phase 2, using `ground_truth_specular.py`. Blended
layers are out of scope, as in the existing tools; their GI check needs the separate forced-core
method from `TransparencyOptimizationProgress20261108.md`.

### Frame configurations (20 frames)

**5 cameras × 4 lighting setups.** Imported New Sponza lights are all disabled: their authored
intensity is 0, the lamps sit inside opaque glass, and `HDRI_SKY` is an exporter artifact. Each
setup defines its lights explicitly.

| Lighting setup | Environment | Lights |
|---|---|---|
| L1 noon | `kloofendal_48d_partly_cloudy_puresky_1k` | High sun (directional) |
| L2 low sun | `qwantani_noon_puresky_2k`, rotated | Low, grazing sun |
| L3 sky only | `kloppenheim_06_puresky_1k` | None (environment lighting only) |
| L4 local | `small_empty_room_1_1k`, intensity 0.2 | 3 point lights in open atrium and gallery space, outside the lanterns |

| Camera | Purpose |
|---|---|
| C1 atrium floor, looking up | Sky-dominated |
| C2 ground-floor colonnade | Bounce-dominated hall, like the old `hall` view |
| C3 upper gallery, looking down | Mixed sky and bounce, long distances |
| C4 corridor under arches | Dark, bounce-only |
| C5 atrium end wall, curtains side | Large lit surfaces near the camera |

**Camera validation**, run by the database builder in the Mitsuba scene before any rendering.
Clearances are in meters, converted with the scene normalization. A candidate fails and is
reported if any check fails:
- **Not enclosed:** of 1,024 uniform sphere rays from the eye, no more than 1% hit a back face
  first, and every ray hits something or escapes to the sky.
- **Clearance:** nothing within 0.3 m of the eye, and nothing within the near plane across the
  view frustum.
- **Standing height:** for floor cameras, a downward ray hits the floor 1.2 to 2.0 m below.
- **Not facing a wall:** fewer than 10% of primary rays hit within 0.5 m.
- **Sky check:** for sky setups, at least 2% of primary rays escape, or the view is explicitly
  marked indoor.

**Configuration format.** Each frame is stored as an **engine-native `.zenrender`**, the file ZenApp
already reads with `--setup`. It holds camera, lights, environment path / rotation / intensity,
GI settings and render size. The reference renderer parses the same file, so there is one source
of truth. Dataset-only metadata goes in a sidecar `frame.json` keyed by the `.zenrender` SHA-256:
validation statistics, sample counts, seeds and file hashes. That avoids depending on the
engine's setup reader accepting unknown fields.

### Database layout and immutability

```
<GroundTruthRoot>/newsponza/v1/
  dataset.json            # frame list, scene + HDR + tool hashes, Mitsuba version, budget used
  frames/<id>/scene.zenrender
  frames/<id>/frame.json
  frames/<id>/truth.npz   # primary surface, D_direct, D_sky, D_bounce, two independent halves
```

- **Location:** outside `build/`, which gets cleaned (proposed `E:/Dev/ZenGroundTruth`,
  configurable with `ZEN_GROUND_TRUTH_ROOT`). It is too large for Git: about 60–100 MB per frame,
  1.5–2 GB in total.
- **Write once:** build into a temporary folder, validate, rename to `v1` atomically, then mark the
  files read-only. The builder refuses to write into an existing version.
- **Verified on read:** every comparison checks `dataset.json` hashes before use. A changed scene,
  HDR or reference algorithm requires a new version (`v2`); nothing is edited in place.

### Reference renderer (Mitsuba CUDA, inputs from the configuration only)

- **Fully on the GPU.** Secondary-hit albedo comes from Mitsuba bitmap textures evaluated by
  Dr.Jit, not NumPy. Alpha uses the existing contract (masked texels transparent, blend tested
  as a mask at 0.5).
- **Environment from the source HDR.** Add an equirect mode to `environment_reference.Environment`
  so the reference does not use the engine's cube. The engine's HDR-to-cube conversion is checked
  separately by an environment-only fixture.
- **Per-term estimators:**
  - `D_direct`: exact, one shadow ray per light per pixel.
  - `D_sky`: environment importance sample plus cosine sample, combined by MIS.
  - `D_bounce`: cosine sample; at the hit, one light sample chosen by power plus the environment.
- **Normals:** vertex shading normals in phase 1, captured with normal maps disabled in the engine.
  Normal-mapped references are phase 2, after a convention fixture passes.
- **Noise:** two independent halves give each pixel's noise. Comparisons subtract the noise from
  the error, and report 8 × 8 block results alongside per-pixel results.

### Engine output (ZenApp)

Add a batch mode, for example `ZenApp --ground-truth=<dataset> --output=<run>`:
1. Load New Sponza once.
2. For each frame: apply its `.zenrender` through `SceneRendering::ApplyRenderingSettings`,
   set the camera, and reset GI history.
3. Run fixed-step frames until GI has settled: at least 128 frames, past the history length (32)
   and distance-field readiness.
4. Capture linear `D_direct`, `D_sky`, `D_bounce` and the receiver G-buffer. Write them with the
   run's executable, shader and configuration hashes.

**Required engine changes:**
- the batch mode, built on existing runtime settings changes;
- a lighting-capture export for ZenApp;
- a `D_direct` irradiance capture component;
- a switch that disables normal maps for phase 1;
- a check that refuses to run while RTSS is loaded.

`Data/engine.cfg` is never edited.

**GI tiers:** hardware hybrid by default. The compute tier is optional; it roughly doubles engine
time.

### Comparison

1. **Check identity:** the same `.zenrender` hash, resolution and camera, and verified dataset
   hashes.
2. **Match surfaces:** keep pixels where the engine's and reference's primary surfaces agree
   (depth within a pixel-footprint tolerance, normals within 25°). Report the match rate.
3. **Per frame and per term:** relative bias, noise-corrected RMS, p95 error, 8 × 8 block RMS, and
   near/far splits.
4. **Output:** `report.json`, `summary.md`, and error heat maps per frame.
5. **Trend:** the first accepted run of a build is stored as that build's baseline. Later runs
   report changes against it, not pass/fail against arbitrary thresholds.

## Time budget

| Measured on 8 Nov (RTX 5080) | Value |
|---|---|
| Mitsuba: load New Sponza glTF / build scene | 1.4 s / 0.4 s |
| Mitsuba: 960 × 540 primary hits with alpha | 0.14 s |
| Mitsuba steady state: bounce ray + shadow ray, alpha-tested | **14.2 M paths/s** (256 spp at 960 × 540 in 9.3 s) |
| ZenApp: five-model assembly load to first rendered frame | about 22 s (New Sponza alone is smaller) |
| ZenApp: New Sponza GPU frame, hardware GI, 1600 × 900 | 11.4 ms (memory, 22 Oct) |

| Step | Estimate at 960 × 540 |
|---|---:|
| Validate cameras, primary surfaces | < 0.5 min |
| `D_sky` 256 spp: 133 M paths at about 14 M/s | about 10 s per frame |
| `D_bounce` 256 spp with textures, light sample and environment: about 8 M paths/s | about 17 s per frame |
| `D_direct`, exact | < 1 s per frame |
| **Database build, 20 frames** | **about 10–11 min** |
| Engine: load, then 20 × (configure + 128 frames + capture), about 3 s each | **about 1.5 min per tier** |
| Comparison (NumPy) | < 1 min |
| **First full pass / routine run** | **about 14–16 min / about 3–5 min** |

The `D_bounce` rate is an estimate and is the first thing phase 0 measures. Each tool records its
own timings in its report. If phase 0 shows the database build cannot fit, reduce sample counts
only while the measured noise stays below a quarter of the error being tested; otherwise lower the
resolution to 768 × 432.

## Implementation phases

| Phase | Work | Done when |
|---|---|---|
| 0 Feasibility | Measure GPU-only `D_bounce` with textures, light sampling and the environment on New Sponza; pick sample counts from the measured noise | Per-frame reference time ≤ 33 s with noise documented |
| 1 Configurations | Camera validator, 20 `.zenrender` frames, equirect environment in the reference | 20 frames pass validation; ZenApp opens each with `--setup` |
| 2 Reference | GPU estimators for `D_direct`, `D_sky`, `D_bounce`; immutable writer; fixtures (open plane under sky, closed box, single light) checked against analytic values | Fixtures pass; `v1` built in ≤ 12 min; hashes verify |
| 3 Engine | Batch mode, lighting and `D_direct` capture export, normal-map switch, RTSS check, convergence check (frames 112 vs 128) | 20 frames captured in ≤ 2 min per tier; static-scene frame-to-frame change below the noise |
| 4 Compare | Identity checks, surface match, metrics, reports, baseline storage | Full pass in < 20 min, routine run in ≤ 5 min |
| 5 Follow-up | Normal-mapped references, specular `S`, compute tier, forced-core leaf GI | Separate acceptance |

## Risks

- **Engine HDR conversion differs from the source HDR.** The environment fixture measures it, and
  every report includes it.
- **GI not fully settled when captured.** The convergence check compares two late frames; the
  frame count is raised until the change is below the noise.
- **Surfaces closer than 0.25 mm z-fight** (lantern and trim), so the visible surface can change
  between runs. These pixels drop out through the surface-match mask.
- **Reference and engine contracts drift apart.** Fixtures and recorded tool hashes catch this;
  any contract change requires a new dataset version.
- **Shared checkout state.** `Data/SpvShaders` is shared by every session. Run with no shader
  experiment in progress, and record shader hashes per run.

## Decisions needed

1. **Dataset location:** `E:/Dev/ZenGroundTruth` (proposed) or another folder outside `build/`.
2. **GI tiers:** hardware hybrid only (recommended for the 20-minute budget), or also the compute
   tier.
3. **Normal maps:** phase 1 without them (vertex normals), adding normal-mapped references in
   phase 5.
4. **Resolution:** 960 × 540 (proposed) or another size.
