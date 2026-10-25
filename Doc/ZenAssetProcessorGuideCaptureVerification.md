# Deterministic learned-guide capture checkpoint

24 October 2026. Opt-in exact guide accumulation now passes the bounded cooked
spatial GI comparisons and the focused BC7 source/reference GI comparisons.
Guide learning remains enabled. Normal rendering keeps its existing FP32
accumulation and cost. This closes the repeatability gap for these captures,
not the complete M5/M6 quality, atlas, performance or release gates.

## Implementation and use

`scene_renderer_demo --deterministic-gi-capture` selects an exact accumulator
for the learned pixel-gather guide. The texture capture harness accepts the
same optional flag. In engine tests/tools, set
`DeferredLightingRenderer::SetDeterministicGuideCapture(true)` before recording.
Changing the option invalidates each view's history with reason
`guide_accumulation`; assigning the current value does not reset it.

[`guide_sum.glsl`](../Data/Shaders/VoxelGI/guide_sum.glsl) adds nonnegative
binary32 samples into ten 32-bit integer words per guide bin, in units of
2^-149. Atomic carries preserve the sum independently of invocation arrival
order. After the workgroup barrier, it rounds once to nearest-even binary32;
the existing receiver averaging and guide update then continue. It needs
neither float64 nor 64-bit atomics. The bound is fewer than 2^32 samples per bin.
Negative nonzero/NaN inputs produce canonical NaN, positive infinity produces
infinity, and a sum outside binary32 range can still overflow on conversion.
This is not a clamp, fixed-point precision reduction, sample removal or disabled
learning path.

Specialization constant 0 chooses the accumulation path. False is the default.
The learned-bin array has 384 uints normally and 3,840 in capture mode: 1,536
versus 15,360 bytes. Freezing/folding both specialization values with the SDK
SPIR-V tools confirms those array sizes and validates both resulting modules.
Full/half, counter and brick trace registrations use the same shader files with
separate capture program names. The fallback pass does not learn a guide.
Screen-probe gather is unaffected. Cross-device bit identity is not claimed.

## Numerical and rendering checks

| Check | Result |
| --- | --- |
| Final cooked array/spatial/ray/GI and exact-sum native suite | 16/16, zero skips, 27.125 seconds |
| Existing FBX/GI, lighting-provider, screen-probe, shadow and relighting regressions | 12/12, zero skips, 47.438 seconds |
| Native presentation, compute warmup and registered shader creation matrix | 15/15, zero skips, 6.520 seconds |
| RenderCore regression suite | 717 executed pass; nine pre-existing disabled cases |
| Shader reflection suite | 23/23 |
| Focused desktop BC7 capture campaign | 36 clean processes, eight exact source/reference pairs, four exact GI repeats |

The GPU arithmetic test uses 276 independently generated cases, each with
4,096 samples and four input permutations, in both inline and threaded modes.
Python `Fraction` computes exact rational sums and selects nearest-even values
by rational binary search. The test compares all ten accumulator words and
the rounded float bits. Cases include zeros, subnormals, normal boundaries,
ties, sticky bits, carries across words, large HDR exponents, overflow,
infinity/NaN/invalid-negative inputs and deterministic random distributions.
This is arithmetic evidence; it is not a general rendered-HDR quality audit.

The permanent [spatial GI tests](../../ZenAssetProcessor/tests/integration/zenengine/Editor/SpatialGIRenderingTests.inl)
cook two original material/alpha fixtures, delete copied sources, and load
standalone and spatial packages without recooking. Each image uses a fresh
device, 16 frames at 256x192, default temporal/filter/adaptive pixel gather,
environment and directional lighting. Software and hardware triangle queries
run in both execution modes. All eight spatial/reference and eight repeated
standalone comparisons have zero changed channels. Provider assertions,
nonblank guards, guide-history reset checks and a visible PBR/GI control pass.

The desktop rerun uses the previously audited M5 packages/configurations and
the exact capture option at 640x480: raster alpha, normal, clearcoat and
shared-role cases, plus software/hardware GI for alpha and shared-role. Source,
raw, mip-only and BC7 packages run separately; GI additionally repeats the raw
package. All process, validation, input identity and exact reference gates pass.
The original configuration is restored byte-for-byte. Renderer and shader
identities remain unchanged throughout the run.

The contact sheet was inspected: geometry, alpha openings and material panels
remain visible. Lossy acceptance is still open. Clearcoat retains its local
normal-map highlight shift (maximum byte delta 113, RMSE 0.547504, 0.075195% of
pixels above delta 8). Alpha BC7 differs by at most three in raster and two in
either GI provider. Normal and shared-role frames are exact in these views.
No image or frame threshold was relaxed; the report's automated pass does not
approve all lossy frames.

## Cost and default behavior

The initial exact implementation was measured against the original shader in
candidate/baseline/baseline/candidate order. Each run used 32 warmup and 96
timed frames at 640x480, inline execution, and GPU timestamps around the
`HybridTrace` node. The numbers below average the two run medians. These are
small fixture trace-pass timings, including its prologue barriers, not total
frame time or production-scene performance.

| Provider / fixture | Original median, microseconds | Initial exact median, microseconds | Change |
| --- | ---: | ---: | ---: |
| Software / material | 92.352 | 125.096 | +35.46% |
| Software / alpha | 90.336 | 105.576 | +16.87% |
| Hardware / material | 103.216 | 148.272 | +43.65% |
| Hardware / alpha | 101.720 | 129.832 | +27.64% |

That cost is why exact accumulation is opt-in. A subsequent run with the final
capture option **disabled** measured 92.800, 89.808, 103.136 and 101.616
microseconds respectively, within 0.6% of the original medians. The final
specialized exact pipeline was verified for correctness, but was not separately
benchmarked; do not equate the initial exact timings with an exact measurement
of that final pipeline. Startup/module overhead and other devices remain
unmeasured. The temporary timing include was removed from the normal test target.

## Presentation failure diagnosis

The first 36-process desktop run failed synchronization validation, including
raster and an exact-option-disabled control. Its reports remain failed. An API
dump shows ZenEngine copying the swapchain image and signaling `RenderComplete`,
then extra render passes and a submission with no signal semaphore occurring
before present. Those extra passes are absent from the engine's dynamic-rendering
presentation path. Present still waits on the earlier engine semaphore. This
explains why the later layout transition is outside that dependency; see the
[Vulkan semaphore synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-semaphores-signaling).

The existing local RTSS isolation procedure uses a hash-identical temporary
`7zFM.exe` copy to select an existing no-hook profile. With the same process-local
implicit-layer exclusions and synchronization validation enabled, the extra
render passes disappear from the API dump and presentation is clean. The final
36-process campaign uses that isolated copy. No renderer synchronization change,
global overlay change, validation suppression or relaxed acceptance was needed.
The ordinary Google Test exit code alone did not detect the validation error;
the log audit did.

## Evidence and remaining work

`build/asset-integration-release/m6-guide-capture-results.json` summarizes the
tests, image comparisons and timing evidence. Its `m6-guide-capture-evidence/`
directory retains hash-verified source, baseline, executable, shader, fixture,
capture, log, API-dump and report snapshots. `m6-guide-capture.diff` isolates
this change from pre-existing GI work. The diagnostic timing fixture remains in
ignored build evidence, not the normal suite.

Desktop images and the unchanged automated gate report are in
`build/asset-processor/m5-guide-isolated-visual/`. Earlier rejected builds and
overlay-contaminated runs remain alongside the successful evidence. The
[earlier guide ablation](ZenAssetProcessorM6SpatialGiRepeatability.md) remains
historical diagnosis, rather than being relabeled as acceptance.

M5 still needs the remaining compressed-profile visual and performance gates.
M6 still needs its broader sampling/device coverage and measured workload
benefit; the spatial fixture previously increased GPU allocation despite fewer
images. Final packaging, release pinning and the complete supported build/runtime
matrix also remain open. This checkpoint does not mark the implementation plan
complete.
