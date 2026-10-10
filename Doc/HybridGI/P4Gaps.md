# P4 gaps and remaining tasks

Status on 2026-10-10. The hardware ray-query provider is implemented. Its tests and fixtures pass on an RTX 5080 and an AMD Radeon iGPU, but **no P4 checklist item in the [plan](../HardwareRayQueryEnvironmentLightingPlan.md#delivery-checklist) is ticked**. This page lists what stands between the current working tree and P4 acceptance: the evidence for each gap, the steps to close it, and the [decisions](#decisions-2026-10-10) recorded on 2026-10-10. The implementation record is [P4.md](P4.md).

**Execution result: P4 remains unaccepted.** The final results and reproducible artifacts are in [P4Execution.md](P4Execution.md) and [p4-gap-results.json](p4-gap-results.json). The bounded filter correction reduces maximum full-image/floor bias to 0.46%. FP32 receiver positions and corrected independent references reduce converged failures from four halls to two (hotel 3.87%, qwantani 4.62%); the review's [flat default normal fix](P4Execution.md#flat-default-normal-fix-review-2026-10-10) leaves one: hotel 3.13% excess, against 3%. All five gating halls still fail the shipping gate. Provider-switch settling and large-coordinate rendering also fail. Failed estimator and precision probes were reverted; no preset or error limit was relaxed.

The environment tables below preserve the **pre-experiment historical baseline**. Current status is stated in the summary, exit criteria and execution notes; historical numbers in each original gap description explain the work requested. Ground truth judges all nine environments and shipping limits gate five, with all nine reported.

## Summary

| ID | Gap | Blocks | Kind | First step |
| --- | --- | --- | --- | --- |
| [G1](#g1-shipping-variance-in-high-contrast-views) | Still fails all five gating halls; filter, extracted-source and reservoir probes failed | P4e shipping exit; P3 checklist | Open: estimator variance | A passing estimator is still needed; rejected probes and bounds are recorded |
| [G2](#g2-filter-bias-with-small-bright-sources) | Measured full-image/floor bias is at most 0.46% across all nine environments | Bias target met | Implemented: bounded coarse residual | P8 must qualify its cost; it does not close G1 |
| [G3](#g3-converged-reference-at-distant-receivers) | Hotel hall still fails (3.13%); the other 17 views pass | P4e converged exit; ground-truth checklist | Open: receiver/visibility agreement | Continue from fixed-origin Vulkan/Embree agreement and targeted origin factorization |
| [G4](#g4-acceptance-environment-set) | Environment sets for the image gates | Every image gate | Decided | Closed by decision 1: ground truth on all nine, shipping limits on five |
| [G5](#g5-platform-matrix) | RX 7900 XT, a GPU without ray queries and MoltenVK are untested | P4a exit; definition of done 5 | Hardware access | Test when hardware is available; P4 can close with them reported unverified (decision 3) |
| [G6](#g6-acceptance-checks-not-yet-run) | Checks executed; settling and large-origin image gates fail | P4e exit; P4d | Measured, partly open | Fix the reported image failures; continuous deformation remains unverified |
| [G7](#g7-raster-and-ray-alpha-disagreement) | Corrected primary mismatch is 0.88% of hall pixels and 0.19% of top pixels | Open where limits fail | Measurement implemented | Separate-region errors are reported; level-zero alpha remains the contract |
| [G8](#g8-ground-truth-coverage-and-tooling) | Captured-normal support, analytic fixture and two independent Papermill views added | Ground-truth checklist | Implemented with limited coverage | The other eight environments have vertex-normal truth and normal-mapped same-tier gates |
| [G9](#g9-uncommitted-work) | P4 work was uncommitted | — | Process | Closed by decision 7: committed as a checkpoint |

## Exit criteria

| P4 exit criterion | Status | Evidence | Remaining |
| --- | --- | --- | --- |
| P4a: supported, disabled and unsupported devices resolve correctly; the voxel tier starts without query shaders or AS resources when RT is unavailable | Partly met | RTX 5080 and AMD iGPU with RT; `--disable-rt`; capability unit tests | A device without ray queries and RX 7900 XT, reported unverified if unavailable; MoltenVK an open, non-blocking sub-item ([G5](#g5-platform-matrix), decision 3) |
| P4b: AS resources, commands, reflection and descriptors | Met on the tested devices | RenderCoreTest 576, VulkanRHITest 51; the AMD instance-alignment fix | Other devices ([G5](#g5-platform-matrix)) |
| P4c: graph-driven hit, miss, distance, barycentrics and identities correct without validation errors, inline and threaded, with replacement and failure cases | Met on the tested devices | RayQueryIntegrationTest 12 on both GPUs; RenderCoreTest failure and barrier regressions | — |
| P4d: motion, deformation, opacity edits, removal, replacement and geometry outside the voxel volume affect visibility | Met on the tested devices | RayQueryIntegrationTest scene, deformation and empty-scene cases | Measure the alpha-mismatch pixels; level-zero alpha is the contract ([G7](#g7-raster-and-ray-alpha-disagreement), decision 6) |
| P4e: converged RT sky within the converged limits | **Fails** on 1 of 18 views (hotel hall, 3.13%); the other 17 pass, 5 of them on 8×8 blocks (decision 2) | [Flat default normal fix](P4Execution.md#flat-default-normal-fix-review-2026-10-10): all top views and floors pass | [G3](#g3-converged-reference-at-distant-receivers) |
| P4e: shipping preset within the shipping limits on every frozen camera | **Fails** in all five gating environments | [Shipping gate](#shipping-gate) | [G1](#g1-shipping-variance-in-high-contrast-views), [G2](#g2-filter-bias-with-small-bright-sources) |
| P4e: D2 gone (no zero squares; flagpole shadows match the reference) | D2 artifact sign-off complete for the frozen top views | All nine crop sheets inspected; no start-cell zero squares and converged shadow structure agrees. Two isolated zeros meet the crop-specific threshold in tracked environments | Shipping variance still fails some crop P99 values and full-floor gates ([G1](#g1-shipping-variance-in-high-contrast-views)) |
| P4e: provider switching, unsupported capabilities, `--disable-rt`, budget rejection, frames in flight | Partly met; switching image gate **fails** | Both actual transitions reset history to 1; frame-32 quality failures recorded | Runtime settling ([G6](#g6-acceptance-checks-not-yet-run)) |

The P3 checklist item ("temporal accumulation, spatial filtering and specular occlusion pass on all frozen cameras") is blocked by the same reconstruction results as the P4e shipping exit.

## Environment sweep (2026-10-10)

Sponza with the frozen top and hall cameras, 960×540, 64³, hardware provider, environment light only, zero bounce. All eight local Poly Haven panoramas (see [Environments/README.md](../../Data/Textures/Environments/README.md)) plus `papermill.ktx`; the build is unchanged from the [ground-truth comparison](P4.md#ground-truth-comparison-2026-10-09) (executable and all SPIR-V hashes identical).

| Environment | Character |
| --- | --- |
| Papermill | Soft interior daylight (default) |
| Hotel room | Window light and two small, very bright lamps |
| Kloppenheim 06 | Soft sunrise sky |
| Kloofendal 48d | Midday sun with scattered clouds |
| Qwantani noon | Clear midday sun, high contrast (2k) |
| Studio small 09 | Studio softboxes: a few small, very bright sources |
| Small empty room 1 | Soft window daylight |
| Large corridor | Sunlit arched windows |
| Carpentry shop 01 | Artificial lights and doorway daylight |

### Shipping gate

Four diffuse rays after 64 frames, normal maps on, against the 1024-sample × 64-frame tier reference of the same environment. Limits: absolute mean bias ≤ 2%, RMS ≤ 8%, P99 ≤ 20%, no exact zeros where the reference is at least 10% of the region mean.

| Environment | Top, all receivers | Top floor | Hall, all receivers | Hall floor |
| --- | --- | --- | --- | --- |
| Papermill | −0.15% / 1.89% / 6.26% / 0 | −0.43% / 3.23% / 10.75% / 0 | **−0.47% / 5.98% / 21.65% / 10** | −0.47% / 2.40% / 8.23% / 0 |
| Hotel room | −0.18% / 2.79% / 8.83% / 0 | **−0.30% / 6.37% / 21.02% / 0** | **−0.40% / 13.85% / 52.18% / 19** | −0.42% / 7.97% / 18.65% / 0 |
| Kloppenheim 06 | −0.00% / 2.56% / 8.49% / 0 | −0.50% / 5.86% / 18.96% / 0 | **−0.45% / 11.96% / 48.57% / 66** | −0.41% / 5.09% / 17.45% / 0 |
| Kloofendal 48d | −0.10% / 1.47% / 4.73% / 0 | −0.69% / 5.97% / 17.95% / 0 | **−0.41% / 7.93% / 28.53% / 31** | −0.81% / 6.75% / 18.77% / 0 |
| Qwantani noon | −0.13% / 2.99% / 10.18% / 0 | **−1.06% / 7.10% / 23.09% / 0** | **−0.37% / 13.57% / 49.49% / 11** | **−1.43% / 6.68% / 23.61% / 0** |
| Studio small 09 | −0.13% / 3.51% / 10.85% / 0 | **−1.65% / 6.41% / 20.62% / 0** | **−0.45% / 31.84% / 88.82% / 60** | −1.99% / 5.04% / 14.31% / 0 |
| Small empty room 1 | +0.19% / 4.48% / 15.57% / 0 | −0.71% / 5.53% / 16.67% / 0 | **−0.55% / 12.22% / 45.36% / 59** | −0.76% / 5.32% / 16.07% / 0 |
| Large corridor | −0.10% / 3.14% / 10.42% / 0 | **−1.02% / 7.06% / 23.13% / 0** | **−0.67% / 13.85% / 48.71% / 55** | −0.84% / 5.62% / 18.91% / 0 |
| Carpentry shop 01 | −0.08% / 2.81% / 9.59% / 0 | −0.08% / 3.29% / 12.44% / 0 | **−0.39% / 13.86% / 57.48% / 18** | **−0.36% / 6.49% / 27.43% / 0** |

Bias / RMS / P99 / zeros; **bold** fails. Every hall view fails, and Papermill is the mildest. The limits gate Papermill, kloppenheim 06, qwantani noon, hotel room and carpentry shop 01 (decision 1); the other four are tracked.

Error attribution (luminance, from the same captures; the temporal stage's unfiltered mean is captured in the moments target):

| Environment | Hall: temporal bias / RMS | Hall: filtered bias / RMS | Worst floor: filtered bias | Hall top-1% errors on fine geometry |
| --- | --- | --- | --- | --- |
| Papermill | −0.01% / 25.5% | −0.47% / 5.8% | −0.47% (hall) | 14% |
| Hotel room | +0.00% / 37.4% | −0.40% / 12.9% | −0.41% (hall) | 11% |
| Kloppenheim 06 | −0.03% / 43.6% | −0.43% / 11.8% | −0.49% (top) | 14% |
| Kloofendal 48d | −0.02% / 26.1% | −0.40% / 7.7% | −0.83% (hall) | 13% |
| Qwantani noon | −0.01% / 45.3% | −0.35% / 13.7% | −1.44% (hall) | 8% |
| Studio small 09 | +0.08% / 82.7% | −0.45% / 31.8% | −1.98% (hall) | 4% |
| Small empty room 1 | −0.03% / 49.4% | −0.55% / 12.1% | −0.77% (hall) | 16% |
| Large corridor | +0.01% / 47.1% | −0.68% / 13.0% | −1.02% (top) | 11% |
| Carpentry shop 01 | −0.01% / 41.5% | −0.39% / 13.4% | −0.35% (hall) | 7% |

- **Temporal accumulation is unbiased** everywhere (|bias| ≤ 0.11%). All bias comes from the spatial filter ([G2](#g2-filter-bias-with-small-bright-sources)).
- **The hall's per-pixel variance is the problem.** Before filtering, hall RMS is 26–83% of the mean; the filter reduces it 2.6–4.4×, which is enough only for Papermill-like environments.
- **Fine geometry is not the main source outside Papermill.** Pixels whose neighbors mostly differ in geometric normal are 2% of the hall and hold 4–16% of its largest errors; the rest are ordinary surfaces — columns, walls, curtains, floor — that see bright sources through the arcades.

### Ground truth

Normal-map-free captures of the same views against Mitsuba 3 (CUDA, 32768 samples per pixel). *Excess* is the RMS error with the ground truth's own noise removed. Rung-3 limits for the tier reference: absolute bias ≤ 1%, excess ≤ 3%, per pixel and on 8×8 blocks; where the per-pixel excess is within the limit but the ground truth's own noise exceeds it, the blocks decide (decision 2). *Near* and *far* split receivers at 0.3 normalized units from the camera; *far* is 13% of hall pixels and 20% of top pixels.

| View | Ground-truth noise | Tier reference: bias / excess / 8×8 excess | Floor: bias / excess | Near / far excess | Shipping: bias / excess | Tier reference |
| --- | ---: | --- | --- | --- | --- | --- |
| Papermill top | 0.51% | −0.12% / 0.22% / 0.17% | −0.05% / 0.35% | 0.24% / 0.10% | −0.28% / 1.39% | Pass |
| Papermill hall | 2.70% | −0.35% / 0.99% / 0.63% | −0.08% / 0.35% | 0.91% / 1.43% | −0.75% / 5.06% | Pass |
| Hotel room top | 0.58% | −0.10% / 0.54% / 0.25% | +0.21% / 1.51% | 0.58% / 0.35% | −0.33% / 1.94% | Pass |
| Hotel room hall | 4.12% | −0.35% / **7.82%** / 2.18% | −0.08% / 0.91% | 0.89% / **21.84%** | −0.74% / 15.39% | **Fail** |
| Kloppenheim 06 top | 0.39% | −0.06% / 0.21% / 0.18% | +0.01% / 0.53% | 0.23% / 0.07% | −0.05% / 1.89% | Pass |
| Kloppenheim 06 hall | 3.35% | −0.32% / 1.63% / 0.90% | −0.15% / 0.58% | 0.85% / **3.99%** | −0.74% / 11.37% | Pass on 8×8 blocks (per-pixel noise 3.35%) |
| Kloofendal 48d top | 0.43% | −0.11% / 0.46% / 0.37% | −0.00% / 0.50% | 0.50% / 0.22% | −0.23% / 1.05% | Pass |
| Kloofendal 48d hall | 2.49% | −0.42% / **5.53%** / 2.30% | −0.08% / 0.57% | 1.04% / **15.30%** | −0.79% / 9.35% | **Fail** |
| Qwantani noon top | 0.39% | −0.13% / 0.51% / 0.42% | +0.01% / 0.68% | 0.55% / 0.24% | −0.29% / 2.10% | Pass |
| Qwantani noon hall | 2.20% | −0.47% / **9.69%** / 3.89% | −0.12% / 0.73% | 1.43% / **26.97%** | −0.83% / 16.72% | **Fail** |
| Studio small 09 top | 0.77% | −0.01% / 0.38% / 0.30% | −0.03% / 1.86% | 0.42% / 0.06% | −0.15% / 2.93% | Pass |
| Studio small 09 hall | 11.08% | −0.18% / 2.78% / 1.94% | −0.11% / 2.05% | 0.44% / **7.73%** | −0.65% / 31.38% | Pass on 8×8 blocks (per-pixel noise 11.08%) |
| Small empty room 1 top | 0.53% | +0.00% / 0.40% / 0.36% | +0.01% / 0.50% | 0.45% / 0.08% | +0.07% / 2.99% | Pass |
| Small empty room 1 hall | 3.10% | −0.34% / 1.41% / 0.78% | −0.11% / 0.50% | 1.02% / 2.92% | −0.90% / 11.15% | Pass on 8×8 blocks (per-pixel noise 3.10%) |
| Large corridor top | 0.66% | −0.03% / 0.56% / 0.51% | −0.06% / 0.63% | 0.63% / 0.08% | −0.18% / 2.46% | Pass |
| Large corridor hall | 4.33% | −0.29% / 1.59% / 0.88% | −0.15% / 0.61% | 0.87% / **3.83%** | −0.98% / 13.11% | Pass on 8×8 blocks (per-pixel noise 4.33%) |
| Carpentry shop 01 top | 0.68% | −0.01% / 0.43% / 0.39% | +0.12% / 0.54% | 0.48% / 0.12% | −0.15% / 2.18% | Pass |
| Carpentry shop 01 hall | 5.38% | −0.20% / **5.92%** / 2.26% | +0.09% / 0.91% | 3.88% / **13.15%** | −0.58% / 14.03% | **Fail** |

- **Every top view and every floor region passes**, and tier-reference bias is within ±0.5% on every view.
- **Hall views:** Papermill passes. The hotel room, qwantani noon, the carpentry shop and kloofendal fail per pixel (excess 5.5–9.7%). Kloppenheim, the studio, the small empty room and the corridor pass on 8×8 blocks: their per-pixel excess is within the limit, but the ground truth's own noise exceeds it (11% for the studio, whose softboxes stay noisy at 32768 samples), so the blocks decide.
- **The disagreement is at distance.** Beyond 0.3 units the excess is 2.9–27.0% in every hall except Papermill's (1.4%). Within 0.3 units it is at most 1.43%, except the carpentry shop (3.9%). See [G3](#g3-converged-reference-at-distant-receivers).
- **Shipping against the ground truth** agrees with the shipping gate: hall excess 5.1–31.4%, top views 1.1–3.0%.
- The Embree visibility cross-check agrees within noise on every view. It compares the cosine-weighted unblocked fraction at 256 receivers, which does not depend on the environment, so it is identical across environments.
- Rasterization and rays see different surfaces at 2.0% of hall pixels and 0.35% of top pixels; these are excluded ([G7](#g7-raster-and-ray-alpha-disagreement)).

## G1 Shipping variance in high-contrast views

**Execution status:** open. Global and isolated-face filter probes, directional collapse, finite-source quadrature with separate source-group reconstruction, and a bounded reservoir fallback were evaluated and rejected. Seven of the ten gating camera/environment pairs fail at least one full-image/floor shipping limit. Four rays, 32-frame history and the five existing filter iterations remain frozen; no P8 preset change is adopted. See [rejected probes](P4Execution.md#estimator-probes-rejected).

**Gap.** At four rays, 32 history frames and five filter iterations, every hall view fails the full-image limits (RMS up to 31.8%, P99 up to 88.8%, up to 66 exact zeros), and floors fail P99 under the noon sun, the corridor, the hotel lamps, the studio and the carpentry shop.

**Cause.** Hall receivers reach the sky through the arcades. With small bright sources, a single ray's contribution varies by orders of magnitude, so the 128 samples a pixel accumulates (4 rays × 32 frames) leave temporal RMS of 26–83%. Neighbors cannot always be averaged: they differ in visibility, which is the signal.

**Why brute force does not close it.** Eight rays per pixel passed Papermill's hall (P99 18.27%, measured before the receiver normal fix), but the studio hall needs RMS reduced about 4×, roughly 16× the samples by √N scaling. More rays or a longer history fix only Papermill-like environments.

**Steps.**

1. Judge every step on the five gating environments and report the four tracked ones ([G4](#g4-acceptance-environment-set)).
2. *Quick filter experiment (keeps every frozen setting).* For the demodulated sky, drop the hard geometric-normal rejection (`dot < 0.9`) and soften the `pow(dot, 32)` shading-normal weight, keeping the plane-distance and identity tests; on pixels whose neighbors all differ in geometric normal, use a smoother normal guide. This follows NRD's documented practice for demodulated signals and sub-pixel geometry. Expected to close Papermill's fine-geometry share (about 14% of its hall's largest errors and its exact zeros), not the other environments. The earlier finding that relaxing normal tests moved P99 by under 0.4 points predates the [receiver normal fix](P4.md#defects-found).
3. *Handle small bright sources explicitly* (decision 4). Prototype bright-source extraction first, on the worst gating views (qwantani, hotel and carpentry halls) with the studio as a stress case; use reuse only if extraction falls short:
   - **Bright-source extraction.** Split the environment's few brightest compact regions (sun, lamps, softboxes) into explicit directional or small area lights with dedicated visibility rays and their own reconstruction; leave the smooth residual to the sky estimator. Removes most of the variance at its source. Its shadow rays are pulled into P4 from P7. If it is adopted, the lighting contract splits `D_sky` into the extracted sources and the residual environment, and the plan is amended before results are evaluated.
   - **Fallback: spatiotemporal reuse of escaped directions** (ReSTIR-style resampling: Bitterli et al. 2020; Ouyang et al. 2021). Keeps four rays; reuses directions that reached the sky from neighbors and earlier frames, re-testing visibility. Larger estimator change; must stay unbiased within 1% or carry a documented bound.
4. *Preset levers, deferred until step 3 is measured* (decision 5). An 8-ray RT preset and a 64-frame history with a 4–6-frame fast history (as in NRD and the RTXGI sample, which runs 60 frames) are preset changes for P8 to cost. The 64-frame history needs a written P0 rationale before it is evaluated and must keep the 32-frame settling limits.

**Done when** the shipping gate passes on both frozen cameras in the five gating environments ([G4](#g4-acceptance-environment-set)), with the tracked four reported and the ground-truth shipping bias consistent with the tier-reference comparison.

## G2 Filter bias with small bright sources

**Execution status:** the bias target is met. The retained bounded coarse residual correction keeps every measured full-image/floor region across all eighteen views within ±0.46%, with no floor zeros in the final sweep. It adds seven transient RGBA32F targets before graph reuse. Performance qualification remains P8 work.

**Gap.** The filtered sky is darker than the reference in almost every region, by up to 1.99%, worst on floors under small sources: studio hall floor −1.99% against the 2% limit, studio top floor −1.65%, qwantani hall floor −1.43%, corridor top floor −1.02%. Unfiltered temporal output is unbiased on the same pixels (|bias| ≤ 0.11%).

**Cause.** The 1σ luminance edge-stopping weight rejects rare bright samples more often than dark ones, so the weighted mean is pulled down. Small bright sources make such samples common. (The same mechanism gave −7.5% when filtered output was fed back into history; that option was rejected in the [hall work](P4.md#hall-full-image-reconstruction-2026-10-09).)

**Steps.**

1. Add a low-frequency bias correction after the à-trous passes: add back a heavily blurred difference between the temporal mean and the filtered result (both demodulated), using only geometry-safe weights (plane, identity). Edge-preserving detail stays; the energy the luminance test removed returns at a coarse scale.
2. Re-measure bias on all floors and full images in the environment set; target |bias| ≤ 1% for margin.
3. Variance reduction from [G1](#g1-shipping-variance-in-high-contrast-views) lowers the bias by itself, because fewer samples are outliers.

**Done when** every region of the five gating environments is within ±1% bias after filtering (limit ±2%); the tracked four are reported.

## G3 Converged reference at distant receivers

**Execution status:** improved, still open. Review fix: materials without a normal map were shaded 0.32° off their vertex normal by the 8-bit default normal texture; with that fixed, qwantani passes (2.36%) and hotel is the only failure (3.13%, 8×8 excess 0.51%); see the [fix](P4Execution.md#flat-default-normal-fix-review-2026-10-10). Earlier in the execution: Independent alpha continuation and primary-ray construction were corrected. Fixed-origin Vulkan and Embree agree on 619,804 visibility queries. The retained FP32 position target passes all controlled near/middle/far bright-edge fixtures and reduces the failing Sponza set to hotel (3.87%) and qwantani (4.62% excess). Receiver-plane and derivative pixel-center probes were reverted. The original diagnosis below predates this evidence; see [final arbitration](P4Execution.md#independent-arbitration-and-precision).

**Gap.** The tier reference (1024 samples × 64 frames, no reconstruction) should match the ground truth within 1% bias and 3% excess. It does within 0.3 units of the camera on every view except the carpentry shop's hall, but not beyond 0.3 units when sources are small. Four hall views fail per pixel (qwantani noon 9.69%, hotel room 7.82%, carpentry shop 5.92%, kloofendal 5.53% excess) and four more pass only on 8×8 blocks. Beyond 0.3 units the excess is 2.9–27.0% in every hall except Papermill's; within 0.3 units it is at most 1.43%, except the carpentry shop (3.9%). All top views and all floor regions pass.

**What is known** (one-off luminance diagnostics of the hotel and kloofendal halls, 2026-10-10; not part of the sweep report):

- **It is repeatable, not noise.** The engine reference's own noise beyond 0.3 units is 2.4–2.6% (from an independent second 64-frame reference), and that independent half shows the same 23% and 19% excess against the ground truth in the 0.30–0.45 and ≥ 0.45 bands.
- **It is per pixel, not a scale error.** Signed bias beyond 0.3 units is −0.3% to −0.5% in the four halls measured (Papermill, hotel room, kloppenheim, kloofendal).
- **It sits on sharp lighting detail.** In the hotel hall, 94% of the far excess is on ground-truth shadow edges (pixels that differ from a neighbor by more than 50%); under the kloofendal sun, 43%. The error maps show speckle of both signs over the lamp- or sun-lit far arcades and the foliage, and under kloofendal a contiguous darker patch on the sunlit upper facade at the far end.
- **Depth precision is not the main cause.** Engine receivers agree with the ground truth's primary hits within one depth quantum (median 7.2e-6 against 9.2e-6 units), and the top view's receivers beyond 0.3 units, seen head-on, pass under the same sources (excess at most 0.32%). Grazing view angles and sub-pixel geometric detail are common to the failing pixels. Shading the engine's own receivers in Mitsuba, with the engine's origin offsets, halved the hotel hall's error beyond 0.45 units but not between 0.30 and 0.45.
- **Neither side is independently confirmed there.** The Embree cross-check compares the cosine-weighted unblocked fraction `nu`, which small sources barely affect.

**Steps.**

1. *Arbitrate with a third tracer.* Give the Embree sky reference ([environment_reference.py](../../tools/environment_reference.py)) environment importance sampling, or exact integration over the brightest texels, and evaluate a few hundred far pixels with the largest disagreement. Whichever of the engine and Mitsuba it matches is right; nothing is changed before this.
2. *If the engine is wrong:* add a debug output of the engine's per-pixel ray origin, geometric normal and visibility toward fixed bright directions, and compare with Embree from the same origins. Candidates: the origin offset at grazing angles, alpha-masked candidates at texture level zero, and the ray range.
3. *If the ground truth is wrong:* fix the tool (origins, normals, alpha) and rerun the sweep.
4. *Fixture.* A sub-degree source behind an occluder edge, with receivers at controlled distances and grazing angles; engine reference against Mitsuba and the analytic shadow edge.
5. *Receiver precision* (reversed-Z, or an FP32 position target at 16 bytes per pixel) only if steps 1–2 implicate the receiver position.

**Done when** the tier reference passes rung 3 on both cameras in all nine environments, under the noise rule of decision 2.

## G4 Acceptance environment set

**Decided on 2026-10-10** (decision 1). P0 froze the cameras but no environments, and the verdict depends almost entirely on the environment. The ground truth (rung 3) covers all nine: a wrong converged answer is a defect under any of them. The shipping limits, a noise budget, gate five, one per lighting type: Papermill (soft interior daylight), kloppenheim 06 (soft sky), qwantani noon (hard sun), hotel room (interior with small lamps) and carpentry shop 01 (artificial lights with daylight). Studio small 09, small empty room 1, large corridor and kloofendal 48d are tracked and reported but do not gate; they repeat those types or, like the studio's softboxes, are stress cases. Recorded in the plan's P0 section and in `baseline.json` (`environments`, with file hashes).

## G5 Platform matrix

**Execution status:** all 46 native FP32-layout cases pass (13 hardware plus 10 voxel on each available GPU); twelve ray-query integration cases pass on each. RTX 5080 driver 617.14 and the local AMD Radeon iGPU were exercised. RX 7900 XT, a non-query GPU and Apple Silicon/MoltenVK remain unverified under decision 3.

**Gap.** P0's matrix requires RTX 5080 and RX 7900 XT, each with RT on and with `--disable-rt`; macOS on Apple Silicon through MoltenVK (recording ray-query availability); and a GPU without ray queries if available. Only the RTX 5080 and an AMD Radeon iGPU have been tested. P0 says untested platforms are reported as unverified; P4a says MoltenVK must be checked explicitly.

**Steps.** On each available device: the unit and integration suites, the 13 hardware and 10 voxel fixtures, the sky-cache check, and the Sponza captures. On a device without ray queries: the voxel tier must start without loading query shaders or allocating AS resources. Record each device and driver.

**Decided** (decision 3). P4 can close with the RX 7900 XT and a GPU without ray queries reported as unverified; MoltenVK stays an open P4a sub-item, recorded but not blocking. Each is still tested when the hardware becomes available.

Recorded separately and outside P4 code: fixture and capture runs need `--allow-present-baseline` for the known `SYNC-HAZARD-PRESENT-AFTER-WRITE`, which `legacy` reproduces too; on the AMD iGPU the compact G-buffer derivative-normal test fails (non-hybrid voxel path) and voxel cone bounce is about 26% lower than on the RTX 5080.

## G6 Acceptance checks not yet run

The heading is retained for existing links. These checks have now been run; failures remain open.

| Check | Requirement | Result and remaining work |
| --- | --- | --- |
| D2 flagpole shadows | P4e exit | All nine crop sheets reviewed; the specific zero-square defect is absent and converged shadow structure agrees. Shipping variance is reported separately |
| Runtime provider switching | P4e exit | Hardware → voxel → hardware resets histories correctly; frame-32 full-image/floor limits still fail. See the final transition table |
| Near, middle and far origins | P4 open item | Controlled bright-edge and translated-AS fixtures pass. Sponza at 100 units has 420 unexpected zeros; at 10,000 units full-image RMS is 75.53% with 177,691 zeros. Large-world rendering remains open |
| Material matrix | P4d | All seven requested alpha variants pass native/independent comparisons; largest primary mismatch approximately 0.027% |
| Image stability under motion | Definition of done 2; P3 exit | Motion, cut, one-shot deformation and alpha-edit sequences captured at frames 1/4/8/32 with matching references. Several frame-32 limits fail. Continuous deformation and every-frame moving-shadow behavior remain unverified |
| D5 voxel sky cache | Plan defect D5; definition of done 4 | Capture-only counter implemented: 0 center fallbacks / 22,997 evaluated occupied voxels on Sponza, both providers. Ownerless fallback policy remains relevant on other geometry |

## G7 Raster and ray alpha disagreement

**Execution status:** measured in the final report. Corrected primary rays reduce mismatches to 984/518,400 top pixels (0.19%) and 4,563/518,400 hall pixels (0.88%), with no missing independent primary hits. These regions fail local error limits but account for at most 3.74% of whole-image shipping squared RGB error. Their errors combine surface disagreement and reconstruction variance; the result does not isolate texture LOD as the cause. Level-zero alpha remains unchanged, and LOD matching remains open rather than being marked unnecessary or passed.

**Gap.** At 2.0% of hall pixels and 0.35% of top pixels, the primary surface the rasterizer drew differs from the one a ray finds through the pixel center. Most are alpha-masked foliage: the rasterizer tests alpha at a mip level, rays test it at level zero (compute shaders have no derivatives). The rest are silhouettes. These pixels are excluded from the ground-truth comparison.

**Steps.** Measure the shipping and reference error on those pixels separately. If it matters, give the ray candidate test a texture LOD from a ray cone (Akenine-Möller et al., *Improved Shader and Texture Level of Detail Using Ray Cones*, 2021), or test the receiver's own alpha at level zero in the prepass.

**Decided** (decision 6). Alpha acceptance follows rasterization's rule but at texture level zero; pixels where the two see different surfaces are reported as primary mismatches, and LOD matching is revisited only if they break a limit. The measurement above remains to be done.

## G8 Ground-truth coverage and tooling

**Execution status:** captured-normal support and its analytic tilted-normal fixture are implemented. Captured half-precision normals are renormalized before cosine sampling and fingerprinted in the dataset. Two full independent Papermill normal-mapped views are checked; sixteen other views retain vertex-normal independent coverage. The corrected vertex-normal dataset contains all eighteen views at 32,768 samples. See [coverage and results](P4Execution.md#native-acceptance-checks).

- **Normal maps.** The ground truth uses vertex normals (`--strip-normal-maps`), so the normal-mapped shading path is not checked against it. Add tangent-space normal mapping to the Mitsuba scene (its `normalmap` BSDF, with tangents matching the engine), or evaluate the ground truth at the engine's captured shading normals.
- **Ground-truth noise** (decided, decision 2). Four hall views have per-pixel ground-truth noise of 3.1–11% at 32768 samples; resolving them per pixel would take 1.1–14× the samples. A per-pixel or 8×8 excess above the limit fails; where the per-pixel excess is within the limit but the noise exceeds it, the blocks decide. Pixels under diagnosis ([G3](#g3-converged-reference-at-distant-receivers)) get targeted high-sample renders.
- **Bounce** (P5) and final-image validation are not covered yet.
- **Tooling.** Done on 2026-10-10: the ground truth is a stored dataset and [ground_truth_sweep.py](../../tools/ground_truth_sweep.py) regenerates and compares the engine captures (see [Reproduction](#reproduction)); `ground_truth_mitsuba.py` now reports the backend it uses and why it fell back. One render takes about 10 minutes at 32768 samples on the RTX 5080 with the GPU mostly idle (the batch loop is latency-bound); four render concurrently in about 12.5 minutes, about 3 minutes per view. The tools need the Python environment from `tools/requirements-gi-quality.txt` (the system Python here has no NumPy).

## G9 Uncommitted work

Closed by decision 7. The P4 work since 555e3748 (review fixes, hall reconstruction, receiver normal fix, ground-truth tools and these documents) is committed on `codex/hybrid-gi-p0-p3` as a checkpoint before the experiments, so each fix that follows is a reviewable change.

## Decisions (2026-10-10)

Taken by the project owner on 2026-10-10, accepting the recommendations made with this review. Changes to the frozen P0 configuration are also recorded in the plan and `baseline.json`, as P0 requires before candidate results are evaluated.

| # | Decision | Choice | Why | Recorded in |
| --- | --- | --- | --- | --- |
| 1 | Environments for the image gates | Ground truth on all nine. Shipping limits on Papermill, kloppenheim 06, qwantani noon, hotel room and carpentry shop 01. Studio small 09, small empty room 1, large corridor and kloofendal 48d tracked, not gating | A wrong converged answer is a defect in any environment; the shipping limits are a noise budget, checked once per lighting type | Plan P0 item 2; `baseline.json` `environments`; [G4](#g4-acceptance-environment-set) |
| 2 | Noisy ground truth | An excess above the limit fails, per pixel or on 8×8 blocks. Where the per-pixel excess is within the limit but the ground truth's per-pixel noise exceeds it, the blocks decide; the per-pixel result is reported. Targeted high-sample renders for pixels under diagnosis | Blocks still expose systematic error and misplaced shadows at low noise; resolving the studio hall per pixel would take about 14× the samples | Plan ground-truth rules; `baseline.json` `ground_truth`; `ground_truth_sweep.py` verdicts; [G8](#g8-ground-truth-coverage-and-tooling) |
| 3 | Untested platforms | P4 can close with the RX 7900 XT and a GPU without ray queries reported as unverified; MoltenVK stays an open P4a sub-item that does not block P4 | The hardware is unavailable; P0 already reports untested platforms as unverified | Plan P0 item 6; [G5](#g5-platform-matrix) |
| 4 | Small bright sources | Prototype extraction into explicit lights with dedicated shadow rays first (pulled into P4 from P7); ReSTIR-style reuse only as a fallback | Removes most of the variance at its source; more rays or history cannot close the studio-like cases | Plan P7; [G1](#g1-shipping-variance-in-high-contrast-views) step 3 |
| 5 | Preset changes (8 rays, 64-frame history) | Deferred until step 3 of G1 is measured; each then needs a written P0 rationale and P8 costing | Frozen settings should not move before the variance fix is known | [G1](#g1-shipping-variance-in-high-contrast-views) step 4 |
| 6 | Texture LOD for alpha | Level zero stays the contract; mismatches are reported and revisited only if they break a limit | Compute shaders have no derivatives; the mismatch is 2% of hall pixels | Plan P4d; [G7](#g7-raster-and-ray-alpha-disagreement) |
| 7 | Uncommitted work | Commit now, as a checkpoint before the experiments | Makes each following fix reviewable | [G9](#g9-uncommitted-work) |

## Order of work

### Execution notes (2026-10-10)

The execution record is [P4Execution.md](P4Execution.md). The original sweep tables above remain historical baseline results; they are not the current build or corrected-reference verdict.

- G3's Embree environment-MIS tiebreaker found a ground-truth defect: alpha rejection used Mitsuba's normal-offset `spawn_ray`, changing the line of sight through nearby foliage. Direction-preserving continuation fixes this, passes the twelve original fixtures and a new 20-micro-unit separated-sheet regression. The intermediate immutable dataset is `build/ground-truth/dataset-alpha-continuation`; the old dataset is preserved. At that stage hotel and qwantani halls still failed (6.49% and 9.74% excess). Final results additionally include corrected primary rays and FP32 receivers.
- Embree at the engine's receivers reproduces the engine much more closely than at exact primary hits. Merely projecting the depth-error offset onto the normal reduced the targeted hotel disagreement only from 0.08010 to 0.07637 absolute RGB RMS and is not adopted. **Next receiver experiment, recorded before evaluation:** store the camera-relative receiver-plane constant in the existing FP32 motion target's unused fourth component, then intersect the pixel ray with that plane. This tests receiver precision without adding the proposed 16-byte position target. Exact raster positions permit the existing small transform/ULP origin offset without the two-depth-step displacement. Keep depth reconstruction for callers without the new plane data and measure the experiment against the corrected ground truth before adopting it.
- G2's coarse demodulated residual correction brings all nine environments' measured full-image and floor biases below 0.5%. Its negative correction is bounded to preserve positive resolved samples. Five edge-stopping iterations, four diffuse rays and 32 history frames remain unchanged.
- Both global soft-normal and isolated-face filter probes reduced exact zeros but worsened errors elsewhere; neither is adopted.
- Bright-source probes were measured on qwantani, hotel, carpentry and studio halls. Directional collapse fails due to finite-source shadow error. Finite-source quadrature plus residual sampling improves the first three but still fails the gates; it remains an opt-in tool, not a lighting-contract change.
- G6: exported sky-cache counts show 22,997 evaluated occupied Sponza voxels and zero center fallbacks. Runtime hardware/voxel/hardware captures show both resets to length 1. The seven alpha-material fixtures and the captured-normal-map fixture have native captures and independent comparisons. Final quality failures and coverage limits are recorded in P4Execution.md.
- The receiver-plane probe still fails (hotel 3.92%, qwantani 4.23% excess with matched offsets); it is reverted. Vulkan and Embree agree on all 619,804 fixed-source visibility rays from identical origins, and the targeted MIS comparison reproduces Vulkan at its own receivers. **Next precision experiment, recorded before evaluation:** use the G3-authorized RGBA32F raster-position target (16 additional bytes/pixel, 64-byte hybrid G-buffer), avoiding inverse-depth reconstruction and its two-depth-step origin displacement for hardware rays. The voxel provider retains its existing reconstruction. This is an evidence-driven receiver experiment, not an accepted P0/preset change; keep it only after the corrected ground-truth comparison and relevant native checks.

- **Pixel-center experiment, recorded before evaluation:** the final 128-pixel arbitration shows FP32 raster positions reprojecting at median 0.00096 pixels (hotel) and 0.00082 (qwantani), versus about 0.000009 for independent primary hits. Their median normal-plane error is at most 4e-9 world units. Test a first-order correction along fragment position derivatives from the interpolated projected coordinate to the actual pixel center. The existing two reserved receiver push-constant words carry the viewport extent; no target, ray, history or limit is added. Keep only after independent comparisons and native checks.
  Result: reprojection improved, but hotel/qwantani excess remained 3.85%/4.52%; the correction was reverted. The confirmed FP32 target remains without that correction.

1. G3 step 1 (Embree tiebreaker) and G2 (bias correction): small and independent.
2. G6 checks.
3. G1 step 2 (filter experiment).
4. G1 step 3: the bright-source extraction prototype, with reuse as the fallback; then decision 5's preset question, if still needed.
5. The G3 steps the tiebreaker points to.
6. G8 normal maps.
7. Acceptance run: `ground_truth_sweep.py` on all nine environments, with the shipping gate judged on the five.

## Reproduction

The eighteen corrected ground-truth renders are stored in `build/ground-truth/dataset-p4-corrected/` (ignored by Git): `truth-<environment>-<camera>.npz/.json`, the fixture-gate report they were validated with, and `dataset.json`, which records each view's scene/environment hashes, camera matrix and captured environment cube. The original `build/ground-truth/dataset/` is preserved for the historical tables above. Engine captures are regenerated by [ground_truth_sweep.py](../../tools/ground_truth_sweep.py), which refuses a capture that no longer matches its render:

```powershell
python tools/ground_truth_sweep.py --exe build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe --scene PATH/TO/Sponza/glTF/Sponza.gltf --dataset build/ground-truth/dataset-p4-corrected --output build/ground-truth/runs/NAME
```

It captures all nine environments and both cameras (72 captures, about 12 minutes) and writes `report.json` and `summary.md`; the tables above come from `build/ground-truth/runs/2026-10-10/report.json`. New views need `--render-missing` and a passing `--fixture-gate`; see [P4.md](P4.md#reproduction).
