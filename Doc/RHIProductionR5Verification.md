# RHI production readiness: R5 verification

Status: implemented and verified, 2026-10-02. This is the first item in section 8's recommended order in [RHIImprovementPlan.md](RHIImprovementPlan.md). This document records the original R5 delivery. Subsequent items are tracked in [RHIProductionVerification.md](RHIProductionVerification.md).

## Changes and failure policy

`VERIFY_EXPR`, `VERIFY_EXPR_MSG` and `VERIFY_EXPR_MSG_F` now report the expression, source location and optional diagnostic to standard error, flush it, and abort in Debug and Release. Formatting uses a 1,024-byte stack buffer; formatting failure still reaches the fatal path. Successful checks evaluate the expression once and do not evaluate diagnostic arguments. The fatal path does not depend on an enabled logger. `VKCHECK` aborts on negative Vulkan results in both configurations; success and positive status codes keep their existing nonfatal behavior. `ASSERT` remains the standard debug-only assertion.

Existing failure coverage exposed three places that needed adjustment:

- Failed shader loading or limit validation directly destroyed an object with its initial reference still held. The factory now releases that reference through the existing RHI-thread destruction protocol before returning null.
- Descriptor binding/set validation already returns a checked boolean for recoverable rejection. These checks now log and return false instead of asserting.
- Staging tests deliberately double-released an allocation, released an allocation through another manager, or enqueued an out-of-bounds upload. These now require termination through death tests. The existing multi-chunk cancellation test uses valid input, forces rejection at the frame completion gate, and verifies cleanup after the earlier serials complete.

The contract is recorded in the [RHI README](../ZenCore/Include/Graphics/RHI/README.md). Checked texture/view, submission and presentation rejection behavior is preserved. This item provides an explicit fatal fallback; R1–R4 and R6 still own recoverable native failure propagation and controlled shutdown. It does not certify the remaining production-readiness work.

## Environment and commands

Windows, Radeon RX 7900 XT, MSVC 19.44, clang-format 19.1.5. Vulkan driver version reported by the profiles: raw `8388981`. Base revision: `e79660ecdef230a90d72ec4c4606ece5da04b15d`, with the user's existing uncommitted changes preserved. No files were staged or committed.

Artifacts, exact subprocess arguments, executable hashes and the runner are in [build/rhi-production-r5/](../build/rhi-production-r5/). GPU programs run sequentially with implicit layers disabled. Correctness runs use validation and synchronization validation; timing runs disable validation. The original configuration remains byte-identical, SHA-256 `c09a51bce3cc624cd1b66d3d90fb26072089bf22aa8c6c4ad0ac5aa483bf7871`.

```powershell
python build/rhi-production-r5/verify.py build
python build/rhi-production-r5/verify.py correctness
python build/rhi-production-r5/verify.py performance
python build/rhi-production-r5/summarize.py
```

The build runner uses the repository's existing MSVC environment wrapper and builds the default targets in both `x64-windows-msvc-debug` and `x64-windows-msvc-release`. Whole-file clang-format verification passes for all seven changed C++ files; `git diff --check` passes. The only build warning is the existing third-party GLI `GLM_ENABLE_EXPERIMENTAL` redefinition.

## Correctness results

Both default builds pass. Results are identical across Debug and Release:

| Suite | Passed | Skipped |
| --- | ---: | ---: |
| CommonTest | 105 | 1 |
| RenderCoreTest | 528 | 0 |
| VulkanRHITest | 45 | 0 |
| VulkanRHIIntegrationTest | 315 | 6 |
| SmartPtrTest | 34 | 0 |
| FlatHashMapTest | 6 | 0 |
| LRUCacheTest | 12 | 0 |
| InputControllerTest | 7 | 0 |
| ConfigLoaderTest | 21 | 0 |
| UIDrawPacketTest | 8 | 0 |
| RuntimeUIIntegrationTest | 10 | 0 |
| SceneModelSwitchTest | 8 | 0 |
| ConeVoxelGIIntegrationTest | 4 | 0 |
| ThreadPoolTest sample | exit 0 | — |

Each configuration passes 1,103 tests. The CommonTest skip needs directory-symlink privileges. The six native skips remain capability-dependent: four presentation-fence cases, the EXT surface-maintenance dependency and D24S8. Ten new tests cover fatal verification, formatted diagnostics, disabled logging, single evaluation, unevaluated diagnostic arguments and Vulkan error/status handling. Existing staging tests add three real-use death checks. The existing missing-shader and over-limit-shader cases pass with clean reference counts and teardown.

All 24 smoke runs pass: Debug/Release × modes 1/2/3 × RHI thread 0/1 × async compute 0/1, 44 frames each with resize, minimize/restore, light changes and shutdown. Suite and smoke logs have no unexpected verification failures, Vulkan validation errors or synchronization-validation messages. Negative suites retain their intentional engine error logs. Teardown reports no CPU or VMA leaks. Exact results are in [correctness-runs.json](../build/rhi-production-r5/correctness-runs.json).

All three 8-frame captures are byte-identical to the prior verified captures. Hashes are in [captures.json](../build/rhi-production-r5/captures.json). No shader or rendering-algorithm change required rerunning the separate glTF and voxel-GI image sweeps.

## Timing comparison

The pre-change Release executable was preserved before rebuilding. Sixteen same-session runs compare it with the final executable in A-B-B-A order for each mode/thread cell: Sponza, voxel resolution 256, 1280 × 720, 60 warm-up and 600 measured frames, fixed step, VSync off and async compute off. No builds or correctness runs overlap these profiles. Submission behavior is unchanged; both async-compute settings are covered by the correctness matrix.

Configuration, scene-document and shader fingerprints, final settings, compiler and device metadata match within every comparison. All 16 profiles pass `validate_engine_profile.validate(..., require_gpu=True, require_frame_gpu=True)`. Every measured frame still has one native submission. Full frame distributions and GPU-pass statistics are in [performance-comparisons.json](../build/rhi-production-r5/performance-comparisons.json), with each run preserved in [performance-summary.json](../build/rhi-production-r5/performance-summary.json).

Median / p95 in milliseconds, summarized as the median of the two per-run statistics; entries show baseline → final:

| Mode / RHI thread | CPU frame | GPU frame | RHI execution |
| --- | --- | --- | --- |
| PBR / inline | 1.1957 / 1.4724 → 1.1976 / 1.4876 | 1.1341 / 1.1961 → 1.1353 / 1.2014 | 0.6285 / 0.9010 → 0.6055 / 0.8900 |
| PBR / threaded | 1.1933 / 1.4901 → 1.1918 / 1.5286 | 1.1355 / 1.2221 → 1.1311 / 1.2144 | 0.5545 / 0.8505 → 0.5585 / 0.8805 |
| GI / inline | 1.5111 / 1.8628 → 1.5121 / 1.8513 | 1.4434 / 1.5326 → 1.4440 / 1.5350 | 0.9130 / 1.2530 → 0.9013 / 1.2350 |
| GI / threaded | 1.5103 / 1.8274 → 1.5166 / 1.8924 | 1.4432 / 1.5070 → 1.4436 / 1.5476 | 0.7700 / 1.0965 → 0.7848 / 1.1570 |

CPU medians change by −0.13% to +0.42%; GPU medians by −0.39% to +0.11%. This supports neutral median frame cost on this machine. Tail statistics vary more: the GI/threaded CPU p95 changes by +3.6%, with final runs at 1.9405 and 1.8443 ms versus baseline runs at 1.8432 and 1.8115 ms. These two repeats do not establish a tail-cost improvement or a portable performance guarantee. The change is retained for correctness; no speedup is claimed.
