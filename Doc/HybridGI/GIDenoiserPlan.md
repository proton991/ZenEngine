# GI denoiser plan: change-aware probe denoising (2026-11-26)

**Status, 2026-11-26.** The experimental HW RT implementation runs behind
`voxel_gi_history_response=local` (setup JSON `rendering.gi.cone.localHistoryResponse`).
The shipping default is `global`. Stage 8 requires measured recovery, cost and quality before removing that path.
Baseline measurements use the release ZenApp built from `94eb2c4d`. Each row below identifies the build or
experiment that produced it. New Sponza has no skinned animation clip, so the animation replay uses a
separate cooked asset and keeps its measurements separate from Sponza.

The current local shader set, `470a9e73ec43`, passes both independent energy gates.
Its 4K no-mover pan is 1.940 ms above the original baseline. The floor edit recovered
99.18% of held energy at observation 32, with unchanged-region RMS at 2.39× the control
and temporal deviation at 1.74×. Both locality limits are 1.10×. Stage 8 remains open.

Validation paused after the 80-frame finite-range moving-light trace on this build.
It measured 33.250 ms median and 34.110 ms p95 at 3840 × 2160 with 1,048,576 settled
cache cells. The next measurement is the matched-capacity glossy motion suite, followed
by diffuse motion, the sparse glossy reference and the 180° sweep.
[Finite-range moving-light trace](../../build/gi-denoiser-stage4/terminal-transport-trial/selected-release/trace-finite-proxy/summary.json)

### Rollout decision

| Stage | Implementation and decision |
|---|---|
| 0. Measurement | ZenApp replays absolute light, material, instance, environment and clip poses. The runner measures pre-tone-map luminance, display-space errors and probe ages against independently held references. |
| 1–3. Local history | The HW RT probe path tracks light, surface and environment edits in bounded journals, propagates cache evidence, and predicts same-ray directional changes. Large surface edits still produce excess unchanged-region error. |
| 4. Moving lights | Authored movable lights and the camera light use a separate probe signal and cache record. Two complete 16-pose cycles pass the energy-drift limit. The fixed- and moving-camera replays use an authored proxy. |
| 5. Cache prior | Implemented as `worldCacheSeed`. The young-probe RMS gate rejected it. |
| 6. Filtering and sampling | Sky, settled bounce and moving bounce have independent filter support. World-anchored sampling remains the default. The age-bin measurements below distinguish the sampling experiments. |
| 7. Reflections | Rough probes use an independent history. Glossy receivers have a separate fast-neighborhood clamp pass and an optional second reprojection candidate. The latter remains off. |
| 8–9. Release and follow-up | Promotion requires every core gate, including three paired comparisons and the 4K cost limit. Keep the global default and the evaluation switch while these gates fail. The outside-denoiser experiments depend on Stage 8. |

### Uncached transport and thin surfaces

A terminal world-cache ray that hit an illuminated surface returned zero reflected energy when
neither the fine cell nor its parent had usable lighting. Local HW feedback now evaluates direct
light at that surface and samples one additional direction for escaped or reflected radiance.
The additional hit can read either cache level or use a direct-light endpoint. This adds no
cache records or dispatches. The rough-probe filter also saves each batch's measured change
alongside its age so the next batch can use that evidence to choose its weight.

All rows below used the same 3,830 matched CUDA receivers, 4,096 frames and 4,194,304 cache
cells. The 16-vertex mean energy tolerance is ±1.491%. Each run used the default 240-frame
unused-cell lifetime except the explicitly labeled retention experiment.

| 16-vertex experiment | Mean bias | RMS |
|---|---:|---:|
| [Previous local implementation](../../build/gi-denoiser-stage4/final-checks/corrected/cuda-local-16/report.json) | −8.321% | 0.003895 |
| [Direct light on terminal misses](../../build/gi-denoiser-stage4/terminal-transport-trial/cuda16/report.json) | −3.902% | 0.003769 |
| [Direct fallback, 960-frame retention](../../build/gi-denoiser-stage4/terminal-transport-trial/cuda16-960/report.json) | −1.582% | 0.003740 |
| [Direct fallback, refresh residency on sampled reads](../../build/gi-denoiser-stage4/terminal-transport-trial/cuda16-read-residency/report.json) | −3.785% | 0.003782 |
| [Direct fallback, one in four secondary updates at the parent level](../../build/gi-denoiser-stage4/terminal-transport-trial/cuda16-sampled-parent/report.json) | −3.001% | 0.003761 |
| [Direct and one continuation on misses](../../build/gi-denoiser-stage4/terminal-transport-trial/cuda16-miss-continuation/report.json) | −0.529% | 0.003731 |
| [Local candidate with instrumented gather](../../build/gi-denoiser-stage4/terminal-transport-trial/local-16/report.json) | −0.453% | 0.003777 |
| [Verified release shaders](../../build/gi-denoiser-stage4/terminal-transport-trial/release/cuda-16/report.json) | −0.410% | 0.003746 |

At pixel (2350, 176), the floor-edit replay read roughness 0.580, full rough-probe blending
and zero glossy-reprojection support throughout. On the first edited frame the rough reflection's
red component fell from 3.061 to 0.183 while diffuse bounce stayed near 3.27. The spatial
filter treated a lighting-history reset as a newly exposed thin surface and limited its estimate
using neighboring geometry. The local implementation now applies that cross-surface limit
while the receiver itself is young. Compatible neighbors still participate in normal filtering.
[Per-signal readback](../../build/gi-denoiser-stage4/terminal-transport-trial/hotspot/material-step/signals.json)

At observation 32, the lantern pixel moved from RGB (14, 7, 2) to (94, 67, 34) with this
change. Floor unchanged-region RMS fell from 1.305 to 0.400 display levels. Sky unchanged-region
RMS moved from 0.589 to 0.565, with 0.0001% of the image over 24 levels versus 0.0018% before.
These are diagnostic trajectories with the same held references. The diagnostic shader
recorded eight probe measurements in the top row. Its binaries persisted after the source
was restored because the restored files carried older modification times. Measurements
using that build are excluded from release qualification.
[Floor comparison](../../build/gi-denoiser-stage4/terminal-transport-trial/hotspot-placement/material-step/report.json) ·
[Sky comparison](../../build/gi-denoiser-stage4/terminal-transport-trial/hotspot-placement/sky-step/report.json)

The release rebuild produced shader set `8f3516484115`. Of 806 compiled programs, 28 differed
from the preceding qualified measurements. All differences were in world-cache shading,
diffuse spatial filtering or rough-probe filtering. The release gather and luminance-capture
programs matched the original binaries. One-bounce energy bias was −1.043% against ±1.971%.
[Shader comparison](../../build/gi-denoiser-stage4/terminal-transport-trial/clean-shader-audit.json) ·
[One-bounce check](../../build/gi-denoiser-stage4/terminal-transport-trial/release/cuda-1/report.json)

At 3840 × 2160 the no-mover pan measured 31.133 ms median and 31.664 ms p95 across 80 frames.
The original median was 29.783 ms. The 1.350 ms increase exceeds the 1.0 ms allowance.
World-cache shading took 0.508 ms versus 0.493 ms on the previous local build. The capture
retained 1,048,576 settled cache cells, with hardware ray tracing, serial submission and zero
VRAM demotion. [Release trace](../../build/gi-denoiser-stage4/terminal-transport-trial/release/trace-local/summary.json)

All 20 targeted native GPU integration tests passed. On the phase-3 180° sweep the worst
frame had 2,309 pixels (0.02784%) over 24 display levels against the 0.1% limit. The sweep
used 1,024-frame warmup and pose holds. [Native integration](../../build/gi-denoiser-stage4/terminal-transport-trial/release/native.xml) ·
[Phase-3 turn](../../build/gi-denoiser-stage4/terminal-transport-trial/release/turn-sweep-180-p3/report.json)

The matched release floor edit reached 99.16% of the held affected-region HDR energy at
observation 32. Unchanged-region RMS was 0.360 display levels, or 3.299× the no-edit
control (limit 1.10×). Affected-region RMS was 0.915× global (limit 0.50×). The lantern
read (96, 68, 35) against a held (90, 64, 32). The largest remaining errors clustered
around (2492, 396), where red was nine display levels below the held image. The 100
largest errors accounted for 52.7% of the unchanged region’s total squared error. This
five-frame replay measures recovery at discrete observations. Temporal deviation requires
32 consecutive observations. [Floor replay](../../build/gi-denoiser-stage4/terminal-transport-trial/release/material-local/report.json) ·
[Pixel errors](../../build/gi-denoiser-stage4/terminal-transport-trial/release/material-local/outliers.json)

The first release reflections-only suite reported eight of fifteen comparisons on the three-material
scene. Its moving frames used 1,048,576 cells and its held references used 4,194,304 cells.
This disqualifies the suite for promotion. The runner now starts both native replays from
the same base camera, checks capacity at every scored frame and verifies each held camera matrix.
The earlier selected and observation-weight glossy comparisons both used 1,048,576 cells in
both launches. The earlier full-lighting Phase 2 diagnostic also changed capacity during motion.
[Historical capacity audit](../../build/gi-denoiser-stage4/terminal-transport-trial/motion-capacity-audit.json)

With a 512-frame common starting hold, the release build used 1,048,576 cells in all
77 captures. The three dolly poses, static sparks and frame-128 lamp recovery passed,
giving five of fifteen strict glossy checks. At fast-pan pose 64, 0.26259% of pixels differed by
over four levels against 0.07929% for retained mode. Moving sparks averaged 51.25 per
frame against 4.67. [Matched glossy suite](../../build/gi-denoiser-stage4/terminal-transport-trial/matched-glossy/report.json)

The independent sparse glossy reference gave 0.54808 RMS for local and 0.55427 for retained.
[Glossy motion results](../../build/gi-denoiser-stage4/terminal-transport-trial/release/glossy-serial/report.json) ·
[Sparse glossy reference](../../build/gi-denoiser-stage4/terminal-transport-trial/release/glossy-sparse/report.json)

The release diffuse suite passed six of fifteen strict comparisons and seven at 3% tolerance.
Cross-launch static RMS was 0.11744 against 0.17153 for retained, and moving sparks were zero
against 1.42 per frame. Lamp recovery at observation 128 was 0.31597 RMS against 0.21242.
At slow-pan pose 32, 0.09520% exceeded four display levels against 0.01264% for retained.
The native run used 4,194,304 world-cache cells and 259,200 probes.
[Diffuse motion results](../../build/gi-denoiser-stage4/terminal-transport-trial/release/diffuse-serial/report.json)

On the matched-capacity glossy run, independent CPU rays through the normalized scene
attributed 19,974 of the 21,780 four-level outliers at fast-pan pose 64 to the first-floor columns. At slow-pan
pose 32, the same material accounted for 8,628 of 10,830. It has authored roughness
0.18 in the glossy fixture. The diffuse errors at those poses were spread across the
ceiling, columns, walls, floor and doorways. These are primary-surface assignments
from the authored triangles. [Surface attribution](../../build/gi-denoiser-stage4/terminal-transport-trial/matched-surface-errors.json)

Two 80-frame 4K gather experiments increased GPU time. Checking per-signal zero flags
raised gather from 4.738 to 5.591 ms. Using raw irradiance at exactly matching receiver
normals took 7.105 ms. Their frame medians were 33.697 and 35.237 ms. Both experiments
were reverted. [Zero-signal trace](../../build/gi-denoiser-stage4/terminal-transport-trial/trace-zero-signal/summary.json) ·
[Matching-normal trace](../../build/gi-denoiser-stage4/terminal-transport-trial/trace-flat-normal/summary.json)

### Empty probes, surface edits and curved glossy receivers

Empty screen probes now skip proposal construction and clear their two diffuse signals
before temporal reduction. In consecutive 80-frame 4K traces, the candidate measured
31.784 ms median versus 32.966 ms after restoring the original shaders. The proposal
pass fell from 2.921 to 2.622 ms and diffuse temporal integration from 2.728 to 2.300 ms.
[Candidate trace](../../build/gi-denoiser-stage4/terminal-transport-trial/empty-probes/trace-candidate/summary.json) ·
[Restored trace](../../build/gi-denoiser-stage4/terminal-transport-trial/empty-probes/trace-before/summary.json)

Screen probes retain evidence from an individual surface observation with a two-frame
half-life. Light edits retain the previous sixteen-frame half-life as their changes propagate
through the cache. These diagnostic trajectories shared held poses and controls.

| Edit and observation | Unchanged RMS, previous → current | Affected RMS, previous → current |
|---|---:|---:|
| [Floor, 32](../../build/gi-denoiser-stage4/terminal-transport-trial/surface-response/shorter/material-local/report.json) | 0.360 → 0.324 | 0.681 → 0.561 |
| Floor, 128 | 0.294 → 0.270 | 0.556 → 0.405 |
| [Object, 32](../../build/gi-denoiser-stage4/terminal-transport-trial/surface-response/shorter/object-step/report.json) | 0.539 → 0.464 | 0.766 → 0.668 |
| Object, 128 | 0.459 → 0.390 | 0.631 → 0.564 |

Glossy history now carries the previous observation's response alongside roughness. The
next sample uses that response to set its weight. On curved glossy receivers, the accepted
part of the bilinear footprint retains a fraction of its observation count proportional
to the square root of its support. With identical held poses and 1,048,576 cache cells,
moving sparks fell from 51.25 to 34.5 per frame and fast-pan pose 64 fell from 0.26259%
to 0.24391% of pixels over four display levels. The retained targets are 4.67 and 0.07929%.
[Focused replay](../../build/gi-denoiser-stage4/terminal-transport-trial/column-lobe/matched-sparse-footprint/report.json)

The release build, editor and integration-test executables compile. All 22 launch tests and
20 targeted GPU tests passed. Nine out of 806 SPIR-V programs changed from shader set
`8f3516484115`, all in glossy history, gather, proposal or integration.
[Shader manifest](../../build/gi-denoiser-stage4/terminal-transport-trial/selected-release/shader-audit.json) ·
[Launch tests](../../build/gi-denoiser-stage4/terminal-transport-trial/selected-release/launch.xml) ·
[GPU tests](../../build/gi-denoiser-stage4/terminal-transport-trial/selected-release/native.xml)

Independent CUDA receiver bias was −1.044% at one vertex and −0.465% at sixteen,
within the respective 1.971% and 1.491% tolerances. The 80-frame serial HW RT pan
measured 31.723 ms median and 32.692 ms p95 at a verified 3840 × 2160 framebuffer
and render extent. Proposal took 2.615 ms, temporal integration 1.575 ms and gather
5.570 ms. The original frame median was 29.783 ms. The settled cache held 1,048,576 cells.
[One-vertex reference](../../build/gi-denoiser-stage4/terminal-transport-trial/selected-release/cuda-1/report.json) ·
[Sixteen-vertex reference](../../build/gi-denoiser-stage4/terminal-transport-trial/selected-release/cuda-16/report.json) ·
[No-mover trace](../../build/gi-denoiser-stage4/terminal-transport-trial/selected-release/trace-pan/summary.json)

With freshly held floor poses, 43,142 pixels differed by under one display level between
the settled pre-edit and post-edit images. At observation 32, unchanged RMS was 0.284
against 0.119 for the no-edit control. Affected RMS was 0.562 against 0.718 for global,
or 0.783× against a 0.50× limit. Affected HDR energy was 98.24%, 99.18% and 99.56%
of the reference at observations 8, 32 and 128. Across observations 32–63 the
unchanged-region temporal deviation was 1.742× the control.
[Floor replay](../../build/gi-denoiser-stage4/terminal-transport-trial/selected-release/material-local/report.json)

### Selected build qualification

The release ZenApp has executable SHA-256 prefix `841aa5090127`. The scene, Phase 2, sparse glossy
and CUDA results in this section used shader set `6c8559b5c0e5`. The later observation-weight
comparison identifies both shader sets below. Each paired comparison uses the same build. The diffuse suite rendered fullscreen at
3840 × 2160 with hardware ray tracing and serial submission. The historical Phase 2 configuration uses
a 1920 × 1080 retained GI buffer and a 3840 × 2160 native probe gather. Each mode supplied its own
settled image, with 4,096 warmup frames and 1,024-frame holds.

| Diffuse result | Retained | Local probes |
|---|---:|---:|
| Cross-launch static RMS, display levels | 0.17153 | 0.10525 |
| Lamp move +128 RMS, display levels | 0.21242 | 0.31503 |
| Slow pan, pose 32, pixels above four levels | 0.01263% | 0.08566% |
| Fast pan, pose 32, pixels above four levels | 0.01437% | 0.10205% |
| Moving sparks, pixels per frame | 1.42 | 0.00 |

Local probes passed 7 of the 15 diffuse comparisons, including both flicker measures, static RMS and both
spark measures. The same seven passed with the 3% allowance. Lamp recovery exceeded the absolute
0.23653 limit. The slow pan missed at all three poses. [Diffuse results](../../build/gi-denoiser-stage4/final-checks/diffuse-serial/report.json)

The sparse glossy check uses a newly cooked three-material variant of New Sponza because the archived
glossy fixture was unavailable. The floor has roughness 0.04, the first-floor columns 0.18 and the
arch stone wall 0.55. All three use metallic factor 1. Against the sparse Monte Carlo reference,
local RMS was 0.5310 display levels and retained RMS was 0.5543. The two reference captures differed
by 0.1833 RMS. [Glossy input](../../build/gi-denoiser-stage4/glossy-input/source.json) ·
[Sparse glossy results](../../build/gi-denoiser-stage4/final-checks/glossy-sparse/report.json)

On the same three-material scene, the serial reflections-only motion suite passed 5/15 strict
comparisons (6/15 with 3% tolerance). All three dolly poses passed. Both pan speeds failed at all
three poses. At fast-pan pose 64, 0.26155% of pixels exceeded four display levels versus 0.07929%
for retained mode. Moving sparks increased from 4.67 to 51.33 pixels per frame. The 128-frame
lamp recovery improved from 0.34865 to 0.17562 RMS. This run used 512-frame warmup and holds.
[Glossy motion results](../../build/gi-denoiser-stage4/final-checks/glossy-serial/report.json)

The independent CUDA check matched 3,830 receivers after 4,096 frames. One-bounce mean energy bias
was −0.999% (tolerance ±1.971%, RMS 0.002265). At 16 vertices, bias was −8.287% (tolerance
±1.491%, RMS 0.003893). The earlier global and local controls below show the same multibounce deficit
on this calibration package. [One bounce](../../build/gi-denoiser-stage4/final-checks/cuda-local-1/report.json) ·
[16 vertices](../../build/gi-denoiser-stage4/final-checks/cuda-local-16/report.json)

With both the camera and the finite proxy moving, affected RMS was 0.431×, 0.472× and 0.332× global
at frames 520, 524 and 532. The four-level outlier gate in the unchanged region failed at all four
scored poses. At frame 520, 0.828% of unchanged pixels exceeded four levels, versus 0.622% with
the same camera and the proxy disabled. At frame 524, 0.0142% exceeded 24 levels, versus 0.0093%
in that control. The full-image 0.1% limit passed throughout. [Moving camera and proxy](../../build/gi-denoiser-stage4/final-checks/remaining/column-pan/report.json)

Each fixed-camera row below uses one matched global comparison. Energy is the sum of affected-pixel
HDR luminance divided by its held reference at observations 8, 32 and 128 after the last edit.
The next two columns compare unchanged-pixel RMS at observation 32 and temporal deviation over the
following 32 samples, including observation 32, with the local no-edit control. Both limits are 1.10.
The last column compares affected-pixel RMS with global at observation 32. Its limit is 0.50.

| Edit | HDR energy at 8 / 32 / 128 | Unchanged RMS / control | Temporal deviation / control | Affected RMS / global |
|---|---:|---:|---:|---:|
| [Floor colour and emission](../../build/gi-denoiser-stage4/final-checks/material-step/report.json) | 0.984 / 0.991 / 0.996 | 5.978 | 4.076 | 1.028 |
| [Bulb colour and emission](../../build/gi-denoiser-stage4/final-checks/remaining/bulb-material/report.json) | 0.977 / 0.978 / 0.977 | 0.949 | 0.850 | 0.644 |
| [Rigid instance step](../../build/gi-denoiser-stage4/final-checks/object-step/report.json) | 0.988 / 0.990 / 0.996 | 3.856 | 6.614 | 0.891 |
| [Rigid column sweep](../../build/gi-denoiser-stage4/final-checks/remaining/column-sweep/report.json) | 0.995 / 0.992 / 0.993 | 4.425 | 7.785 | 0.896 |
| [Sky rotation and intensity step](../../build/gi-denoiser-stage4/final-checks/sky-step/report.json) | 1.067 / 1.047 / 1.025 | 5.031 | 8.008 | 2.171 |
| [Sky-only indoor rotation](../../build/gi-denoiser-stage4/final-checks/remaining/indoor-sky/report.json) | 1.005 / 1.032 / 1.020 | 6.881 | 16.174 | 1.415 |
| [Lamp colour and intensity drag](../../build/gi-denoiser-stage4/final-checks/remaining/lamp-drag/report.json) | 1.000 / 1.000 / 1.000 | 1.004 | 1.002 | 0.452 |
| [Fixed camera, moving finite proxy](../../build/gi-denoiser-stage4/final-checks/column-fixed/report.json) | 0.997 / 0.999 / 1.001 | 1.487 | 1.359 | 0.472 |
| [Second skinned clip pose](../../build/gi-denoiser-stage4/final-checks/remaining/skinned/report.json) | 1.000 / 0.997 / — | 13.411 | 19.145 | 0.884 |

Only the lamp drag met all four limits shown at observation 32. The skinned replay ends 63 frames
after its second pose change. At observation 32 that pose recovered mean energy within 0.3%, while
its unchanged pixels had 13.41 times the control RMS (0.00613 vs 0.00046 display levels).

In the floor edit, 100 of 41,551 unchanged pixels contributed 68.3% of their squared error at
observation 32. Pixel (2350, 176) went from RGB (88, 62, 31) before the change to (15, 7, 2)
at observation 32. Its edited held reference was (88, 62, 32) in the last reference sample.
The same pixel darkened to (8, 4, 1) after the rigid step and (9, 4, 1) after the sky step.
All three no-edit controls stayed within one display level of their initial colour at that pixel.
The global trajectories also darkened at the largest local outliers. Both estimators show this
transient in the same part of the image. [Outlier measurements](../../build/gi-denoiser-stage4/selected-outliers.jsonl)

With reflections and specular occlusion disabled, the same pixel at observation 32 was (21, 23, 29)
against a held mean of (40, 33, 31). Disabling indirect diffuse while retaining reflections gave
(16, 10, 7) against (83, 59, 31). Both runs used their own held poses and the same floor edit.
Reflections account for much of the transient at this pixel, and the view still misses its
unchanged-region limit with reflections disabled. [Reflections off](../../build/gi-denoiser-stage4/final-checks/response-comparison/control/reflections-off/report.json) ·
[Indirect diffuse off](../../build/gi-denoiser-stage4/final-checks/response-comparison/control/indirect-diffuse-off/report.json)

Three interleaved lamp switch-off comparisons used a shared held reference within each pair. The
median affected-region RMS was 0.117, 0.273, 0.438, 0.493 and 0.676 times global at observations
1, 8, 32, 63 and 128. Unchanged-region RMS met the 1.10 limit on all 36 scored frames. The
32-sample temporal ratio was 0.994. The last observation still exceeded the 0.50 affected-RMS
limit even though mean energy was within 0.01% of the reference. [Paired switch-off results](../../build/gi-denoiser-stage4/final-checks/remaining/lamp-pairs/report.json)

For the room-wide lamp, the complete 16-pose cycles at frames 529–544 and 1009–1024 had mean
energy ratios of 1.00162 and 1.00374 over the same 8,235,463 affected pixels. Both met the
±1% drift limit over 512 frames of motion. [Room-wide motion cycles](../../build/gi-denoiser-stage4/final-checks/remaining/room-cycle/report.json)

### Observation-weight correction

In the local diffuse and sky integrations, a current ray that saw a change could activate a
shorter history from older neighboring probes. The corrected shader consults neighbors when
the center's earlier observations provide evidence. It records new evidence for the next
observation. The release build reproduced the two tested shader binaries byte for byte.
The executable stayed `841aa5090127`; the corrected shader set is `6f5c45b177c6`.
[Qualification identity](../../build/gi-denoiser-stage4/final-checks/response-comparison/continuation-identity.json)

Four fresh control and corrected trajectories used the same archived held poses, pixel masks,
scene assets, executable and sampling phase. Values below are at observation 32. Both variants
missed the unchanged-region limit for the floor, rigid object and sky edits. The sky's energy
overshoot remained above 4%. The 20 native GPU integration tests passed across all four submission
modes on the corrected shaders. [Native tests](../../build/gi-denoiser-stage4/final-checks/response-comparison/native.xml)

| Edit | Unchanged RMS, control / corrected | Affected RMS, control / corrected | Affected HDR ratio, control / corrected |
|---|---:|---:|---:|
| [Floor](../../build/gi-denoiser-stage4/final-checks/response-comparison/independent/material-step/report.json) | 1.3015 / 1.2953 | 0.6726 / 0.6741 | 0.9915 / 0.9917 |
| [Rigid object](../../build/gi-denoiser-stage4/final-checks/response-comparison/independent/object-step/report.json) | 0.5204 / 0.5057 | 0.7746 / 0.7712 | 0.9937 / 0.9914 |
| [Sky](../../build/gi-denoiser-stage4/final-checks/response-comparison/independent/sky-step/report.json) | 0.5640 / 0.5514 | 1.4882 / 1.4769 | 1.0494 / 1.0485 |
| [Lamp drag](../../build/gi-denoiser-stage4/final-checks/response-comparison/independent/lamp-drag/report.json) | 0.1368 / 0.1364 | 0.4604 / 0.4646 | 0.9991 / 0.9995 |

At pixel (2350, 176), the corrected floor replay's captured probe age fell from 511 before the
edit to 0.99 at observation 8 and 3 at observation 32. On the sky step the same pixel still
had age 1,031 at observation 8. Reading the age alongside the colour helps distinguish local
history loss from errors that occur while the gathered probe history is mature.

The corrected shader's two-pose pan used 384-frame holds and the same per-pixel anchor age map
for both sides of each comparison. Entries are RMS divided by the corresponding anchor RMS,
listed for frames 520 and 524. The seed uses the full-lighting anchor. The two reflection
options use the reflections-only anchor.

| Option | Ages 0–7 | Ages 8–31 | Ages 32–127 | Ages 128–254 | Ages 255+ |
|---|---:|---:|---:|---:|---:|
| [World-cache seed](../../build/gi-denoiser-stage4/final-checks/remaining/footprint-seed/report.json) | 0.967 / 0.969 | 0.961 / 0.917 | 0.946 / 0.951 | 0.947 / 0.974 | 0.970 / 0.985 |
| [Rough-probe footprint](../../build/gi-denoiser-stage4/final-checks/remaining/footprint-rough-footprint/report.json) | 0.988 / 0.964 | 1.013 / 1.080 | 1.026 / 1.110 | 0.959 / 1.088 | 0.919 / 1.341 |
| [Second glossy reprojection](../../build/gi-denoiser-stage4/final-checks/remaining/footprint-dual-rough/report.json) | 0.996 / 0.992 | 1.008 / 0.995 | 1.002 / 0.998 | 1.001 / 0.999 | 0.997 / 0.998 |

The seed lowered young-probe RMS by 3.3% and 3.1%. Stage 5 requires 20%. Footprint reuse
increased mature-probe RMS by 34.1% at the second pose. The second glossy reprojection showed
mixed sub-percent differences. All three options remain off by default.

All seven native 4K turn replays passed the per-frame 0.1% limit. Each used 1,024-frame warmup
and pose holds, disjoint samples for the settled reference and its noise floor, and 1,048,576 cache cells. The percentages below
are the worst individual frames, counting pixels that differ from the settled pose by more than
24 levels in any channel. The worst observed frame among the seven was 0.02838%.

| Replay | Sampling phase | Worst frame | Pixels over 24 levels |
|---|---:|---:|---:|
| [180° sweep](../../build/gi-denoiser-stage4/final-checks/remaining/turn-sweep-180-p0/report.json) | 0 | 1028 | 0.02194% |
| [180° sweep](../../build/gi-denoiser-stage4/final-checks/remaining/turn-sweep-180-p1/report.json) | 1 | 1028 | 0.02332% |
| [180° sweep](../../build/gi-denoiser-stage4/final-checks/remaining/turn-sweep-180-p2/report.json) | 2 | 1028 | 0.02673% |
| [180° sweep](../../build/gi-denoiser-stage4/final-checks/remaining/turn-sweep-180-p3/report.json) | 3 | 1028 | 0.02838% |
| [180° sweep](../../build/gi-denoiser-stage4/final-checks/remaining/turn-sweep-180-p4/report.json) | 4 | 2056 | 0.02478% |
| [−180° sweep](../../build/gi-denoiser-stage4/final-checks/remaining/turn-sweep--180-p2/report.json) | 2 | 2057 | 0.02179% |
| [180° cut](../../build/gi-denoiser-stage4/final-checks/remaining/turn-cut-180-p3/report.json) | 3 | 1025 | 0.00855% |

The corrected diffuse suite passed 7/15 strict comparisons and 7/15 with 3% tolerance. It used
the frozen retained measurements with the identical setup, assets and executable. Of the 806
compiled shaders, only the two local probe-integration programs changed. Cross-launch static
RMS improved from 0.17153 in retained mode to 0.11096. Moving sparks went from 1.42 pixels per
frame to zero. Lamp recovery at +128 measured 0.31448 RMS against the absolute limit of 0.23653.
Slow-pan pose 32 measured 0.07678% above four levels against 0.01263% in retained mode.
[Corrected diffuse comparisons](../../build/gi-denoiser-stage4/final-checks/corrected/diffuse-serial/report.json)

The corrected CUDA comparison used the same 3,830 matched receivers on the recooked calibration
package. At one bounce the bias was −1.046% against ±1.971% (RMS 0.002252). At 16 vertices it
was −8.321% against ±1.491% (RMS 0.003895). The multibounce energy gate still fails.
[Corrected one-bounce check](../../build/gi-denoiser-stage4/final-checks/corrected/cuda-local-1/report.json) ·
[Corrected multibounce check](../../build/gi-denoiser-stage4/final-checks/corrected/cuda-local-16/report.json)

On the three-material glossy surrogate, the corrected native result measured 0.54767 RMS
against the sparse Monte Carlo reference, compared with 0.55427 for retained mode. The two
reference samples differed by 0.18334 RMS. [Corrected sparse glossy check](../../build/gi-denoiser-stage4/final-checks/corrected/glossy-sparse/report.json)

The reflections-only motion suite on that surrogate passed 8/15 strict comparisons and 8/15 with
3% tolerance. All three dolly poses passed. All six pan poses failed. At fast-pan pose 64,
0.25993% of pixels exceeded four levels, against 0.07929% in retained mode. Moving sparks were
53.42 vs 4.67 pixels per frame. The lamp's +128 RMS was 0.17754 vs 0.34865.
[Corrected glossy motion comparisons](../../build/gi-denoiser-stage4/final-checks/corrected/glossy-serial/report.json)

### Corrected 4K GPU traces

Each 80-frame Nsight trace used the same standalone executable and corrected shader set at verified
3840 × 2160 framebuffer, render and GI extents, with hardware ray tracing and async compute off.
The moving runs use the same camera pan and lamp orbit in both modes.

| Setup | Median / p95 GPU frame (ms) | Probe trace / hit shading (ms) | Committed VRAM (MiB) |
|---|---:|---:|---:|
| [Local, no mover](../../build/gi-denoiser-stage4/trace-selected-local-retry/summary.json) | 31.248 / 32.096 | 1.463 / 3.425 | 11,736 |
| [Global, no mover](../../build/gi-denoiser-stage4/trace-selected-global/summary.json) | 32.180 / 32.792 | 1.458 / 3.412 | 11,734 |
| [Global, range 0.12](../../build/gi-denoiser-stage4/trace-selected-finite-proxy-global/summary.json) | 31.287 / 31.914 | 1.455 / 3.392 | 11,734 |
| [Local, range 0.12](../../build/gi-denoiser-stage4/trace-selected-finite-proxy/summary.json) | 32.939 / 34.023 | 1.491 / 4.079 | 11,680 |
| [Global, range 1000](../../build/gi-denoiser-stage4/trace-selected-room-wide-global/summary.json) | 31.065 / 31.924 | 1.455 / 3.351 | 11,734 |
| [Local, range 1000](../../build/gi-denoiser-stage4/trace-selected-room-wide/summary.json) | 33.968 / 34.999 | 1.461 / 4.674 | 11,680 |

The no-mover local pan cost 1.465 ms more than the original 29.783 ms baseline, exceeding the
+1.0 ms budget. Its first capture overlapped the shader rebuild. The table uses the repeated
capture whose input hashes matched throughout. Local mode added 1.651 ms with the finite lamp and 2.904 ms with the room-wide lamp against their
matched global modes. Probe tracing showed no speedup. Each run retained 1,048,576 settled cache
cells and reported zero bytes of VRAM demotion. The trace archives retain the original capture,
per-frame and per-pass timings, launch commands and hashed input manifests. The large intermediate
regime tables can be regenerated from the original captures.

### Earlier renderer qualification

The release executable for the following runs has SHA-256 prefix `b898d0a35e0e`, and its compiled shader
set has prefix `960e0af0310d`. Each linked report records the full hashes and effective settings.
Superseded trials retain their reports, run commands, input identities and representative frames.
Recomputing every image metric for those trials requires replaying their recorded commands.

| Check | Measured result | Decision |
|---|---|---|
| Native GPU integration | 20/20 across the four submission modes. Covers unannounced light off/on, cache sample weighting, directional resolve and expiry, material/visibility shading and pass graphs. | Pass. [Native results](../../build/gi-denoiser-stage4/qualified-final-native.xml) |
| CUDA diffuse energy, one bounce | 3,830 matched receivers, relative bias −1.024%, 3σ energy tolerance ±1.971%, RMS 0.002261. | Pass. [One-bounce report](../../build/gi-denoiser-stage4/cuda-1/report.json) |
| CUDA diffuse energy, 16 vertices | Same 3,830 receivers, bias −8.261%, tolerance ±1.491%, RMS 0.003892. | Fail. [Multibounce report](../../build/gi-denoiser-stage4/cuda-16/report.json) |
| Full-lighting New Sponza, 512-frame warmup and holds, async compute on | 6/15 comparisons within 3% of matched retained mode. Static RMS 0.6552 vs 0.3019. Lamp +128 RMS 0.5126 vs 0.3778. Both pan speeds failed at all three poses. Static sparks were zero for both modes. Moving sparks fell from 9.67 to 1.83 pixels/frame. | Diagnostic with reflections and specular occlusion enabled. [Full-lighting report](../../build/gi-denoiser-stage4/phase2/report.json) |
| Reflections-only New Sponza, original materials, 512-frame warmup and holds, async compute on | 14/15 strict comparisons. At slow-pan pose 32, 0.01925% of pixels exceeded 4/255 against 0.01447% in retained mode. Static RMS was 0.1092 vs 0.1254. | Slow-pan pose 32 exceeds the motion limit. [Reflections report](../../build/gi-denoiser-stage4/glossy/report.json) |

The CUDA calibration uses the archived reference and matching uniform-gray Sponza geometry. The original cooked
package was unavailable. Recooking removed unused texture tables from the source while preserving its geometry
buffer. These captures rendered 3840 × 2160 with a 1280 × 720 framebuffer, 4,096 frames and 4,194,304 cache cells.
Under global history on the same package, reference, executable and cache size, one-bounce bias was
−1.032% and 16-vertex bias was −8.268%. Local history changed the latter by +0.0064 percentage
points. Both modes miss the independent multibounce energy limit. The historical 16-vertex result
used 8,388,608 cells. [Global one-bounce](../../build/gi-denoiser-stage4/cuda-global-1/report.json) ·
[Global multibounce](../../build/gi-denoiser-stage4/cuda-global-16/report.json) · [Calibration input](../../build/gi-denoiser-stage4/calibration-input/source.json)

The fixed-camera step replays below used independent 512-frame holds. Energy is the sum of
pre-display luminance over affected pixels divided by that of the held image. Unchanged RMS is
measured in display levels against the same-pose control. These are single comparisons.

| Edit | Energy, observations 8 / 32 / 128 | Unchanged RMS / control at 32 | Result |
|---|---:|---:|---|
| Floor colour and emission | 0.9895 / 0.9971 / 1.0013 | 1.606 / 0.102 (15.75×) | Energy recovered. The unchanged region failed. [Replay](../../build/gi-denoiser-stage4/material-step-qualified-final/report.json) |
| Rigid instance step | 0.9915 / 1.0015 / 1.0091 | 0.594 / 0.169 (3.52×) | Energy recovered. The unchanged region failed. [Replay](../../build/gi-denoiser-stage4/object-step-qualified-final/report.json) |
| Sky rotation and intensity step | 1.0690 / 1.0486 / 1.0270 | 4.51× | The 32-frame energy and unchanged-region limits failed. [Replay](../../build/gi-denoiser-stage4/environment-step-qualified-final/report.json) |

In the floor edit, 42,917 pixels qualified as unchanged. At observation 32, the 100 pixels with
the largest errors accounted for about 68% of their squared error. Two independently sampled
held means differed by 0.105 RMS in that region. At pixel (2350, 176), pre-display luminance was
0.00273 on the edit trajectory and 0.0604 in the settled image. Both values came from native HDR
captures at the same camera pose. [Held-reference diagnostics](../../build/gi-denoiser-stage4/unchanged-reference-residuals.json)

A trial bounded the change passed to a secondary cell by the fraction of upstream luminance
reflected at its sampled surface. Applying that bound immediately after an analytic light switched
off left 6.8% of its previous cache energy at the fixture's 80-frame checkpoint. The limit was 5%.
All four submission modes caught this regression. The previous source energy can disappear from
the current sample when a lamp turns off. [Unconditional-bound trial](../../build/gi-denoiser-stage4/unconditional-optional-native.xml)

The selected implementation limits the response propagated by a secondary surface to the fraction
of incoming energy it reflects. For one history window after a settled analytic light or the sky
changes, it uses the upstream response directly. This preserves the response to energy that vanishes
when a source dims. The selected build passed all 20 native GPU checks across four submission modes
and all 22 runtime launch tests. [GPU tests](../../build/gi-denoiser-stage4/selected-native.xml) ·
[Launch tests](../../build/gi-denoiser-stage4/selected-launch.xml)

### Earlier matched 4K GPU traces

Each row covers 80 frames of the same fullscreen camera pan with base GPU clocks and serial submission.
The moving rows use an 80-frame authored lamp orbit. Trace and shade are median GPU pass times.
Every trace retained 1,048,576 cells with zero VRAM demotion.

| Setup | Frame median / p95 (ms) | Trace hits / shade hits (ms) | Committed VRAM (MiB) |
|---|---:|---:|---:|
| [Original baseline, no mover](../../build/gi-denoiser-stage0/pan/summary.json) | 29.783 / 30.754 | 1.452 / 3.210 | 11,681 |
| [Local, no mover](../../build/gi-denoiser-stage4/pan-qualified-final/summary.json) | 33.301 / 34.003 | 1.478 / 3.448 | 11,736 |
| [Global, range 0.12](../../build/gi-denoiser-stage4/finite-proxy-global-qualified-final/summary.json) | 32.150 / 32.730 | 1.456 / 3.397 | 11,734 |
| [Local, range 0.12](../../build/gi-denoiser-stage4/finite-proxy-qualified-final/summary.json) | 32.478 / 32.952 | 1.461 / 4.072 | 11,680 |
| [Global, range 1000](../../build/gi-denoiser-stage4/room-wide-global-qualified-final/summary.json) | 31.962 / 32.494 | 1.455 / 3.401 | 11,734 |
| [Local, range 1000](../../build/gi-denoiser-stage4/room-wide-qualified-final/summary.json) | 34.296 / 35.286 | 1.460 / 4.655 | 11,680 |

The no-mover median exceeded the original baseline by 3.519 ms. The local finite-range orbit cost
0.328 ms more than its matched global orbit. With the room-wide lamp that difference grew to
2.335 ms. The probe hit-shading pass accounted for 0.675 and 1.254 ms respectively. The targeted
probe-tracing speedup and the overall 1.0 ms budget both failed on this build.

### Measured checkpoints

| Canonical 3840 × 2160 New Sponza run | Baseline | Local candidate | Decision |
|---|---:|---:|---|
| 80-frame pan, base GPU clocks, async compute off, median GPU frame | 29.783 ms | 33.763 ms (first implementation) | +3.980 ms exceeds the +1.0 ms budget. Temporal integration accounts for 2.722 ms of the increase. |
| Settled world-cache capacity on that pan | 1,048,576 cells | 1,048,576 cells | Same settled allocation |
| Lamp 1 off, edit-frame RMS in unchanged pixels | 2.959 | 0.261 | Local no-edit control was 0.244. One paired launch is within 1.10×. |
| Same frame, RMS in affected pixels | 3.898 | 0.645 | One paired launch is below half the baseline. |
| Frame 32 after toggle, affected mean display RGB / held reference | — | 0.9729 | Outside the 2% recovery band. Display RGB is a diagnostic. The linear-energy gate needs linear samples. |

The first lamp pilot used a fixed camera, four held display images per reference and a fully serialized setup. The two
reference poses in this pilot were held consecutively in one launch. Different cache ages and using the
same reference for mask selection and scoring can bias the result. Acceptance runs need independent mask
samples and equal warmup for both poses. The three runs (edit, control, held) comprise **one comparison**.
Three independent interleaved comparisons are required for sign-off. The paired lamp result below uses that procedure. The probe-path frame summaries showed
a zero radiance generation and the requested local mode. The camera light was disabled.

The offline RGB step, equal-luminance colour edit, shadow snap and 512-frame symmetric motion cases run with
`tools/gi_history_response.py`. The predicted motion case had 5.75% RMS of peak and 0.0019% mean energy
error. Step bias at observation 32 was under 0.23% of peak in all simulated step cases. These are estimator
checks. The renderer has visibility, cache latency and nonlinear display mapping absent from the simulation.

Local run evidence lives under `build/gi-denoiser-stage0` (baseline pan, lamp reports, simulation) and
`build/gi-denoiser-stage1/pan` (initial local pan). Each trace directory contains its inputs, exact ZenApp
command, actual extents, backend, cache allocations and per-pass timing samples.

### Follow-up runs

| Measurement | Result |
|---|---|
| Same pan after splitting sky prediction | 33.173 ms median. Probe temporal overhead fell from 2.722 to 1.647 ms. |
| Same pan after restoring a fixed stride for settled histories and specializing the rough-reflection shader | 31.133 ms median and 31.842 ms p95. The median is still 1.350 ms over baseline. Probe temporal and prefilter overheads fell to 0.470 and 0.068 ms. |
| 180° sweep and return, Stage 5 prior enabled | In each of the 12 turn steps, over 97% of the new anchors had an exact warm seed. The least-covered step had 56,186 exact and 16 parent hits among 57,548 new probes. A warm cache warrants the image-error experiment. |
| New lamp toggle pilot, 1,024-frame independent warmups and disjoint reference/mask samples | Observation 32 had affected RMS 0.453 and mean display ratio 0.99295. Unaffected RMS was 0.1767 against 0.1314 for the no-edit control. The unrelated no-edit preframe also differed across launches. |
| Combined 45° environment rotation and 50% intensity step | Mean pre-display HDR luminance was 1.09× the held reference at observation 8 and 1.05× at 32. The 32-frame energy gate failed. Unaffected-region RMS was 4.16× the control at 32. |
| Native unannounced directional light off/on fixture | The global control passed. The first local versions overshot on switch-on. Holding measured change over several observations and pooling previous-frame evidence reduced that probe overshoot. With a two-observation cell residual half-life the original energy and stability assertions passed in all four submission modes, in both local and global configurations. |
| Combined sky edit with factored intensity and same-ray cache prediction | At observations 1, 8, 32 and 128 the affected linear energy ratios were 1.0911, 1.0612, 1.0334 and 1.0090. The 8-frame target passes in this pilot. The 32-frame target and unaffected-region RMS still fail. |
| Combined sky edit after separating current radiance from its own history weight | Affected linear energy was 1.0673× / 1.0236× / 1.0077× the held reference at observations 8 / 32 / 128. At 32, unaffected RMS was 0.516 against 0.133 for the control. |
| Room-wide authored lamp, 512 frames of symmetric motion | At frames 1008 and 1016 the affected linear energy ratios were 1.0291 and 1.0151. The corresponding unaffected RMS ratios were 4.65 and 4.99. The settled world cache stayed at 1,048,576 cells. The two motion samples do not establish the full-cycle drift. |
| Moving lamp, full symmetric cycles near the start and end of 512 motion frames | The 16-pose mean linear energy / held reference was 1.00154 at frames 529–544 and 1.00378 at frames 1009–1024. Both pass ±1%. Each cycle used the same union of 8,234,254 affected pixels and a separate held image for each distinct pose. [Cycle report](../../build/gi-denoiser-stage4/room-cycle/report.json) |
| Material 19 colour/emission step, observations 8 / 32 / 128 | Affected HDR energy ratios were 0.9939 / 0.9975 / 1.0000. Unaffected RMS ratios were 3.82 / 12.72 / 11.60. The mask covered 42,925 pixels of the 4K frame. |
| Rigid instance 5 step, observations 8 / 32 / 128 | Affected HDR energy ratios were 0.9923 / 0.9964 / 1.0027. Unaffected RMS ratios were 4.61 / 4.75 / 4.97. |
| No-mover pan with workgroup-staged gather coefficients | 31.963 ms. Gather rose to 5.864 ms and the change was removed. |
| No-mover pan after rejecting taps outside cubic support before geometric evaluation | 30.749 ms median, 31.312 ms p95. One 80-frame median was +0.966 ms against baseline. Gather took 4.369 ms, versus 4.290 ms at baseline. Committed VRAM was 11,415 MiB, versus 11,681 MiB at baseline. Both retained 1,048,576 cells. |
| Moving authored lamps on the same pan | Range 0.12 took 32.704 ms and range 1000 took 33.207 ms. Both used a single live direct-light batch per probe ray. Earlier range 0.6 timings with the workgroup-staged gather used a different build. |

On the later no-mover pan, the 80-frame median was 31.354 ms and p95 was 32.036 ms.
The median exceeded the baseline by 1.571 ms. The 30.749 ms measurement above was one run.
[No-mover trace summary](../../build/gi-denoiser-stage4/pan-qualified/summary.json)

The broad floor-material edit affects almost the entire frame. After suppressing unchanged caster
bounds and stale cache observations, only 35,062 of 8,294,400 pixels met the unchanged-mask definition.
At observation 32 their RMS was 1.543 display levels against 0.102 for the control. The preceding
run measured 1.291 against 0.373. Both fail 1.10×. The corresponding affected energy ratios
were 0.99589 and 0.99655. The control variance makes the absolute errors important when comparing revisions.
[Floor material replay](../../build/gi-denoiser-stage4/material-step-observations/report.json)

CesiumMan supplies the skinned test. Both steps recovered affected linear energy to within 1% by
observation 8. Unchanged-region RMS was 5.68× and 11.11× control at observation 32. The same run
verified that an opaque-only scene could deliver the pre-display luminance capture.
[Skinned pose replay](../../build/gi-denoiser-stage4/skin-hdr/report.json)

### Paired lamp toggle

Three interleaved global/local comparisons switched off the same authored lamp at frame 1025. Both
ends of each pair use the candidate's held scene poses and identical affected mask. Each reference was
warmed independently for 1,024 frames. The camera was fixed and its attached light was disabled.

| Observation after switch-off | Unaffected RMS / no-edit | Affected RMS / global | Affected linear energy / held |
|---|---:|---:|---:|
| 1 | 1.011 | 0.129 | 0.99984 |
| 8 | 1.012 | 0.290 | 0.99983 |
| 32 | 1.008 | 0.471 | 0.99990 |
| 63 | 1.004 | 0.527 | 0.99996 |
| 128 | 1.008 | 0.704 | 1.00000 |

The median 32-frame temporal deviation ratio was 0.992. The affected-area RMS gate passed
at observations 1, 8 and 32, then failed at 63 and 128 as global mode recovered. The 8/32
energy, far-field and large-outlier gates passed at those checkpoints.
[Paired report](../../build/gi-denoiser-stage4/lamp-triplicate/report.json)

### Camera-motion experiments

In a one-degree-per-frame pan, score frames 520 and 524 against independently held 384-frame poses.
Within each row, both alternatives use the same reference and the anchor run's per-pixel probe age.
The age 0–7 populations are 1,254,204 and 1,699,698 pixels. A ratio below one favors the candidate.

| Candidate relative to anchored sampling | Age 0–7 RMS ratio at 520 / 524 | Decision |
|---|---:|---|
| World-cache prior | 0.9707 / 1.0038 | Fails the 20% gate despite high warm-cache coverage. Remains off. |
| Screen-tile Sobol rotation | 0.9757 / 0.9800 | Small improvement. Age 128–254 worsens by 2.7% at frame 524. Retain anchored sampling. |
| Retained birth-tile Sobol rotation | 0.9813 / 0.9787 | Age 8–31 worsens by 3.5% at frame 524. Retain anchored sampling. |
| Additional glossy surface reprojection | 1.0054 / 0.9975 | Mixed full-image results. The setting remains off. |

The tile patterns in this table use the engine's Owen-scrambled Sobol digital shifts.
A separate experiment generates a 16 × 16 × 64 scalar spatiotemporal blue-noise mask using
void-and-cluster energy on each spatial slice and each temporal column. A tile's 64 ranks order
a complete two-dimensional Sobol net within a held block. Current-tile indexing can change order
during reprojection. The retained variant follows the anchor's birth tile. The mean spectral power in the
first two spatial frequency rings was 0.0048× the higher-frequency mean. Temporal bins 1–8 were 0.0314× bins
9–32. These are properties of the scalar mask. Per-probe ray proposals and projection still determine image error.
[Mask generator](../../tools/gi_probe_stbn.py) · [Wolfe et al., EGSR 2022](https://research.nvidia.com/publication/2022-07_spatiotemporal-blue-noise-masks)

| STBN experiment | Age 0–7 RMS / anchor, frames 520 / 524 | Age 8–31 RMS / anchor | Mean held temporal deviation, levels |
|---|---:|---:|---:|
| World-anchored Sobol | 1.0000 / 1.0000 | 1.0000 / 1.0000 | 0.0327 / 0.0324 |
| Current tile | 0.9906 / 1.0142 | 1.1340 / 1.0359 | 0.0528 / 0.0532 |
| Retained birth tile | 0.9923 / 1.0101 | 1.1016 / 1.0303 | 0.0527 / 0.0532 |

Held deviation is the spatial mean of the per-pixel temporal standard deviation of the brightest
display channel, using eight held samples 16 frames apart. The p99.9 value was 0.50 levels throughout.
Both STBN variants increased mean flicker and worsened ages 8–31. Keep world-anchored Sobol as the default.
[Age and flicker measurements](../../build/gi-denoiser-stage4/stbn-held-flicker.json)

The follow-up trace folders, pose references and JSON reports are under `build/gi-denoiser-stage4`.
The local implementation uses bounded GPU edit journals and per-material CPU revision stamps. It compares
committed shadow-caster revisions and unions old and new bounds so deformation can invalidate paths with
unchanged model matrices. A separate committed visibility revision distinguishes changes in ray coverage from changes in raster
shadow appearance. Material colour or emission edits identify their material without adding an unchanged
mesh's entire bounding box to the swept geometry list. The optional moving and seed records are allocated independently. Current and
birth-tile Sobol sampling experiments use the engine's existing spatial Sobol rotations; a retained birth tile keeps its
digital shift across reprojection. The stage selection criteria below still apply.

### Replaying scene changes

Pass `--scene-path=<file.json>` alongside `--setup` and an optional `--camera-path`. A document selects
one target and provides ascending, positive scene-frame keys. ZenApp holds the first key up to its frame
and holds each pose until the next key. Coordinates use the runtime scene space. The existing
`--light-path` form with `lightId` and position-only keys is also supported.

| Target | Selector | Required pose fields in addition to `frame` |
|---|---|---|
| `light` | Authored `lightId` | `position`, linear RGB `color`, `intensity`, `enabled` |
| `cameraLight` | Camera light | Absolute `position`, optional `enabled` (defaults to true) |
| `environment` | Active sky | `intensity`, `rotationDegrees` |
| `instance` | Renderable `index` | Absolute world `position` |
| `material` | Material `index` | Linear RGB `color` and `emission` |
| `animation` | Setup's active clip | Absolute clip time in `seconds` |

`tools/gi_scene_replay.py --setup ... --scene-path ... --frames 256,257,264,288,384 --output ...`
captures the edit and camera-matched no-edit control. Supply explicit camera keys on scored frames when the
camera moves. `--linear` writes FP32 pre-display scene luminance and the age of the contributing probes. The scene colour measurement precedes display-space transparency blending. `--temporal-start` captures 32 consecutive frames at one pose. `--baseline` compares against the same mask and held reference. Frames 257, 264 and 288 are observations 1, 8 and 32 for an edit at frame 257.

Generate the local and global setups, authored scene paths and matched pan from a saved New Sponza view.
The generated lamp paths use its light ID 1. Use the resulting files with the replay and profiling commands.

```powershell
python tools/gi_scene_scenarios.py --setup sponza.zenrender --output replay-inputs
python tools/gi_scene_replay.py --setup replay-inputs/local.zenrender --compare-setup replay-inputs/global.zenrender --scene-path replay-inputs/lamp-step.json --frames 1024,1025,1032,1056,1087,1152 --hold 1024 --temporal-start 1056 --linear --output lamp-pairs
python tools/gi_scene_replay.py --setup replay-inputs/room-wide.zenrender --scene-path replay-inputs/room-cycle.json --frames 512 --cycle 529:544 --cycle 1009:1024 --hold 512 --linear --output lamp-cycles
```

**Scope:** the HW RT screen-probe path: sky, diffuse bounce, the rough-reflection atlas, glossy reflections
and the world cache. Compute shares some of these shaders but is not a target (HW RT is the current focus).

**Origin:** the user saw whole-screen noise with the light ball on, confirmed it, and asked that it not be
captured again. That cause, and the other causes below, come from reading the source. Stage 0 measures each
one before anything is replaced.

| Replay input | Authored pose sequence |
|---|---|
| Lamp toggle / drag | Switch off setup light ID 1 at frame 1025. For the drag, use 16 equally spaced updates, ending at twice its initial intensity and RGB (.5, .9, .95). |
| Column and proxy | Move light ID 1 from x=.024 to .104 over 16 equal updates, starting at frame 513. Hold y=−.202 and z=−.148, colour (1, .9, .7), intensity .003 and range .12. The camera-only control disables this light. |
| Symmetric lamp | Start at (−.02, −.15, −.12). From frame 513 move x to .18 and back in .025 increments. Repeat the 16-frame triangular cycle 32 times. |
| Sky step / indoor rotation | Step from intensity 1 and 0° to .5 and 45°. For indoor rotation, disable the authored lights and analytic and emissive GI. Advance one degree per frame for 45 frames and lower intensity linearly to .5. |
| Rigid motion | For the step, move renderable 5 from (0, 0, 0) to (.08, 0, .06). For the continuous sweep, start it at (.178, −.243, −.069) and increase z by .0025 on each of 48 frames. |
| Floor / lamp-bulb material | Material 19 changes from white with zero emission to RGB (.35, .8, .45) and emission (.025, .075, .025). Material 12 changes from white with unit emission to RGB (.45, .85, .55) and emission (.25, .75, .4). |
| Skin | Hold clip time at 0, .3 and .65 seconds from frames 1, 257 and 321 of the CesiumMan replay. |

The 4K trace uses an 80-frame periodic pan, base GPU clocks and async compute disabled.
For the final moving-light profiles, both the camera and light repeat every 80 frames. Lamp 1 follows
`(.08 + .12 sin(θ), -.15, -.03 - .09 cos(θ))` with intensity .003, colour (1, .9, .7), shadows on
and range .12 or 1000. A full capture covers one joint period regardless of the profiler's start frame.
The earlier 64-frame lamp trajectory remains identified by the copied scene path in each trace.

## Summary

Two mechanisms contribute to the observed noise.

1. **Frame-wide history response.** At the baseline revision, changes to lighting, environment, geometry
   or materials can shorten history across the image. A small edit can therefore disturb receivers far
   from its physical influence. A skinned object triggers this path when its committed geometry changes.
2. **New probes with no history.** Newly exposed receivers initially have few ray samples. The pan and
   turn replays score these receivers by probe age against a held reference at the same camera pose.

**The plan applies one set of rules to every history:**

- Use authored source changes, visibility changes and common-ray samples to decide where to shorten
  history. These observations still sample a finite set of directions.
- Propagate per-cell change and transition staleness out of `WorldLookup` into screen probes and glossy hits,
  and catch up sparse cells and multi-frame probe reprojections against their last-updated frame (`lastSample`)
  rather than diffing only adjacent frames.
- Where it changed, shorten history to minimize lag plus noise: a cube-root cap for continuous drift, and a
  step rule (allowing `N` down to `1` or exponential weight decay) for discrete/unexplained jumps so recovery
  clears the 8- and 32-frame gates.
- Where the change can be computed on the same rays, adjust history with an **additive correlated delta**
  (`ΔSH`), reserving exact uniform scaling (`I_new / I_old` from uniforms) strictly for pure global intensity
  changes.
- Keep moving lights in their own short-lived signal (excluding them from `WorldLevel` cache refinement),
  give probes born without history a world-cache prior, and refine spatial filtering and sampling.
- Gate camera-motion options outside the core denoiser (Stage 9, optional follow-up: extra probes on young
  tiles, denser grid, short pixel pass, temporal upscaler, or random one-probe gather with a pixel pass)
  behind the core denoiser landing and passing profiling.

**Hypotheses to test**

- **Lighting, environment, animated objects and material edits:** the unchanged-region error should fall if those receivers retain their histories. Locality, convergence and timing require independent measurements for each kind of edit.
- **Camera motion:** modest and experimental inside the denoiser, since two earlier ideas of the same kind
  failed here. Denser probes are the one lever already measured to help, at a cost. Each camera-motion stage
  has a stop rule, and Stage 9 decides outside-denoiser options by measurement after the core lands.
- **Camera cuts, scene loads and settings changes:** unchanged. They still reset, which is correct.

**Cost target:** at most +1.0 ms of total denoising, change-evidence and extra-batch overhead at 4K against
the `94eb2c4d` baseline (checked with Nsight at Stages 0, 1, 4 and 8), plus whatever Stage 9 adopts. Probe
tracing during local changes should be substantially cheaper than today's global 3× ray blow-up, and
persistent VRAM growth must not halve `WorldRadianceCache` cell capacity.

## Baseline behavior at `94eb2c4d`

### Global resets and couplings

| Trigger | What resets or breaks | Where |
|---|---|---|
| A light moves, including the light ball | Bounce history of every probe restarts. Only sky has a distance rule (`lane == 0u`), and its edit box grows during continuous motion. Glossy and rough-reflection histories restart. Cache cells drop from 128 updates to about 16. Moving `cameraLight` also shifts `WorldLevel` cache levels around it (`localLights[]`), invalidating settled cache cells; and `probeCDF`'s 64-frame reuse freeze delays light-guiding toward newly approaching lights | `screen_probe_integrate.comp:101-115`, `HybridGIRenderer.cpp:1111-1118`, `glossy_reflection_temporal.comp:29-30`, `screen_probe_prefilter.glsl:127-131`, `world_cache_lighting.comp:72-81`, `world_cache_resolve.glsl:37-41`, `world_cache.glsl:33-51`, `WorldRadianceCache.cpp:183-190`, `screen_probe_proposal.comp:69-85` |
| A light's intensity or colour changes, or a light turns on or off | All of the above, plus sky restarts and every cache cell is unreadable until resampled | `world_cache_lighting.comp:65-71`, `world_cache.glsl:59-61` |
| Any object moves or animates | Every probe is capped at 16 frames for 64 frames after the change. Cache cells drop to about 16 updates. Learned lamp footprints are dropped. Continuous animation keeps all of this on permanently. Also `unchangedReceivers` omits `m_geometry`, keeping `glossy_reflection_temporal.comp:31` on its static fast path under fixed-camera rigid motion | `RenderScene.cpp:557-568`, `RenderSceneGeometry.cpp:711, 778-781`, `screen_probe_integrate.comp:115`, `world_cache_lighting.comp:86-102`, `world_cache_resolve.glsl:38-45`, `HybridGIRenderer.cpp:1131-1132` |
| A material, an emissive or a skinned vertex changes | Bounce and glossy history restart, and the cache's lighting epoch changes for every cell (`RenderScene` tracks only a single `m_materialsDirty` boolean, not per-material dirty indices) | `RenderScene.cpp:578-583`, `RenderSceneGeometry.cpp:425`, `HybridGIRenderer.cpp:1111-1118`, `WorldRadianceCache.cpp:170-172` |
| The environment rotates, changes intensity or changes texture | All screen history and the whole world cache are cleared, and `VoxelGIRenderer` rebuilds the entire environment importance CDF every frame even though `environment_columns.comp` and `environment_tiles.comp` are cubemap-local and invariant to rotation and intensity | `SceneLighting.cpp:58-61`, `HybridGIRenderer.cpp:986-996, 1053-1056`, `VoxelGIRenderer.cpp:829-833` |
| Any of the above | A settled probe can move from one 16-ray batch to three with specular occlusion enabled. The diffuse-only scheduler permits five batches. The probe filter runs at its widest (192 px) with the bounce edge stop off | `screen_probe_proposal.comp:46-58`, `screen_probe_filter.glsl:71-72, 146-152` |

- **Why it shows:** an earlier build measured one probe's per-frame irradiance at 168% of its mean, and 98% of
  the pixel level after filtering, with temporal filtering off ([NativeGIPlan](../NativeGIPlan.md), probe change
  detector, 2026-11-15). A settled probe averages hundreds of frames. After a reset it averages one.
- **Why no gate caught it:** the runners turn the light ball off and disable animation. At the baseline revision, ZenApp could move only
  the camera and one scene light during a run (`--camera-path`, `--light-path`), and `gi_turn_replay.py:151-152`
  rejected `animationEnabled`. No baseline quality gate rendered a moving ball, an animated object, an environment change
  or a material edit.
- **Direct light is not the cause:** it uses no random sampling (`receiver_visibility.glsl`).
- **A reference-path precedent.** The correlated predictor is gated by the reference/relax selection. In that configuration it measured the change along the same rays and moved history by an additive delta (`HybridGIRenderer.cpp:1097-1100`). The HW tier restarts instead (`:1109-1110`).

### Camera motion

- **Measurements:**
  - Error is concentrated in young probes. On 2026-11-18, probes with no history had 2.178% of pixels above
    four levels, against 0.003% for probes 255 frames or older ([NativeGIPlan](../NativeGIPlan.md), receiver
    query investigation).
  - Extra rays for young probes cut failures for ages 1–31 from 6,292 to 1,778, but not for probes with no
    history (471 against 468).
- **Historical baseline gates:** Phase 2 diffuse passed 15/15. The seven 180° replays stay under 0.1% noisy pixels
  ([FastTurnImplementation](FastTurnImplementation.md)).
- **Remaining errors that are not noise:** repeatable differences at creases and thin moldings. They come
  from reconstruction, not noise, so a denoiser will not fix them.

## How shipped engines handle it

The source comparisons use UE5 checkout `821fa3e7e3e5` and NRD 4.11.3 checkout `33e10fa`.

| System | What it does | What we take |
|---|---|---|
| Lumen, UE 5.9 (`LumenSceneRendering.cpp:2603-2673`, `LumenScreenProbeGatherTemporal.usf`, `LumenScreenProbeFiltering.usf`) | The global-light update checks the selected directional light and sky. It requests propagation when the old/new maximum-channel intensity ratio exceeds 4 or falls below 0.25. The screen history defaults to 10 frames. Rays that hit moving objects (`IsTraceMoving`, `LumenScreenProbeTracingCommon.ush:28-44`) raise the update speed, hold it over frames and widen the probe filter there | Local response from what rays hit, including moving objects. Hold it over time. Widen the filter only there |
| Lumen world cache (`LumenWorldCacheUpdate.usf`) | 16-frame cap, no edit handling. New cells take their parent's history. Optional separate indirect record | Separate records. Seeding from existing cache data |
| MegaLights, UE 5.9 (`MegaLightsDenoiserTemporal.usf`, `MegaLightsDenoiserSpatial.usf`) | 12 frames, softly clamped to a 5×5 neighborhood. The clamp distance shortens history, down to 4 frames. The spatial filter runs only where relative deviation is high | Clamp distance as a confidence |
| Lumen reflections (`LumenReflectionDenoiserTemporal.usf`) | 12 frames. Chooses the surface or hit-point history that matches the neighborhood better | Dual reflection history |
| Lumen ReSTIR gather (`LumenReSTIRGather.usf:280`) | Re-traces stored samples to validate them | Fallback for occluder changes |
| NRD ReBLUR and ReLAX (local README: 4.11.3) | Fast and slow histories. The slow one is clamped to the fast one's neighborhood, and anti-lag shortens history without triggering history repair. Optional confidence input | Fast/slow clamp for glossy |
| [RTXDI confidence](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/Confidence.md), after [A-SVGF](https://cg.ivd.kit.edu/atf.php) | Shades the same surface with the same light sample in both frames. The matched sample supplies a temporal gradient for denoiser confidence | Measure change with a common sample. Finite ray coverage still limits the estimate |
| SHaRC (RTXGI 2.9 `Docs/SharcGuide.md`; the local library is 1.6.5) | Responsive mode for short-lived lights. New cells blend the adjacent level's data | Moving-light record. Seeded new cells |
| [Frostbite GIBS 2](https://advances.realtimerendering.com/s2024/content/EA-GIBS2/Apers_Advances-s2024_Shipping-Dynamic-GI.pdf) | One estimator per probe sets the history blend. Probes give surfels their first estimate where coverage is low | One change value per probe. Cache priors for new probes |
| [AMD GI-1.0, §2.1.4](https://arxiv.org/html/2310.19855) | Its asymmetric radiance blend preserves occlusion detail and can darken the image | ZenEngine scores mean HDR energy independently of noise reduction |
| [EA FAST](https://www.ea.com/seed/news/spatio-temporal-sampling) | Shapes sample noise so the following filter removes more of it | Sampling decorrelated across probes (with anchor-reprojection check) |

**Considered and rejected as the main denoiser:**

- **NRD per pixel.** The local README gives 2.40 ms for `REBLUR_DIFFUSE_SPECULAR` at 1440p on an RTX 4080
  and 621.6 MB at 2160p, which is about 5.4 ms at 4K by pixel count. The engine's own per-pixel denoiser cost
  26.4 ms at 4K. Probes are 51× fewer than pixels, so denoising belongs there.
- **Per-pixel ReSTIR for disocclusions.** 3.6–13.7 ms measured for the retained stages
  ([NativeGIPlan](../NativeGIPlan.md)).
- **Machine-learned denoisers.** The baseline has no vendor ray-reconstruction integration or matched
  measurements. Adding one requires a separate implementation and hardware qualification.

## What has already been tried here

| Experiment | Result | Consequence |
|---|---|---|
| Change detected from the noisy signal: fast/slow, then pooled 6σ | False resets, then reflections too dark for over 1,000 frames | Change must be measured, never inferred from noise |
| All priors kept after a light edit; full far history | RMS at +128 rose from 0.3424 to 0.3630; far history lagged | Response scales with the measured change |
| History borrowed from neighbours within 45° | Error at the borrowed age 38.2 against 1.3 per 10,000 pixels | The cache prior below uses the probe's own surface and decays within frames |
| Rays moved from mature to fresh probes | All nine motion comparisons worse | Mature probes keep their rays |
| History and filter radius limited by mean hit distance | 0.1071% against 0.0767% | Not used |
| Proposal learned from returned irradiance; snapped ray origins | Worse | Not used |
| Per-pixel history fused into the gather, 1,012.5 MiB, 32 frames | The build with it failed all nine motion comparisons and light recovery | A pixel pass is only a Stage 9 option: short, small and young-only |
| Half-resolution final history | More motion and light-change error | Not used |
| Filtered probes fed back into history | Moves energy along the camera path (`screen_probe.glsl`) | Histories stay raw |
| Four-pixel probe grid | Focused fast pan 0.0293% against 0.0458%, at 4× probes | Not part of the core plan (cost) |

## Algorithm

```text
world cache updates (paths) -> direct/env/object/material change evidence (catching up from lastSample)
                     |
    per cell:  D_cell, R_cell -> cell history cap + stored cell residual change / staleness
                     |
rays (probe, glossy) -> hit (`WorldLookup` + cell residual change; live direct for moving lights) or escape
                     |
    per probe / texel:  D = absolute change, Δ = signed correlated SH/radiance delta, R = radiance
                     |
    history:  drift/step cap N* from noise and change; additive Δ prediction (exact uniform scale for
              intensity-only); no history -> cache prior (`WorldIndirect` with parent fallback)
                     |
    probe signals: sky | settled bounce | moving-light bounce -> spatial filter per signal -> gather
                     |
    [Stage 9 follow-up: extra probes on young tiles, denser grid, pixel pass, TAA/upscaler or random gather]
```

### A. Change evidence

A sample can carry `D` (change magnitude), `Δ` (signed correlated change) and `R` (radiance).
Authored edits and visibility queries supply evidence of change. Finite sampling affects how quickly
a cell observes it and the accuracy of the reconstructed coefficient.

| Source | How a sample measures it | Predictable (`Δ`) |
|---|---|---|
| Stationary light with edited intensity, colour, cone or enabled state | Match previous and current parameters by the stable 64-bit light ID even when active slots move. A sampled surface can reuse visibility when both states have the same shadow ray and geometry. Turning a light off gives it zero power. | Direct contribution on the sampled surface |
| Light moving or changing direction | Evaluate visibility toward the old and new light poses on the same surface. Reuse a coincident shadow ray and skip states with zero incident power. This measures shadow entry and exit caused by the light. Concurrent caster motion also uses the swept-bounds journal because both queries see the current acceleration structure. | Signed direct difference for light motion on the sampled surface |
| Environment, rotation and intensity | Split `m_environmentTextureRevision` (still global reset) from parameter revisions in `RenderScene`, stopping per-frame `BuildEnvironmentDistribution` rebuilds in `VoxelGIRenderer.cpp:829-833`. Escaped rays evaluate previous and current environment parameters—applying the `residualEnvironment` source-tile exclusion (`ProbeMissRadiance`) under **both** previous and current orientations so rays crossing per-pixel sun-tile boundaries never generate false `D` or `Δ` | Yes (additive `ΔSH` along escaped rays; exact uniform scale for intensity-only) |
| Moving objects | Compare each committed caster's ray-visibility revision and its previous bounds. Record the union of old and new bounds for rigid movement, deformation and coverage changes. Test primary, shadow and escape segments against those bounds. The glossy static fast path also checks the geometry revision. | No |
| Materials and emissive | Stamp each changed material and publish its exact index. A hit can match that index across the elapsed interval even when its cell was skipped on the edit frame. Colour and emission alone do not add the caster's entire bounds. | No |

- **Cache-to-probe/glossy propagation and phase alignment (critical for HW RT):**
  - On the HW RT path, `ScreenProbeShadeHits` (`screen_probe_trace.glsl:411`) and non-mirror glossy hits
    (`ray_hit_lighting.glsl:195`) read `WorldLookup`, not direct analytic lights. If a non-moving light edits
    on frame `t`, most world-cache cells still return pre-change radiance on frame `t` and converge over
    frames `t+1…t+16`.
  - Therefore, **do not** predict direct-light `Δ` into settled probe bounce while `ScreenProbeShadeHits` is
    still blending pre-convergence `WorldLookup` radiance. Instead, `WorldCacheShade` computes direct/env/
    object/material change `D_cell` on cache updates; `WorldCacheResolve` caps the cell's history and stores a
    decaying **cell residual change / staleness** value in the cell state; and `WorldLookup` returns that
    residual change alongside radiance so every probe and glossy hit reading a transitioning cell adds it to
    its own `D` (and discounts observation confidence if the cell is still mid-transition).
  - This single mechanism handles **both** bounce-1 phase alignment and **multi-bounce indirect changes**
    (indoor walls lit by bounce-2+ from a rotated sky, moved occluder shadow or edited emissive), cleanly
    replacing the global `sourceSignature.w` scalar.
  - Conversely, Stage 4's **moving-light signal** evaluates fresh direct light at probe hits, so its `D` and
    correlated `ΔSH` match what that signal actually integrates on the same frame.
- **Sparse-cell and multi-frame catch-up:**
  - `world_cache_resolve.glsl:19-23` skips cells with `samples == 0u` (~90% of resident cells on any single
    frame), and `ScreenProbePlace` can reproject from `previousProbes[1..2]` (up to 3 frames old). Diffing
    only frame `t` against `t - 1` would miss every cell or probe not updated on the exact frame of a discrete
    edit, trapping stale radiance at `age = 128`.
  - Store a compact change log / per-entity last-modified frame stamp (per light ID, environment parameters,
    recent swept-bounds ring and per-material last-modified frame) alongside previous snapshots for recent
    discrete edits. When a cell with `worldCells[slot].lastSample < currentFrame - 1` is next sampled (or a
    probe reprojects across `> 1` frame), evaluate all changes stamped `> lastSample`. If an edit aged out of
    the snapshot ring before the cell was visited, mark the cell stale (`D = R`) on its next visit.
- **Aggregation:**
  - **Probes** accumulate signed directional differences on their existing sampled rays. The temporal pass
    pools previous change evidence over a compatible 5×5 footprint during transitions. Weighting the
    current radiance by its own measured change biased the native switch-on fixture.
  - **Cache cells:** accumulate `D` and `R` (8 bytes per cell) during `WorldCacheShade`; `WorldCacheResolve`
    caps cell `age` and updates the stored residual-change field read by `WorldLookup`.
  - **Glossy texels:** the closest-hit payload carries a scalar response. The trace stores the maximum
    across its rays in `glossyRaw[index * 2 + 1].y`, beside hit distance. Temporal accumulation uses it
    to cap age. Cached radiance already includes corrections published by the cache.
- **Proposal refresh:** decouple the analytic top-8 light selection in `screen_probe_proposal.comp` from the
  64-frame `reuse` block (`:69-85`) whenever lights move, toggle or change intensity, so settled probes guide
  toward newly approaching or turned-on lights immediately. Keep the octahedral CDF and 3 learned lobes frozen
  over the 64-frame block so MC samples stay uncorrelated.
- **Full resets** apply to camera cuts, scene loads, incompatible settings or topology, a replacement sky
  texture and allocation of the first or removal of the last moving-light record. Journal overflow makes an
  observer whose last update predates a discarded edit take a conservative update. Settled and moving
  light journals record their discarded epochs independently.

A cell accumulates normalized path changes in fixed point and divides by its actual sample count.
Its packed response stores both the remaining uncertainty and the fraction that has reached the
published radiance. Secondary feedback consumes the first quantity, including when a lamp becomes
dark. Probe and glossy observations consume the second. An additive same-ray correction changes
the retained mean directly. This distinction uses the existing four-byte response word.

### B. History length

Choose the weight of a stochastic observation from information available before drawing that
observation. This includes the decision to read old neighboring evidence. In the sparse-direction
example, one ray direction in sixteen has radiance 16 and the rest have radiance zero. The true
mean and the retained mean are both 1. If only a current hit activates a neighbor's shorter history,
the expected next mean is 1.23071. Selecting the weight from retained evidence gives 1.00000.
Current evidence can inform the next observation. [Reproduce the expectation](../../tools/gi_history_response.py)

- **Continuous drift vs discrete step:**
  - Under steady drift at relative rate `λ` per frame, MSE `σ²/(2N) + λ² N²` is minimized by
    `N_drift = clamp((σ² / 4λ²)^(1/3), 4, max)`.
  - Under a **discrete or large unexplained step** (light toggle, lamp jump, material edit, stepped object,
    shadow snap behind a column), clamping at `nmin = 4` with harmonic `n = min(n + 1, cap)` retains 75% stale
    energy on frame 0 and decays only as `3 / (3 + k)` once `λ` returns to 0—leaving **25–27% error at frame 8**
    and **8.3–8.6% at frame 32** (~147 frames to reach 2%), violating the acceptance gate.
  - Therefore, pair `N_drift` with a step cap `N_step = clamp(1 + σ² / Δ_unexplained², 1, max)` (so large
    unexplained steps drop `N` toward `1`, or discount retained prior weight exponentially while `D` is held),
    taking `N* = min(N_drift, N_step)`.
- **Explicit `σ` and units per history (avoiding filter circularity):**
  - `ScreenProbeTemporal` runs **before** `ScreenProbeSpatial*` (`ScreenProbeGI.cpp:398-419`), and the spatial
    filter radius depends on post-temporal `sampleAge`, so temporal integration cannot measure *this* frame's
    post-filter `σ`. Use a calibrated per-signal post-filter noise constant (or previous frame's filtered
    variance with a floor) scaled by `1 / sqrt(batches)`:
    - **Probes:** caps effective `batchWeight` and `age` per signal independently (`sky`, `settled bounce`,
      `moving bounce`).
    - **World cache:** calibrated cell noise constant, capping update `age` (up to `giCache.limits.y = 128`).
    - **Rough-reflection atlas (`screen_probe_prefilter.glsl`):** its own per-probe specular age/change state
      (instead of sharing `min(sky.age, bounce.age)`), decoupling sky edits from bounce reflections.
    - **Glossy reflections:** texel noise from `moments` (with floor), capping `age` (up to 512).
- With no change, `λ = 0` and `Δ_unexplained = 0`, so today's settled behaviour is unchanged.
- Ray batch counts in `screen_probe_proposal.comp` follow the capped weight, so only affected probes trace
  extra batches. Learned directions and lamp footprints are kept while `λ` is small.

### C. Prediction

- **Additive correlated delta (`ΔSH`) as the primary predictor:**
  - A scalar multiplicative ratio `r = R / (R - S)` has four concrete failure modes: (1) zero/negative
    denominator when turning a light on from dark (`R ≈ S`, and `0 × 2 = 0` cannot brighten a dark probe);
    (2) hue/color changes at equal luminance (`S_luma ≈ 0`); (3) directional distortion on L1 SH and octahedral
    atlases—contrary to intuition, scalar multiplication is **not exact for sky rotation**, because rotating
    the sky behind fixed windows shifts directional L1 moments as well as magnitude; and (4) Jensen/ratio bias
    when dividing by noisy Monte Carlo `R` plus asymmetric clamping over 512 frames.
  - Instead, accumulate the signed correlated delta `ΔSH` (`radiance_curr - radiance_prev` along the **same**
    rays, just as the Compute tier did) for applicable signals (escaped sky rays on environment rotation/
    intensity; live direct-light hits on Stage 4's moving-light signal), and shift history additively before
    blending. Add the correction componentwise. Clamp the scalar irradiance and the constant SH coefficient
    to zero from below. Retain the sign of the three directional coefficients and limit the length of that
    vector to `sqrt(3) × L0` for each colour channel. The same-ray difference can represent turn-on, hue
    changes and changes of direction behind a fixed aperture. It has the correct expectation under matched
    sampling before clamping. Clamping or choosing weights from the current observation can introduce bias.
- **Exact uniform scaling for pure global intensity edits:** when only environment intensity changes (zero
  rotation) or a single isolated signal scales uniformly with a known noise-free factor `I_new / I_old` from
  uniforms, multiply history by `I_new / I_old` directly (zero MC variance).
- For a simultaneous intensity and orientation edit with a positive old intensity, factor out the known
  intensity ratio first. Apply the same-ray angular remainder `L_new − (I_new/I_old) L_old` to the
  scaled history. When the old intensity is zero, use an additive difference from zero.
- **Residual safety margin:** B still counts at least half of predicted change (`0.5 * |Δ|`) as unexplained
  residual in `λ` so shadow transitions and sample noise in `ΔSH` never lock in stale history.

### D. Moving lights in their own signal

- **Moving class:** the light ball when enabled, and scene lights marked **Movable** (a new flag, off by
  default). Adding the first or removing the last moving class changes the history layout and resets it.
  Within an existing layout, changing a light's class subtracts its old contribution from one estimator
  and adds it to the other through its stable ID. A disabled authored mover retains the layout across toggles.
- **Signals per probe:** `sky`, `settled bounce`, `moving bounce`.
  - Moving bounce evaluates fresh direct light from moving lights at probe hits, and reads their bounce-2+
    indirect lighting from one extra world-cache record with a 16-update window (1 record = +32 B/cell,
    isotropic/geometric-normal indirect; Stage 4 verifies whether bounce-2+ directional detail is sufficient).
  - **Moving-light shadow samples.** Probe batches occupy different angular strata. Always using
    batch zero omits the other strata. Choose one batch uniformly per ray and multiply its contribution
    by the batch count before averaging. The signed predictor reuses the live visibility result and
    evaluates the oldest recorded pose on the same surface. Use the authored 0.12 proxy and the
    range-1000 lamp for the two Nsight cost measurements.
- **Exclude moving lights from `WorldLevel(position)`:** stop packing `cameraLight` and `Movable` lights into
  `worldCache.localLights[]` (`WorldRadianceCache.cpp:183-190`, `world_cache.glsl:33-51`). Otherwise moving
  the light ball constantly shifts spatial cache levels around it and invalidates settled-bounce cells.
- **Hit records** keep six words. A hit with moving energy or a signed moving delta packs settled RGB,
  moving RGB and signed delta RGB into the three existing payload words. A hit with neither contribution
  retains FP32 settled radiance. Flags distinguish these encodings from the sky-rotation payload.
- **Filter handoff:** `screen_probe_horizontal` grows from 8 to 12 `vec4`s per probe; the vertical filter sums
  settled and moving bounce back into 8 `vec4`s so `ScreenProbeGather` is unchanged, while combining both
  signals' `gatherNoise` and `age` into the statistics vector (`screen_probe_filter.glsl:58-59`) so gather's
  slope-fit confidence and `bounceAge` stay valid.
- **Reflection trade-off:** `ScreenProbeSpecular` (112 texels/probe) and `GlossyReflections` are **not** split
  into settled vs moving signals (to avoid doubling reflection VRAM and gather bandwidth). Reflections near a
  moving light therefore shorten their own history locally via `D`/`N*`.
- With no moving class, allocate only the settled records and omit the moving filter and integration passes.
  Shared shading programs skip moving-light evaluation using the frame's layout flag.

### E. Prior for probes without history

- A probe with no predecessor reads the world cache's indirect seed record (`WORLD_SEED_RECORD`,
  `WorldIndirect`) at its anchor, along its normal.
- **Codebase prerequisites:**
  - Enable `WORLD_SEED_RECORD` on the screen-probe path (`WorldRadianceCache.cpp:127` currently disables it
    when `screenProbeGI` is true).
  - Add a `level + 1` parent fallback to `WorldIndirect` (`world_cache.glsl:208-223`), matching `WorldLookup`,
    so newly disoccluded fine cells can seed from a warm coarse parent.
  - Note that `world_cache_shade.glsl:151-156` accumulates scalar irradiance along each path's *shading* normal
    (not geometric anchor normal), so the prior initializes isotropic L1 SH along `probe.normal.xyz`.
- The prior counts as at most one batch (16 rays), so the probe's own three batches outweigh it within a frame
  or two (at most 25% of frame 0's bounce estimate). Relocated probes with little footprint support take it in
  proportion to missing support. Sky is not seeded.
- **Stage 0 pre-check:** measure what fraction of age-0 probes on the 180° turns and pans actually hit a warm
  cache cell (exact or parent), since disoccluded rooms may have cold cells. If warm hit rate is too low to
  move age 0–7 error by 20%, stop early rather than tuning prior weights blindly.
- **Difference from rejected borrowing:** reads the probe's own surface cell, not a neighbour's probe, and
  decays within frames. Gated on the same age-binned error table that rejected borrowing.

### F. Spatial filter and sampling

- Each signal gets its own filter radius and edge stop from its own `sampleAge` and `batchWeight`. Today one
  radius comes from `min(sky, bounce)` (`screen_probe_filter.glsl:71`). The vertical pass sums settled and
  moving bounce and merges their variance/age into the gather statistics vector.
- **Experiment (blue-noise sampling):** evaluate replacing `ProbeAnchorSeed` hashes with spatiotemporal blue-
  noise offsets (FAST / STBN) so the probe filter averages decorrelated errors.
  - **Known tension to test explicitly:** screen-tile STBN breaks a world-anchored probe's 64-frame Sobol
    sequence (`screen_probe_trace.glsl:267-289`) as it crosses tiles during camera motion, while locking an
    offset to the probe's birth tile destroys spatial blue-noise correlation between neighboring tiles—and
    per-probe CDF/light/lobe proposals already decorrelate adjacent probes' mappings. Test tile-indexed vs
    birth-tile offsets against static flicker and motion error; drop immediately if temporal stratification
    regresses.

### G. Reflections

- **Rough reflections (`screen_probe_prefilter.glsl`):** give rough reflections independent per-probe change
  and age tracking under A–B (instead of reading `min(sky.age, bounce.age)` at `:127-129`), and check whether
  relocated probes benefit from footprint reprojection.
- **Glossy reflections (`glossy_reflection_temporal.comp`):**
  - Replace the hard fast/slow reset (`:118-123`) with A–B change-driven `N*` plus soft clamping of `slow` to
    `fast`'s 3×3/5×5 neighborhood, shortening history by clamp distance. Because neighboring `nextFast` values
    are written in the same pass, compute the neighborhood via a shared-memory tile+halo (or in a separate
    pass), and exclude or tighten the clamp on near-mirror texels (`roughness < 0.08`) to preserve sharp edges.
  - **Experiment (dual history):** under camera motion, choose between virtual-position and surface-position
    histories by neighborhood match (as Lumen reflections do). Evaluate the surface candidate with bilinear
    taps (or only when virtual weight/match is weak) rather than doubling the 12-tap Catmull-Rom search
    (`:45-99`).

### H. Camera-motion options (Stage 9 — optional follow-up)

- **Why gated:** Stage 8 requires measured locality, Phase 2 diffuse 15/15 and 180° replays below 0.1%. Stage 9 tests five outside-denoiser options that trade cost and
  engine-wide complexity for lower young-probe motion error; **run Stage 9 only after Stages 1–8 land and pass
  profiling**, and keep at most the best one or two.
- **Timing:** each option needs interleaved 4K Nsight runs because each trades GPU time and VRAM for quality.

| Option | What it does | Evidence and codebase constraints | Expected cost |
|---|---|---|---|
| H1. Extra probes on young tiles | While a base tile's probe is young (< ~16 frames), allocate one diffuse-only adaptive probe from a capped pool (32,400 slots, +25%) and release it when mature | One adaptive slot per tile in diffuse-only lowered all nine motion errors and static RMS; lamp recovery rose 0.24342 → 0.24515 ([NativeGIPlan](../NativeGIPlan.md)). **Constraint:** adaptive probes are currently arithmetic-indexed (`grid.z + base / adaptiveTiles`); a dynamic pool needs per-tile indirection in Place/Filter/Gather, multi-frame slot stability for reprojection, and indirect dispatch to cost zero when settled. Compare first against simpler extra-batch reallocation on young base probes | ~0 ms static (with indirect dispatch); bounded by pool during motion. ~50 MB diffuse history |
| H2. Uniform denser diffuse grid (6 px or 4 px) | Diffuse probes on a denser grid; reflection atlas stays at 8 px | 4 px improved focused fast pan 0.0458% → 0.0293%; with reflections it needed 15.01 GiB and fell back to PBR. **Constraints:** `screen_probe_gather.glsl:19` ties workgroup size to `ZEN_SCREEN_PROBE_SPACING` (6×6 = 36 and 4×4 = 16 hurt warp occupancy and must be decoupled). At equal rays/probe, 6 px (288k probes) and 4 px (648k) are **already expected to exceed the +2.0 ms gate**—only test H2 if paired with fewer rays per probe (e.g., 8 rays/batch), or use 4 px purely as an offline quality upper bound | Several ms at 6 px, >10 ms at 4 px at equal rays/probe; ~1 GB diffuse history at 4 px |
| H3. Short pixel pass | Applies only where gathered probe weight is low (young-probe pixels during motion). At most 8 frames, soft neighborhood clamp. Never feeds probes or caches | The fused 1,012.5 MiB, 32-frame pixel history failed motion and light recovery | 0.4–0.6 ms, 150–215 MB persistent VRAM at 4K (verify no world-cache capacity drop) |
| H4. Temporal upscaler / TAA | Jittered camera + full-frame temporal resolve (native AA first, then upscaled). In-house TAA proxy or capability-selected vendor SDK (FSR 3.1 Vulkan / DLSS via Streamline) | **Major engine scope (not a 3-day experiment):** ZenEngine has no jitter or per-object screen-space velocity buffer for animated geometry, and jitter affects `ReceiverVisibility` (4×4 reuse), `ScreenProbeSources` (8×8 reuse), `ScreenProbePlace`, `ScreenProbeGather`, `GlossyTemporal` (`unchangedReceivers`), plus `DeferredLightingRenderer` (`ForwardOpaqueDisplay`, `ForwardTranslucent`, `ResolveRefraction`, `CameraLightBall`). Alone it adds little GI denoising because world-anchored probe errors stay spatially coherent; mostly useful paired with H5 or as a separate AA/scaling initiative | Requires a separate timed implementation and GPU memory measurement |
| H5. Random interpolation with a pixel pass | Probes stay world-anchored; each pixel selects 1 (or 2) probes at random by weighted reservoir over `gatherMask` using today's `ProbeCubicWeight × ProbePlaneWeight`, entering a full-frame pixel history whose length follows probe maturity and `λ` | Gather is ~3.6–4.8 ms today (`NativeGIOptimization.md` O2, `FastTurnNoisePlan.md:88`). **Constraints:** Gather must still iterate `gatherMask` to compute tap weights, must specify how `ProbeSpecular` behaves (1 random probe vs existing near-ring), loses the spatial slope fit (`screen_probe_gather.glsl:210-245`), and without TAA (H4) shows raw 1-probe dithering on disoccluded motion pixels | Must beat Stage 8 overall (gather savings minus 0.4–0.6 ms pixel pass); 150–215 MB VRAM shared with H3 |

- **Selection rule:** keep an option only if all of these hold:
  - Against the Stage 8 build, it cuts focused fast-pan pixels above four levels and age 0–7 error by at least
    30%.
  - It passes every other gate, including preserving `WorldRadianceCache` capacity.
  - It fits its cost: H1 and H3 at most +1.0 ms during motion and ~0 static, H2 at most +2.0 ms, H5 no slower
    than Stage 8.
- **H5's other route:** H5 is also kept if it matches Stage 8 on every gate (including static flicker, static
  RMS, and crease/thin-surface crops) and saves at least 1.0 ms.
- **H4 adoption:** treat H4 as a separate user-approved rendering milestone (AA, resolution scaling, velocity
  buffer, possible vendor SDK), not a quick Stage 9 check.
- **2026-11-26 update:** the [three-case check](GIDenoiserFinalFix.md#three-case-check-2026-11-26) found 3.93%
  of moving-camera pixels more than 10% off their settled value, against 0.000% with a still camera. H3 is
  extended to a full-frame pixel history limited by measured lighting change instead of probe age:
  [Next fix: pixel history for camera motion](GIDenoiserFinalFix.md#next-fix-pixel-history-for-camera-motion).

## Original hypotheses by situation

| Situation | Baseline | Intended outcome | Parts | Initial confidence |
|---|---|---|---|---|
| Light ball, moving or animated lamps | Whole screen restarts; `WorldLevel` churns around ball | Local; moving light in its own predicted signal (`ΔSH`), isolated from `WorldLevel` | A–D | High |
| Light intensity or colour edits, flicker, on/off | Global restart, including sky and cache | Local, predicted (`ΔSH` / exact uniform scale); sparse cells catch up via timestamps | A–C | High |
| Directional sun rotation | Global | Shortened where sun/bounce changes reach; indoor bounce updated via `WorldLookup` propagation | A–C | Medium |
| HDR environment rotation or intensity | Everything cleared + per-frame CDF rebuild | No CDF rebuild; escaped rays use `residualEnvironment`-matched `ΔSH` / uniform scale; indoor bounce via cache propagation | A–C | Medium |
| Animated or moved objects, skinning | Every probe capped and cache shortened while anything moves | Only probes/cells hitting moved instances or crossing swept bounds (including shadow walls via `WorldLookup`) | A, B | Medium |
| Material or emissive edit | Global restart | Hits on journaled material IDs and propagation through the world cache | A, B | Medium |
| Camera pan, turn, dolly | Young probes noisy for their first frames | Cache prior (with parent fallback) and filter/sampling; optionally Stage 9 | E, F, H | Medium: denser probes are measured to help; the rest are experiments |
| Static view | Converged | Same, possibly less noise from sampling | F | Low |
| Camera cut, scene load, settings, environment texture | Reset | Reset (correct) | — | — |
| Thin surfaces with no probe | [FastTurnNoisePlan](FastTurnNoisePlan.md) | Not addressed here | — | — |

## Offline check of the history rule

An existing one-probe simulation (`build/moving-light-denoiser-20261125`) modeled heavy-tailed multiplicative
lognormal noise at the documented 168% after a spatial filter worth 20, 40 or 60 probes, on a smooth bell-
curve light pass (`sig = 0.02 + 1/(1 + ((t-300)/w)^2)^1.5`). It tests the drift rule, not the renderer.

| Constant signal that nothing changes (`K = 20–60`) | Frame-to-frame change |
|---|---:|
| Reset every frame (synthetic comparison) | 31–53% |
| 10-frame history (Lumen's length) | 2.2–3.9% |
| Cap with `λ = 0`, up to 256 frames | 0.08–0.15% |

| Smoothly changing signal near a moving light, RMS of its peak (`K = 40`) | Fast pass (`w = 20`) | Walk (`w = 90`) |
|---|---:|---:|
| Cap, no prediction (`residual = 1.0`) | 3.4% | 3.9% |
| Predicted, 10% random error, `residual = 0.25` | 1.8% | 2.2% |
| Predicted, 50% systematic error, `residual = 0.5` | 2.5% | 3.2% |
| Predicted, change missed entirely, `residual = 0.5` | 3.4% | 4.1% |

- On smooth motion, the drift cap matches a well-tuned fixed EMA while keeping 256-frame stability wherever
  `λ = 0`. Counting half the predicted change as residual (`0.5`) bounds worst-case error when prediction
  misses.
- **Scope of the original simulation and the Stage 0 extension:**
  - The scripts tested only smooth bell curves, not **discrete steps** (toggles, material edits, column
    occlusions). Analytically, `nmin = 4` with harmonic `1/n` growth leaves `3/(3 + k)` stale error after a
    step (**25–27% at frame 8**, **8.3–8.6% at frame 32**, ~147 frames to 2%), which is why Section B adds
    `N_step` down to `1`.
  - The scripts applied a scalar ratio from the noise-free truth `sig[t] / prev`, not noisy `R / (R - S)` or
    additive `ΔSH`. Stage 0 extends the offline check to verify step changes, shadow snaps and additive `Δ`
    stability over 512 frames. [Extended simulation](../../tools/gi_history_response.py)

## Stages

Each stage ends with a decision. Checks cover GI quality, native tests of the shaders touched and one real
ZenApp launch per new program. **Nsight 4K frame-time checkpoints run at Stages 0, 1, 4, 8 and 9** (not only
at the end) so shadow-ray and memory-bandwidth regressions are caught immediately.

| Stage | Work | Continue if |
|---|---|---|
| 0. Measure (2 days) | Baseline Nsight 4K timings on `94eb2c4d`; warm-cache hit rate for age-0 probes on 180° turns/pans; extended offline simulation (steps, shadow snaps, additive `Δ`); ZenApp path/pose-hold support + scenario runner on `gi_turn_replay.py` (lifting the `animationEnabled` block via frozen reference poses); confirm `radianceSteps == 0` on probe path | Each scenario reproduces its excess noise; baseline timings and age-0 cache hit rates are logged |
| 1. Lights & cache propagation (3–4 days) | Stable GPU `LightId`s; A (lights, shadow-transition handling, sparse-cell timestamp catch-up, `WorldLookup` residual-change propagation, analytic top-8 light refresh decoupled from 64-frame `probeCDF` freeze); B (drift + step rule) for all histories; retire light resets. **Nsight check** | Lighting scenarios meet far-field and 8/32-frame recovery limits; lamp recovery passes; overhead fits budget |
| 2. Environment (1–2 days) | Split `m_environmentTextureRevision` vs parameter revisions (stop per-frame CDF rebuilds); A (escaped rays with dual-orientation `residualEnvironment` masking) + C (exact intensity scale, additive `ΔSH` on rotation); replace environment clear | Environment scenario passes (including indoor bounce via `WorldLookup`) |
| 3. Objects and materials (2–3 days) | Committed shadow-caster revisions and swept world AABBs; per-material revision stamps in `RenderScene`; fix `unchangedReceivers` to check `m_geometry`; A (moving instances, swept bounds in cache & probe traces, changed materials); replace geometry and surface resets | Animated-object and material scenarios pass |
| 4. Moving lights and prediction (3–4 days) | D (`Movable` flag, 3rd probe signal, 16-update cache bounce-2+ record, exclude moving lights from `WorldLevel`, RGBE hit-record packing, merged vertical filter stats, ball radius clamp) and C (additive `ΔSH` on moving signal). **Nsight check** on an authored finite-range proxy (`range = 0.12`) and a room-wide `Movable` lamp | Near-field limits; no drift over 512 frames of motion; shadow cost within budget; cache capacity preserved |
| 5. New-probe prior (1–2 days) | E (enable `WORLD_SEED_RECORD` on probe path, add `level + 1` parent fallback to `WorldIndirect`); skip if Stage 0 shows warm hit rate is too low | Age 0–7 error falls by at least 20% with no other age band worse |
| 6. Spatial and sampling (2 days) | F (per-signal filter radius/edge stop; tile vs birth-tile blue-noise experiment) | Static and motion gates no worse; blue-noise kept only if motion error drops without breaking temporal stratification |
| 7. Reflections (1–2 days) | G (independent rough-reflection age/change; glossy shared-memory/pass fast-neighborhood clamp excluding near-mirrors; bilinear-assisted dual virtual/surface history) | Glossy motion 15/15; dual history kept only if it lowers motion error |
| 8. Finish core (1 day) | Delete replaced global-reset code & switch, finalize defaults, full Nsight 4K frame-time & VRAM verification, update docs | All core gates, cache capacity and +1.0 ms cost limit pass |
| 9. Camera-motion options *(optional follow-up)* | Evaluate selected subset after Stage 8: H1 (3–4 days, indirection + indirect dispatch vs extra-batch baseline), H3 (2–3 days), H5 (3–4 days, reusing H3), H2 only if rays/probe scale down (2–3 days). Treat H4 (jitter + velocity + TAA/upscaler) as a separate multi-week milestone if requested | Selection rule in H; H4 only on explicit user product decision |

- **Rollout** requires a passing measured gate per scene class. A lower edit-frame RMS alone cannot establish recovery, steady-state energy or moving-camera quality.
- **Switch:** `voxel_gi_history_response=local|global` keeps the old behaviour available during evaluation. Remove it only after the Stage 8 gates pass.

### Main files

| Stage | Changes |
|---|---|
| 0 | `ZenApp/LightPath.*`, `RuntimeApplication.cpp` (ball, environment, material and animation/instance pose-hold paths), scenario runner extending `gi_turn_replay.py` |
| 1–3 | `world_cache_lighting.comp/.glsl`, `world_cache.glsl`, `world_cache_shade.glsl`, `world_cache_resolve.glsl`, `screen_probe_trace.glsl`, `screen_probe_hits.glsl`, `screen_probe_integrate.comp`, `screen_probe_proposal.comp`, `glossy_reflection_*`, `light_occluders.glsl`, `HybridGIRenderer.cpp`, `VoxelGIRenderer.cpp`, `WorldRadianceCache.*`, `ScreenProbeGI.*`, `SceneLighting.*`, `RenderScene.h/.cpp`, `RenderSceneGeometry.cpp` |
| 4–5 | `ScreenProbeGI.h` (+5 lighting vectors, 12 horizontal filter vectors, hit packing), `world_cache.glsl` (seed parent fallback + moving-light record), `sampled_hit_diffuse.glsl`, `screen_probe_filter.glsl`, editor light panel |
| 6–7 | `screen_probe_filter.glsl`, `screen_probe_prefilter.glsl`, `glossy_reflection_*` |
| 9 *(follow-up)* | `ScreenProbeGI.h`, `screen_probe_place.comp`, `screen_probe_gather.glsl` (decoupled workgroup / random-tap variant), pixel-history pass; if H4 is separately approved: velocity buffer, camera jitter across GI/shadow/forward passes, and TAA around `ForwardToneMap` / `ForwardOpaqueDisplay` in `DeferredLightingRenderer.cpp` |

Hit records stay 6 words via RGBE packing (or 7 words if FP32 is required). Tests extend `GICacheTests.inl`
(`NativeProbesRecoverLightingWithoutCPURevisionHint`, plus sparse-cell catch-up and shadow-entry behind an
occluder), `RayGITraceTests.inl` and the pass-list pins in `HybridGIPassListTests.inl`. New cases: unaffected
probes keep growing their weight under each kind of change, affected ones and stale cache cells respond
within 8/32 frames, and settled energy matches a reset. Run all four submission modes.

## Acceptance

Acceptance uses New Sponza at 4K with HDR and normal maps through standalone `ZenApp` (`--setup`).
The skinned fixture and CUDA calibration use their own identified assets. Held
references freeze the exact camera, light, environment, material and animation pose of each scored frame (as
`gi_turn_replay.py` freezes camera poses).

- **Unaffected region (fixed-camera Scenarios 1a, 2–5):** pixels whose settled images before and after the
  change differ by under one display level.
- **Unaffected region during camera motion (Scenario 1b).** Compare settled images at each camera pose with the
  authored finite-range proxy on and off. Select pixels differing by under one display level and score both
  trajectories against the corresponding held image.

**Scenarios:**

1. **(a)** Move an authored light of equivalent range past a column with the camera fixed. **(b)** Use a matched
   authored-light trajectory for camera-motion comparisons. Keep the previously reported camera-light run
   as historical evidence and record which comparisons use a proxy.
2. A lamp moved, its intensity and colour dragged, and a lamp toggled on/off.
3. Environment rotated slowly and its intensity changed (including an indoor view lit only by sky bounce).
4. One object moving across the hall (casting moving shadows on static walls), and an object moved in steps.
5. One material's colour and emission changed.
6. The existing camera gates: Phase 2 diffuse, glossy motion and the seven 180° replays.

| Check | Limit |
|---|---|
| Fixed-camera Scenarios 1a, 2–5, unaffected region: temporal deviation over 32 frames, and RMS | At most 1.10× a run without the change |
| Moving-camera Scenario 1b, unaffected region: RMS and pixels > 4 / > 24 levels | Within 1.10× the matched camera-only run without the light ball |
| Scenarios 1–5, affected region RMS | At most 0.5× today's |
| Affected-region mean energy against the held reference (settled / post-recovery, plus steady-state symmetric motion mean) | Within ±1% |
| Recovery after a discrete or stopped change (toggles, steps, column occlusion, material edit) | Within 10% of the held image by frame 8, 2% by frame 32 |
| Pixels above 24 levels per scored frame | Under 0.1% |
| Lamp move, +128 frames | At most 0.23653 RMS |
| Phase 2 diffuse; glossy motion | 15/15 within accepted tolerance; 15/15 |
| Age-binned motion error (Stage 5) | Ages 0–7 at least 20% lower, no other band worse |
| Camera-motion option (Stage 9, if run) | Focused fast pan and ages 0–7 at least 30% lower than Stage 8, within the option's cost. For H5's other route: every gate within tolerance (including static flicker, static RMS and crease/thin-surface crops) and at least 1.0 ms faster |
| Probe CUDA energy; static flicker and static RMS | Unchanged limits; no worse |
| GPU cost (Stages 1, 4 and 8) | At most +1.0 ms total denoising, change-evidence and extra-batch overhead at 4K against the `94eb2c4d` baseline (without Stage 9). During local changes, probe tracing substantially faster than today's global-reset run |
| World-cache capacity | `worldCacheCapacity` unchanged vs `94eb2c4d` baseline (no VRAM-driven halving) |

For sign-off, compare the median of three paired baseline/candidate measurements. Reverse launch order in
the middle pair. Use the same held reference and pixel mask within each comparison. Pass
`--compare-setup <global.zenrender>` to the scene runner to capture the pairs and their medians.

## Cost and memory

Targets, not measurements. Note that `0.83 ms static / 1.07 ms pan` for probe temporal+spatial passes comes
from the older 39.2 ms build (`NativeGIOptimization.md:52`), while the newer `46.6 FPS / 21.59 ms` GPU median
pan is from `FastTurnNoisePlan.md:17` (still 5.25 ms above the 16.67 ms 60 FPS target). Stage 0 captures
fresh per-pass timings on `94eb2c4d`.

| Addition | Target time | Memory at 162,000 probes, 4K |
|---|---:|---:|
| Change pass, temporal rule, 3rd signal in horizontal/vertical filter | 0.5 ms | 52 MB persistent history (`5 × vec4 × 4` frames), 10 MB transient (`+4 vec4` horizontal) |
| Light evaluation at changed hits, batch-0 shadow rays for moving lights (tightly range-culled; high risk for unculled `Movable` lamps or rotating sun — checked at Stages 1 & 4) | 0.3 ms | 0 MB transient with RGBE 6-word hit packing (or 31 MB at 7 words) |
| Third-signal ray sums | included above | 41 MB transient |
| Cache change counters (8 B), residual staleness, seed record (32 B) and 1-record moving-light indirect (32 B): +72 B/cell (+34% over 212 B/cell) | 0.1 ms | ~75 MB at 1,048,576 cells, ~300 MB at 4,194,304 (verify `PlanGIRadianceCache` doesn't halve capacity) |
| Swept-bounds tests against CPU-filtered moving instances | < 0.1 ms with few moving instances | Shared, bounded journal of swept AABBs and material IDs |
| Glossy change, neighborhood clamp and bilinear-assisted dual history | < 0.1 ms | 0 MB raw (`glossyRaw[i*2+1].yzw` free), < 10 MB total |
| *Follow-up* Stage 9 options, outside the 1.0 ms limit | H1 & H3 ≤ +1.0 ms motion / ~0 static; H2 ≤ +2.0 ms; H5 ≤ Stage 8; H4 separate milestone | H1: ~50 MB diffuse history (32,400 slots). H3/H5: 150–215 MB persistent VRAM (watch world-cache VRAM budget). H2 at 4 px: ~1 GB. H4: depends on choice |

## Risks

- **World-cache vs probe lag mismatch or sparse-cell staleness.** Without `WorldLookup` residual-change
  propagation and timestamp catch-up against `lastSample`, probes freeze stale cache values after discrete
  edits and sparse cells never recover. Stages 1–3 test toggles, stepped objects, material edits and indoor
  sky-bounce rooms specifically to expose this.
- **Shadow-ray cost in `ScreenProbeShadeHits`.** Today `ScreenProbeShadeHits` traces shadow rays only on rare
  cache misses, and `CachedLightOccluded` skips `cameraLight` and directional lights. An unculled `Movable`
  lamp or rotating sun could blow past +0.3 ms; Nsight checkpoints at Stages 1 and 4 catch this early.
- **VRAM headroom halving `WorldRadianceCache` capacity.** Driver memory budgets already dropped some RTX 5080
  runs from 4M to 1M cells (`FastTurnImplementation.md:59`). Adding +72 B/cell (+34%), +52 MB probe history
  and optionally +150–215 MB (H3/H5) can halve capacity; every stage checks `worldCacheCapacity`.
- **A probe's rays miss what changed.** History then lags. Pooling 25 probes, updating analytic top-8 light
  guides immediately, and holding change over frames reduce this. The column, object and energy checks expose it.
- **Swept bounds are conservative.** A large moving object can shorten history in a wide region. The object
  scenario measures this, and per-probe validation rays (Lumen's approach) are the fallback.
- **Reflections near moving lights remain unsplit in Stage 4.** Rough and glossy reflections near the light
  ball shorten their shared history instead of isolating settled reflections; check glossy flicker in Scenario 1.
- **The cache prior is biased and may be cold on disocclusions.** `WORLD_SEED_RECORD` stores shading-normal
  scalar irradiance (no L1 SH) and newly uncovered rooms may miss warm cells even with parent fallback. Stage 0
  measures warm hit rate before Stage 5 invests in it; the age-binned gate catches bias.
- **Additive `ΔSH` noise or clamping bias.** Correlated differences can go slightly negative on rare high-
  variance samples; clamping and the 512-frame Stage 4 drift test verify energy stays within ±1%.
- **The ball's inverse-square peak.** Light at hits is clamped only at `1e-6` squared distance
  (`scene_lighting.glsl:54`), so a hit at the ball's radius of `0.005` sees 100× the power at `0.05`. Stage 4
  clamps GI evaluation at the ball's radius.
- **Resets also hid changes nothing else saw.** Each trigger keeps its global path behind the
  `voxel_gi_history_response` switch until its scenario passes.
- **Stage 9 risks (if run):**
  - **H4 jitter + velocity scope:** jitter shifts receivers every frame across `ReceiverVisibility`,
    `ScreenProbeSources`, placement, gather, glossy and forward passes, and animated meshes need motion
    vectors.
  - **H1 dynamic pool cost during motion:** adds indirection in Gather and extra work exactly when frames are
    slowest; compare against extra-batch reallocation first.
  - **H5 pixel-history and detail risk:** depends on a full-frame pixel history (a larger one failed here
    before), loses Gather's spatial slope correction, flickers on thin surfaces, and without TAA exposes raw
    1-probe dithering on disocclusions.

## Measurement limits

- The three-pair lamp toggle, camera-age experiments and baseline pan are measured above. The remaining
  scene-class comparisons and release decisions follow the same paired procedure.
- The offline extension covers correlated RGB and directional L1 SH. In a finite-window aperture test,
  scalar angular rescaling produced 32.38% relative coefficient error and same-ray signed reconstruction
  produced 0.384%. Renderer cache coverage and moving occluders are assessed through scene captures.
- Environment, rigid instance and material steps have matched HDR replays. Their far-field RMS remains
  above the limit. One pilot per scene class cannot supply the three paired comparisons required for promotion.
- The cost and memory budget table contains design targets. The Nsight tables contain actual ZenApp timings.
  Vendor upscalers have no matched measurements on this scene. Stage 9 has no qualified core build for comparison.
