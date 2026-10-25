# BC7 search performance and native integration

The 2026-10-24 follow-up reduces endpoint-search work while preserving the
previous refinement's output. It also rebuilds the engine/editor integration
and verifies actual filtered-normal recooking through the editor. The full M5
quality/throughput gate and the overall implementation plan remain open.

A later [endpoint score-cache checkpoint](ZenAssetProcessorBC7ScoreCacheVerification.md)
adds bounded score reuse and records a further paired speedup with identical
tested payloads. The measurements below remain the earlier campaign's evidence.

## Changes

Profiling the 1024x1024 clearcoat normal level attributes 20.278 of 24.643
seconds to candidate generation, versus 0.397 to neighborhood construction,
0.450 to baseline measurement and 3.469 to candidate filter scoring. Only four
blocks hit the context cache; the remaining 65,532 require candidate generation.
This diagnostic isolates the private refinement; it is not a complete cook time.

Only `ZenAssetProcessor/src/Codecs/BlockEncoding.cpp` changes in the production
processor for this follow-up. The image-level guards, memory budget, sampler
handling, decoder and public quality settings remain unchanged. The search:

- Packs a block only after accepting an endpoint trial, using bounded one/two-byte
  field writes instead of writing each bit separately.
- Stores palette components contiguously and computes independent selector
  distances before the original ordered minimum reduction.
- Skips the redundant second p-bit toggle and unchanged endpoint trials.
- Reads initial endpoints once and reuses exact results when quantization maps
  different extension amounts onto the same endpoint seed.
- Stops a trial once its nonnegative partial error sum cannot beat the best
  complete score. A zero best score needs no further coordinate passes.

Accepted trials still measure every texel. The sixteen candidate positions,
strict comparison rules, selector tie-breaking and pixel summation order remain
the same. No approximation, threshold increase, new instruction-set requirement,
public setting, runtime format or decoder change is introduced.

## Evidence

The diagnostic variants reduce that large level from 24.643 seconds to 22.154
after packing changes, 18.189 after distance-layout/redundant-trial changes,
17.913 after seed reuse, and 15.968 after partial-score rejection. Every variant
produces exactly the same 1,048,576 encoded bytes across all 65,536 blocks. These
are single profiling runs; the paired production measurements below provide the
complete-cook comparison.

Three paired sequential Windows Release cooks use the same clearcoat source,
desktop BC7 quality 4, offline mips and two workers, alternating pair order.
No builds, tests or renderer runs overlap these measurements.
The host has an AMD Ryzen 9 9950X3D with 16 cores/32 logical processors; both
executables are x64 MSVC Release builds. Native graphics checks use an RTX 5080.

| Complete process wall time | Initial refinement | Optimized refinement |
| --- | ---: | ---: |
| Median of three runs | 34.750 s | 22.219 s |

This is a 36.1% reduction in median wall time (1.56x speedup). Every repeated
package has the same resource payloads as the previously verified package,
apart from build metadata. The final package also passes the independent image
audit. The roughly 1.8-second pre-refinement result belongs to the earlier
campaign; the remaining added cost has not been accepted as shipping throughput.

A separate deterministic corpus contains 4,096 source blocks: constant colors,
narrow ramps, positive-Z normals and arbitrary RGB, crossed with opaque,
constant and varying alpha. All 65,536 generated candidates from the final
optimized source match the prior implementation byte for byte and decode with
the independent block decoder. This checks candidates that the image-level gate
might reject, as well as those it would retain.

All 35 standalone Release CTests and four affected Debug suites pass. The
existing 84 edge/alpha combinations, undefined-direction regression, quality
preset checks, allocation failure and cancellation-at-every-poll tests still pass.

## Engine and editor

`CommonTest` and `EditorRenderingTest` were rebuilt against the optimized sibling
library. The engine and standalone build fingerprint headers match. The build
also compiled the current workspace's newer shaders and `ScreenProbeGather`
source; none of that unrelated source was edited for this task.

The editor's `recook_textured.gltf` fixture now includes an 8x8 filtered clearcoat
normal ramp, alongside its existing base-color and scaled-normal fallback uses.
The batch/editor parity regression requires that ramp to remain BC7Unorm with
all four mips. The installed CLI builds it with vertex-fetch optimization,
Zstandard and meshoptimizer transport. A read/write probe removes transport
encoding for the independent Pillow image audit, whose controls use the same
geometry optimization. All four normal mips pass the unchanged quality policy.

Fifty source/cache/loader/adapter tests and six native editor tests pass without
skips. The native tests cover inline/threaded execution, source recook rollback,
CLI/editor canonical scene equality, package loading and failed texture upload.
They render with the newly built shader payloads and report no Vulkan validation
errors or CPU/VMA leaks.

The harness initially treated all error log entries as failures. Inspection
confirmed exactly the eight errors injected by the rollback/upload tests: one
publication failure and three invalid-upload errors in each execution mode.
The finalizer verifies their exact messages and test association against the
completed successful test results; no passed tests were rerun to conceal them.
The earlier audit setup also needed transport decoding and matching geometry
settings. Those were harness corrections; production code was unchanged.

The clearcoat package comparison uses byte equality against the previous
verified package. No new comparison against the earlier GI screenshots is
claimed after the unrelated shader rebuild.

## Remaining work

The optimized search still adds offline work compared with the pre-refinement
encoder. Wider scene/device measurements, continuous/cross-mip filtering and
shipping-profile throughput acceptance remain open. This checkpoint does not
publish a hosted release or complete the migration.

## Artifacts

- [Paired production timings and image audit](../build/asset-processor/m5-bc7-optimized-timing/results.json)
- [Installed CLI and native integration evidence](../build/asset-processor/m5-bc7-optimized-editor/results.json)
- [Release suite](../build/bc7-performance-probe/ctest-release.log)
- [Affected Debug suites](../build/bc7-performance-probe/ctest-debug.log)
- [Frozen identities](../build/asset-integration-release/m5-bc7-optimized-evidence/identities.json)
- [Final checkpoint audit](../build/asset-integration-release/m5-bc7-optimized-evidence/final-audit.json)
