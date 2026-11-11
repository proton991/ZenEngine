# ReSTIR GI for the per-pixel HW RT gather — plan

2026-11-10. Status: Phase 0 complete. The core Phase 1–4 code paths are implemented behind
the default-off `restirGI` setting. The verification round is complete, but acceptance gates fail
and the refresh-versus-reset comparison remains undone. The whole plan is not complete. Phase 5
remains the optional Compute extension. Implementation details, deviations and measured gates are
tracked in [ReSTIRGIImplementation.md](ReSTIRGIImplementation.md). Implementation is not acceptance.

The active follow-up accepts the current four-ray Phase 3 quality as the baseline and targets
4K output at 30 FPS within 8 GB dedicated memory. Reduced-resolution reconstructed GI is allowed.
The performance checkpoint was documented and work stopped at the user's request. The tested
RTX 5080 center view reaches 35.8 FPS; sampled dedicated process memory is 7.770 GiB. Actual 8 GB
hardware validation and the quality limits remain open; see the implementation report's 4K section.

Subsequent user requests resumed [unused rendering-code cleanup](RenderingCleanup.md) and
[HW RT camera-motion flicker correction](HWRTFlicker.md). The latter reduces repeated-pose floor
variation by 82% and continuous-motion error by 45% in the tested 4K view. Its final RTX 5080
measurement is 32.6 FPS with 7.809 GiB sampled dedicated process memory. Original plan acceptance
and actual 8 GB hardware validation remain open.

The [editor/HW RT follow-up](EditorHWRTFollowup.md) qualifies the configuration mismatch:
the earlier benchmark disabled normal maps. With normal maps enabled, the clean 4K
fullscreen retest measures 18.5 FPS for half-resolution GI with the original package,
and 35.8 FPS for quarter-resolution GI with the compressed package. A further shading-normal
history correction reduces measured camera-motion variation by 17%; isolated flicker remains.

Phase 0 evidence and reproduction commands are recorded in [ReSTIRGIPhase0.md](ReSTIRGIPhase0.md).

## Why

The per-pixel HW RT gather is noisy wherever indirect light arrives through rare, very bright
paths, and it is the largest cost at 4K. New Sponza shows both. Its 22 imported lamps sit inside
opaque lantern glass, so their indirect light reaches the room only through samples that find a
lantern interior or a gap. Those samples are correct and thousands of times brighter than their
neighbours, so they read as flicker that neither filtering nor longer histories remove without
also removing the light (in screen-probe mode, a neighbourhood clamp that cut specular flicker
13% → 4% removed 49% of the reflected light). The same pattern follows any small bright region: sunlit patches seen through
openings, emissive props.

Measured on the 4K center view (`build/nsight-zenapp-4k-center-20261106/sponza.zenrender`,
`--backend=hardware`, per-pixel gather, 24 lights; mean absolute change over 16 frames after
convergence, relative to the term's mean):

| Term | All 24 lights | Sun only |
|---|---:|---:|
| Final color | 1.28% | 0.24% |
| GI specular (reflected hit radiance) | 29.3% | 8.4% |
| Diffuse bounce (`D_bounce`) | 3.0% | 0.56% |

The same frame costs 95 ms at 3840×2160 on an RTX 5080: HybridTrace 48 ms (4 diffuse rays per
pixel, 6 on uncertain receivers, plus one specular ray), HybridFilter0–4 27 ms, HybridSkySupport +
HybridSkySearch 7.4 ms, HybridTemporal 2.8 ms, SceneLighting 11 ms (ZenApp `--profile`, per-pass
times can overlap). These numbers include the uncommitted light-resampling and memory-pressure
changes of 2026-11-09.

ReSTIR GI reuses the secondary path one pixel found in its neighbours and in later frames, with
resampling weights that keep the estimate unbiased or nearly so. A rare bright path then becomes a
stable contribution instead of a spike, and fewer fresh rays per pixel reach the same quality,
which is the main lever on HybridTrace.

## Scope

In scope:

- Per-pixel gather with HW RT traversal (`hardwareMask` bounce/reflection bits).
- Diffuse ray-hit bounce (`rawBounce`, `D_bounce`) first, then reflected hit radiance on rough
  lobes (`rawSpecular.rgb`).

Out of scope for this plan:

- Sky escapes. Missed rays keep the existing estimator (visibility guide, residual environment,
  per-source rays). They are already well sampled, and keeping them separate preserves the
  `D_sky`/`D_bounce` split used by captures and the ground-truth workflow.
- Screen probe gather. Its counterpart is radiance-guided probe rays, a separate plan.
- Compute (distance-field) traversal. The design does not depend on HW RT except for visibility
  queries; extending it is a later phase.
- Primary direct lighting. It is deterministic and unchanged.

References: Bitterli et al. 2020 (ReSTIR, SIGGRAPH), Ouyang et al. 2021 (ReSTIR GI, HPG), Wyman and
Panteleev 2021 (production ReSTIR, HPG), Lin et al. 2022 (generalized RIS, SIGGRAPH).

## Design

### Sample and target

A sample is one secondary vertex: hit position `x_s`, hit geometric normal `n_s`, and the
outgoing radiance `L_o` from `x_s` toward the receiver, as computed now by `TraceHitRadiance`
(`ray_hit_lighting.glsl`, including the per-hit light resampling).

The diffuse target at receiver `x_q` with shading normal `n_q` is

`p̂_q(y) = luminance(L_o) · max(0, cos(n_q, x_s − x_q)) / π`

without albedo, because `D_bounce` is stored unmaterialed and the deferred pass applies albedo.
The shaded estimate is `f_q(y) · W`, with `f_q` the RGB form of `p̂_q` times binary visibility,
and `W = w_sum / (M · p̂_q(y))` the reservoir's unbiased contribution weight.

### Reservoir layout and memory

One reservoir per pixel, 24 bytes, two copies (previous and current):

| Field | Encoding | Bytes |
|---|---|---:|
| `x_s` | 3 × fp32, scene coordinates | 12 |
| `n_s` + `M` + flags | octahedral 2 × 12 bit, `M` 7 bit, valid bit | 4 |
| `L_o` | RGB9E5 (range to 65408, ample for lantern interiors at about 1e2) | 4 |
| `W` | fp32 | 4 |

The original receiver is not stored: temporal reuse reads it from the existing `Position` history,
spatial reuse from the current G-buffer. At 3840×2160 the two copies take 398 MB; a separate
specular reservoir (Phase 3) adds the same again. The 4K frame already runs in memory-pressure mode
(13.4 GB of 16 GB allocated), so each phase records allocation, and the plan retires
HW-unused transients (`rawBouncePrevious`, `rawReflectionPrevious` are only meaningful with the
Compute radiance cache) before adding the second reservoir.

### Passes (render graph order)

1. **HybridTrace (changed).** Each diffuse ray that hits becomes a candidate with source density
   equal to the existing three-proposal mixture pdf (solid angle). Candidates stream into a fresh
   reservoir with weight `p̂_q / pdf`; escaped and in-domain zero-contribution rays count toward
   `M`, unavailable traces do not. The sky estimate is unchanged. Writes the initial reservoir.
2. **ReSTIRTemporal (new).** Reprojects with `receiverMotion`, accepts the previous reservoir with
   the same surface tests HybridTemporal uses (identity, plane distance, normal agreement), and
   combines it with the initial one. The reconnection shift keeps `x_s` and applies the Jacobian
   `(cos φ_q / cos φ_r) · |x_r − x_s|² / |x_q − x_s|²`. `M` is capped (start at 20 frames' worth
   of candidates). One visibility ray `x_q → x_s` for the reused sample drops paths a moved
   occluder now blocks.
3. **ReSTIRSpatial (new, one or two iterations).** Picks k = 4 random neighbours within a radius
   that scales with resolution (start at 32 px at 1080 lines, 64 px at 4K), skips neighbours
   failing depth/plane/normal tests, re-evaluates each candidate's target at `x_q` with the
   reconnection Jacobian, and traces visibility for the selected sample only. Bias handling starts
   with the simple (biased) 1/M normalisation and moves to pairwise MIS (Lin et al.) if the
   ground-truth bias check below fails, typically as dark halos near contacts.
4. **ReSTIRResolve (new or folded into spatial).** Writes `f_q · W` as the frame's bounce sample
   into `rawBounce` with the existing validity alpha, so HybridTemporal, the filters, captures and
   `D_bounce` keep their meaning.

The reservoir from step 3 becomes next frame's previous reservoir.

### Change handling

- Lighting, surface-material and geometry revisions already reset bounce/reflection histories
  (`tracedLightingChanged`, `geometry_or_opacity`, `provider_changed`). The same resets set `M = 0`.
- `L_o` is stale only under those changes. Phase 2 evaluates refreshing `L_o` of the selected
  sample each frame (one hit shading per pixel) against the reset-only policy, by cost and by a
  moving-light sequence.
- Disocclusion falls back to the initial reservoir alone, as it does now.

### Specular (Phase 3)

Reflected hit radiance on lobes with roughness ≥ 0.3 (tunable) uses a second reservoir with target
`luminance(L_o · f_GGX · cos)` at the receiver's view direction, the same reconnection shift, and
neighbour tests that also require a similar roughness. Smooth lobes keep the current one-ray
estimator: reconnection reuse does not apply to near-mirror reflections. This phase targets the
29% specular flicker, the largest per-pixel noise term.

### Ray budget and denoiser (Phase 4)

With reuse in place, the diffuse budget drops from 4/6 to 2 (then 1) rays per pixel, and the
denoiser is retuned: HybridTemporal's bounce variance assumes independent frames, which reused
samples are not, and two of the five à-trous passes may become unnecessary. Each change is
measured against the acceptance criteria, not assumed.

## Verification

| Check | Tool | Acceptance |
|---|---|---|
| Diffuse accuracy | `tools/ground_truth_workflow.py run --dataset newsponza/v3` (20 views) | Per-view `D_bounce` bias within ±1 point of the current per-pixel run; mean relative RMS not worse |
| Lamp transport accuracy | New dataset version with an "imported lamps + sun" lighting setup; high-sample reference on selected pixels (as in `fixes/20261109-hwrt-hit-164136`) | Bias within reference noise |
| Static noise | 16-frame change per term on the 4K center view (the measurement above), all 24 lights | Bounce and specular at or below the sun-only levels (0.6% / 8%) |
| Motion | New deterministic camera path in ZenApp (scripted orbit/dolly), each frame compared with a converged static render at the same pose | No added lag or ghosting versus current per-pixel; numbers reported |
| Dynamic lighting | Moving point light sequence | Reset and recovery within the existing responsive window |
| Performance | `tools/profile_hybrid_gi.py` at 3840×2160 (AGENTS.md profiling rules) | Recorded per phase; Phase 4 target HybridTrace ≤ 25 ms at equal or better noise |
| Correctness | New ZenTest GPU fixtures: reservoir streaming and combination, Jacobian on a known geometry, `M` cap, rejection of foreign surfaces, reset on revision change, convergence to the analytic irradiance of an emissive plane | Pass in all four submission modes with validation |

Every phase reports these numbers against the same baseline build; no phase is accepted on
appearance alone.

## Phases

| Phase | Work | Exit |
|---|---|---|
| 0 — complete | Prerequisite rendering commits landed; noise measurement, camera-path/capture mode and lamp reference tool completed; stale test fixtures repaired. | [Recorded accuracy, noise, motion, memory and 4K time baselines](ReSTIRGIPhase0.md); reference-noise limits retained |
| 1 — implemented, not accepted | Reservoir format, candidate streaming in HybridTrace, ReSTIRTemporal, folded resolve; optional launch settings | GPU contracts pass; temporal-only mean RMS and diffuse-noise gates fail |
| 2 — core implemented, not accepted | Checkerboard spatial reuse, pairwise MIS after the simple estimator lost energy, revision-reset policy for `L_o` | Spatial mean RMS improves; per-view bias-change and diffuse-noise gates fail. Refresh-versus-reset comparison remains undone |
| 3 — implemented, not accepted | Separate rough-specular reservoir and normalized GGX target; smooth gather retained | Specular change is 5.615%, below 8%; inherited diffuse gates still fail |
| 4 — implemented, not accepted | Fixed 4, 2 and 1 budgets measured against the frozen setup; correlation-aware variance; editor switches; default stays off | 2/1 rays meet the trace-time target but fail the RMS gate. Camera/light metrics improve, but eight-frame lighting recovery is not established; default-on remains conditional on every gate |
| 5 (optional) | Compute traversal | Same criteria on the Compute tier |

## Risks and open questions

- **Bias from temporal reuse without `L_o` refresh** under slowly changing lighting that does not
  bump a revision. The measured moving-light path bumps revisions; unreported changes and the
  planned refresh-versus-reset comparison remain unverified.
- **Correlation artefacts** (blotches) from aggressive spatial reuse at low ray counts; mitigated
  by `M` caps, neighbour rejection and the remaining denoiser.
- **Memory at 4K.** Two reservoir types cost about 0.8 GB; the frame already runs memory-limited.
  Phase 1 measures allocation; specular reservoirs may need to run at reduced resolution.
- **Interaction with adaptive budgets and the visibility guide**, which share the diffuse rays
  with the sky estimator. Candidate weights must use the same mixture pdf the sky estimate uses.
- **Lamp reference cost.** A reference for light that leaves sealed lanterns through rare paths is
  itself noisy; it needs many samples on few pixels.
- **Prerequisites landed.** Actual-hit lighting (`a22f4cdf`), one-light resampling (`352c7d7e`) and
  memory-pressure frame limiting (`76d2bac2`) are committed. The introductory measurements above
  remain historical; use the Phase 0 report for the frozen comparison baseline.
- **Test prerequisites repaired.** `RenderCoreTest` links. Phase 0 corrected stale UV-buffer stride,
  forward shader fixture registration and GI allocation-count expectations. All 780 RenderCore
  tests and the scene-triangle regression in all four submission modes pass; see the Phase 0 report.
