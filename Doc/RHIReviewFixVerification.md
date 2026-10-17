# RHI review fix verification

Execution date: 2026-10-17. Base revision: `7b49daa05e68f8572af560c30759abb060f392af`.
The supplied plan was the only untracked file at the start; source and index were clean.
All entries refer to the current uncommitted implementation. No commits were created,
and the supplied [plan](RHIReviewFixPlan.md) is preserved unchanged.

Implementation phases 1–8 are complete. Final acceptance results are recorded below.
This work does not close the broader hardware/vendor matrix in [RHIProductionTODO.md](RHIProductionTODO.md).

## Environment and baseline

- Windows x64; Visual Studio 2026 Professional 18.9; MSVC 14.51.36231; Ninja.
- CMake presets `x64-windows-msvc-debug` and `x64-windows-msvc-release`.
- Vulkan SDK 1.4.357.0; NVIDIA GeForce RTX 5080, driver 617.42.
  Native logs report API version 4211039 and driver version 2588573696.
  The AMD adapter mentioned in older production documentation was not available here.
- Correctness runs use `VK_LAYER_VALIDATE_SYNC=1`, `VK_LOADER_LAYERS_DISABLE=~implicit~`,
  and `DISABLE_RTSS_LAYER=1`. Executables use isolated `7zFM.exe` aliases to avoid
  RTSS interception, following the existing native-test convention.
- Both configurations build `VulkanRHITest`, `VulkanRHIIntegrationTest`, `RenderCoreTest`,
  `CommonTest`, `EditorRenderingTest`, and `scene_renderer_demo`.
  `NoEngineExceptions` runs through CTest.
- Local runner: `build/rvfix_verify.py`. Each run's `results.json` records commands,
  exit codes, test counts, durations, skipped cases and validation messages.
  Logs and copied executables remain under ignored `build/rvfix/`.

Baseline evidence: `build/rvfix/baseline/results.json`. There was one existing failure
in each configuration: `RHIWindowThreadingTest.SentResizesDeferAndCoalesceCallbacksUntilWindowUpdate`.
Windows clamped the requested 140-pixel client width to its 176-pixel minimum.
The fixture now uses 320/340-pixel widths and 200/210-pixel heights, preserving its
coalescing assertions. Production window policy is unchanged. Existing corrupt Debug
PDBs for the two Vulkan test executables were regenerated before baseline linking.

| Suite | Baseline Debug | Baseline Release | Final Debug | Final Release |
| --- | --- | --- | --- | --- |
| VulkanRHITest | 51 passed | 51 passed | 58 passed | 58 passed |
| VulkanRHIIntegrationTest | 343 passed, 1 failed | 344 passed, 1 failed | 373 passed | 374 passed |
| RenderCoreTest | 583 passed | 583 passed | 588 passed | 588 passed |
| CommonTest | 114 passed | 114 passed | 114 passed | 114 passed |
| EditorRenderingTest | 43 passed | 43 passed | 43 passed | 43 passed |
| NoEngineExceptions | passed | passed | passed | passed |

All final suite exit codes are zero, with zero VUID/synchronization-hazard messages.
CommonTest skips one directory-symlink case in both configurations because this Windows
session lacks symlink-creation privilege, as in the baseline. No Vulkan cases are skipped.
Release includes the additional `NonStrictShutdownLogsOutstandingResourcesAndContinues`
test. Eight RenderCore benchmark cases and two editor image-capture cases remain disabled
by default; they are opt-in workloads, not failed tests. The new queue benchmark was run
explicitly for O4 measurements.

The baseline's 48 smoke runs passed without VUID or synchronization-hazard messages.
The final matrix repeats modes 1/2/3 × RHI worker off/on × async compute off/on × UI
off/on in Debug and Release, with `--smoke-test --frames=44 --fixed-step --vsync=0`.
The smoke driver exercises resize and mode changes. Intermediate `final/` logs also
have 48/48 smoke passes, but contain superseded test expectations; acceptance uses
the subsequent `accepted/` run after those expectations and the metrics check were fixed.

## Item-by-item implementation and evidence

Commit status for every row: **uncommitted working tree**, based on the revision above.
Test names are searchable in the linked files. Complete suites also cover existing regressions.

| Item | Resolution | Evidence |
| --- | --- | --- |
| RV1 | Reject ordinary runtime arrays before native creation; remove ordinary variable-count/update-after-bind allocation. Global heap unchanged. | [Layout/corpus tests](../ZenSamples/CommonTest/ShaderReflectionTests.cpp), `VulkanReviewTest.LayoutRulesRejectRuntimeArraysReservedSetAndDuplicateNames`, `VulkanReviewIntegrationTest.InvalidShaderLayoutsReturnNull`. |
| RV2 | Explicit viewport stage/access barriers; terminal RDG graphics color-attachment state; matching state adds no barrier. | [Native presentation tests](../ZenSamples/CommonTest/VulkanPresentationReviewTests.cpp): clear-only readback and next-frame attachment load; [fake tests](../ZenSamples/RenderCoreTest/RHIThreadingTests.inl): `PresentationRestoresClearOnlyBackbufferOnGraphics`, `MatchingPresentationStateAddsNoBarrier`. |
| RV3 | Definite immediate submission rejection discards recording and permits retry; fatal/uncertain outcomes block with the actual cause. | [Review integration tests](../ZenSamples/CommonTest/VulkanReviewIntegrationTests.cpp): `ViewportRejectionAllowsRetryAndResizePreservesTheCause`, `ViewportDeviceLossBlocksWithNativeCause`, timeline/fence paths. |
| RV4 | Validate image shape, usage, format support, samples/mips and limits; validate texel usage/features/divisibility/element limit. | `TextureShapeRejectsInvalidExtentsMipsAndSamples`, `InvalidImageAndBufferDescriptionsNeverReachInvalidNativeUsage`; existing view/allocation/texel readback tests. |
| RV5 | Debug helpers delegate to guarded `SetObjectName`, skipping null resources. | `DebugNamesWithoutExtensionDoNotCallNativeFunction`, timeline/fence paths. |
| RV6 | Null-safe callback strings through `RHIErrorText`; no exception handling added. | [Pure tests](../ZenSamples/CommonTest/VulkanReviewTests.cpp): `NullDebugMessageFieldsAreSafe`. |
| RV7 | Worker exit/admission changes under one mutex; admitted late cleanup executes inline without a worker; ordinary jobs arriving during the drain are rejected, and after `Stop` returns all work runs inline as before `Start`. | `RHIThreadTest.StopWithoutFinalizerRunsLateCleanupExactlyOnce`, 1,000 iterations with concurrent cleanup/Invoke; existing finalizer rejection/death tests. |
| RV8 | Failed recording begin discards GPU timing instead of leaving it pending. | [Timing test](../ZenSamples/CommonTest/VulkanGPUTimingIntegrationTests.cpp): `FailedRecordingBeginDiscardsPendingTiming`. |
| RV9 | One descriptor-element resolver; allocate, resolve all, update native descriptors, then publish cache entry. | [Binding tests](../ZenSamples/CommonTest/VulkanBindingIntegrationTests.cpp): wrong/null sampler, misaligned UBO, BLAS-for-TLAS and valid retry; existing 2,049-dispatch/lifetime/readback suites. |
| RV10 | Enable/report optional cube arrays, mirror clamp and full uint32 indexing; report actual index limit; clamp anisotropy including nonfinite input. | `InvalidSamplersAreRejectedAndOptionalCapabilitiesMatchDevice`; [capability injection](../ZenSamples/CommonTest/VulkanCapabilityIntegrationTests.cpp) disables optional features; pure scalar limits test. |
| RV11 | Shared numeric UINT/SINT/float clear conversion; initialized union; reject depth/stencil `ClearTexture` before recording. | `ClearConversionIsNumericAndDefinedAtTheLimits`; [integer attachment/texture clear readback](../ZenSamples/CommonTest/VulkanAttachmentIntegrationTests.cpp) verifies value 7; invalid-description tests. |
| RV12 | Fix allocation sentinel; reject `eNone`/invalid policies; share VMA mapping, retaining host-access/budget flags; four fixture descriptions now request GPU allocation explicitly. | `BufferAllocationPolicyKeepsHostAccessAndBudgetExplicit`; invalid-buffer, progress and failure-injection suites. |
| RV13 | Delete cross-enum BitField conversions; retain integer interoperability; accumulate in int64; rename `HasAnyFlag`; remove unused `IsRenderTarget`. | Compile-time conversion assertions and scalar test; six intentional RDG usage-mask casts have explicit comments. |
| RV14 | Validate reserved set 0; only heap shaders bind/flush/capture it during native preparation; preserve layout compatibility. | Native binding observation for ordinary compute; layout/corpus tests; O2 measurements. |
| RV15 | Reject duplicate nonempty names across locations; document block-type/image-variable names; const single-lookup accessor. | Duplicate-block fixture, pure layout test and shipped corpus. |
| RV16 | Warm the three measured late compute keys through the existing cache. They are marked for warm-up where they are registered, behind the same capability gates, so only supported programs are warmed. The remaining case receives a separate asynchronous design. | Native `RendererInitializationWarmsRegisteredComputeKeysForLaterReuse`; O1 evidence and [follow-up](RHIPipelineCompilationFollowup.md). |
| RV17 | Remove zero-count default constructor; release-active underflow check on the atomic decrement result; document adoption; initialize pipeline pointers. Retain zero blend mask for unused slots. | `VulkanReviewTest.DoubleResourceReleaseAborts`; resource lifetime/default-state/retention tests; RenderCore/UI/editor pointer audit; both builds. |
| RV18 | Correct ownership, presentation, feature, descriptor, error and shutdown documentation. | [README](../ZenCore/Include/Graphics/RHI/README.md) reread against implementation. |
| X1 | `BlockSubmissions` requires a cause; dependency/finalization/resize/immediate-submit/device-loss paths preserve native code/operation/source. Native creation failures use their own error slot. | Viewport OOM/device-loss injection, `CreationFailuresStayOutOfSubmissionErrorsAndReachViewportCause`, and production submission-failure tests. Resize stays terminal under option B. |
| X2 | Shared null-safe formatting in terminal publication, cancellation, recording errors and callback. | Null-string pure test, error-path suites and source audit. |
| X3 | Terminal graphics node depends on prior backbuffer users, including compute. | [Queue test](../ZenSamples/RenderCoreTest/RDGQueuePreferenceTests.inl): `FinalPresentationStateDependsOnAsyncWriter`; native execution matrix. |
| N1 | Invalid stage/type indices return `Unknown`/0. | `ScalarContractsHandleBoundaryValues`. |
| N2 | Shared integer `bit_width(max(1, maxDim))` mip calculation. | Zero/power-of-two boundary tests. |
| N3 | Parameter bytes delegate to the view's existing 64-bit bounds calculation. | Scalar bounds test and shader-parameter tests. |
| N4 | Resource storage uses `alignas(RESOURCE_TYPES...)` and a size/alignment assertion. | Compile-time assertion; allocator/resource suites. |
| N5 | Track/enable optional validation-features extension before chaining debug-printf features. | Instance/capability suites; validation-enabled matrix. |
| N6 | Remove merged-workload variable shadowing. | Queue/submission suites. |
| N7 | Compare texture-view handle with `VK_NULL_HANDLE`. | View lifetime/failure tests. |
| N8 | Header utilities use `inline`. | Reflection suite and both builds. |
| O1 | Keep targeted initialization warmup of supported programs only; separate asynchronous follow-up design. | Hitch measurements below. |
| O2 | Keep as correctness requirement; aggregate timing neutral. | Timings and native bind observation below. |
| O3 | Keep as neutral under Phase 7 gate: copy/blit region inline capacity four. | Load and steady-state timings below. |
| O4 | Move accepted jobs; preserve rejected jobs for cancellation; add queue rvalue overload to remove hidden copy. | Throughput benchmark, shutdown/cancellation tests, frame timings below. |
| O5 | Skip only waits already covered by cached producer completion; no polling added. | Completed wait count 1→0 on timeline path; exact-serial/readback tests; GPU timings below. |
| O6 | Fence swap-and-pop; ordering contract unchanged. | Fence-mode retirement/submission suites. |

### Decisions and supporting audit

- Extraction only expresses sampled/storage/transfer-source read states, so it cannot
  enforce graphics color-attachment state. The terminal state-only access uses ordinary
  scheduling/dependencies. RDG metrics recognize its matching no-barrier case.
- Resize failure stays terminal (Phase 2 option B); suspend/retry is not introduced.
- Two legacy shipped vertex shaders had ordinary UBOs at set 0; their sources now use
  set 1. Sixty-four ignored stale SPIR-V files without current source counterparts were
  backed up under `build/rvfix/stale-spv/`. The regenerated 148-file shipped corpus passes
  reflection/layout validation. No tracked SPIR-V asset was deleted.
- Capability tests disable advertised cube-array/full-index support and mirror-clamp
  extension support. Native creation also exercises reported-off cube-array rejection
  on this capable adapter.
- The nine resource-pointer audit matches are declarations and intentional borrowed
  ownership, including UI textures, cached GI resources, extracted resources and editor
  fixtures. No factory-adoption leak was found. Unused blend slots rely on zero masks,
  so the optional RGBA default was not applied. Depth/stencil defaults are preserved
  and documented.
- Whole-tree formatting exposed pre-existing drift. Format-only changes to other active
  files are included to make `format_active.py --check` clean. The whitespace-insensitive
  added-line audit, including new C++ tests, finds no new `auto`, `std::vector`, `throw`,
  `try` or `catch`. Existing tokens on untouched/format-only lines are not new code.
  New logic follows final-return, explicit-type and project-container rules.
- Intermediate test expectations were corrected: completed exact producers may skip
  waits, while pending producers still wait for the original accepted serial. Warmup is
  tested through the real native renderer, not the fake fixture's stub initialization.

### Post-review corrections

A review of the implementation against the plan found seven issues. All were corrected
on 2026-10-17 in the same uncommitted working tree:

1. **RV7 inline use after stop.** The inline path had started rejecting ordinary jobs after
   a threaded `Stop` without a finalizer. That contradicted the documented contract that
   such a stop preserves standalone inline use. Ordinary jobs now run inline again once
   `Stop` returns; jobs arriving during the drain are still rejected.
2. **RV17 concurrent double release.** The underflow check read the counter before the
   decrement, so two racing releases could both pass. It now checks the value the atomic
   decrement returns.
3. **Creation errors.** Image, view, buffer and sampler creation failures were written into
   the submission-error slot. They now go to a separate creation-error slot, which the
   viewport reads when backbuffer creation fails.
4. **Duplicated fallback.** `PrepareSubmissionDependencies` now uses the shared
   `GetSubmissionFailureCause(fallback)`. The shared `RHIErrorOr` helper also replaces
   two identical ternaries.
5. **Shader name in layout errors.** Validation now runs after the shader name is set.
6. **Corpus test.** The test walks the compiled sources with non-throwing filesystem calls
   and needs no particular working directory. SPIR-V left over from deleted sources is
   ignored, and a source without its compiled shader fails. Cross-stage names are covered by a
   native test that creates every registered program.
7. **Capability-aware warmup.** Warmed programs are registered next to their capability gates,
   so only supported programs are warmed. Hardware ray-query programs were already registered
   only when ray queries are usable. A ray-tracing-disabled instantiation now verifies that
   no registered program needs an acceleration structure, and that every program creates its shader.

Re-running the suites also exposed two timing-dependent expectations left over from O5.
`TransferFeedsGraphicsThroughSubmissionDependency` required exactly one GPU wait and
`OwnedScheduleDispatchesBetweenProducerAndGraphicsReadback` exactly two, but O5 legitimately
skips a wait when its producer has already completed. Both failed intermittently in
asynchronous cases: the first once, the second in 2 of 40 repeated runs. All three upload
tests that check exact producer waits now share one check. Each emitted wait must carry an
expected producer's exact serial, in order, and every skipped producer must already be complete.

## Performance gates

Release, RTX 5080, Sponza, 1280×720, fixed step, vsync off, validation off, async compute
on. Each row averages two runs of 600 measured frames after 60 warmup frames. Entries
are **CPU frame / GPU frame / RHI execution**, in ms. Initial baseline profiles overlapped
compilation and are excluded; isolated runs reuse the preserved original executable.
Raw CSVs and summaries: `build/rvfix/{baseline-isolated,correctness,o3,o4-move,o5}/`.

| Candidate | Mode 2, inline | Mode 2, worker | Mode 3, inline | Mode 3, worker |
| --- | --- | --- | --- | --- |
| Original baseline | 0.7534 / 0.1747 / 0.1794 | 0.7809 / 0.1746 / 0.1867 | 4.6469 / 4.6385 / 0.2975 | 4.6523 / 4.6436 / 0.3765 |
| Correctness changes, including O2 | 0.7599 / 0.1733 / 0.1804 | 0.7696 / 0.1756 / 0.1813 | 4.6446 / 4.6350 / 0.2987 | 4.6399 / 4.6318 / 0.3851 |
| + O3 | 0.7594 / 0.1747 / 0.1810 | 0.7947 / 0.1758 / 0.1902 | 4.6512 / 4.6429 / 0.3013 | 4.6398 / 4.6306 / 0.3816 |
| + O4, actual queue move | 0.7604 / 0.1724 / 0.1827 | 0.7893 / 0.1779 / 0.1899 | 4.6525 / 4.6436 / 0.3048 | 4.6377 / 4.6292 / 0.3793 |
| + O5 | 0.7572 / 0.1761 / 0.1790 | 0.7932 / 0.1766 / 0.1880 | 4.6469 / 4.6387 / 0.2963 | 4.6467 / 4.6388 / 0.3830 |

The small differences do not establish a steady-state speedup. Phase 7 explicitly
permits neutral results; targeted counters/structural changes support retaining O3–O5.
O2 is also required for set-0 correctness regardless of timing.

- **O2:** Recording-thread bindless captures remain 1/frame in steady state, as intended.
  Native non-heap draws no longer bind set 0; the aggregate counter does not count
  individual native binds.
- **O3:** Mode-3 threaded scene-load mean: 221.506 ms before, 220.884 ms after, two runs
  each. The 0.28% difference is treated as neutral.
- **O4:** Five-run medians for 100,000 Dispatch / 10,000 Invoke jobs: correctness control
  31.874 / 85.199 ms; immediate pre-O4 control 30.794 / 82.976 ms; actual move
  30.762 / 82.316 ms. Immediate-control differences are treated as neutral. An earlier
  `o4/` candidate still copied via `Queue::Push(const T&)`; `o4-move/` supersedes it with
  `Push(T&&)`. The opt-in `RHIThreadTest.DISABLED_QueueThroughputBenchmark` has no timing
  assertions. Later O5 benchmark noise is not attributed to O4.
- **O5:** `CompletedCrossQueueDependencyWaitCount` reports known-completed producer waits
  falling 1→0 for timeline mode in both inline/threaded execution; fence mode stays 0.
  GPU frame time is neutral. Exact-producer tests require the accepted serial when a
  wait is emitted, or cached completion at least that serial when it is skipped.

### O1: pipeline hitch gate and warmup

In the original isolated mode-3 worker baseline, the largest CPU frame after frame 0
within the first 300 was frame 8: 165.283 and 164.065 ms. Temporary cache-key probes
identified late `HybridTemporalSP` creation around 162 ms and voxel-debug entry around
11 ms. All probe instrumentation was removed from final sources.

Renderer initialization warms `HybridTemporalSP`, `ResetDrawIndirectSP` and
`VoxelPreDrawSP` through the existing RenderDevice cache, still on the RHI thread.
`ShaderProgramManager::StoreWarmedComputeProgram` marks them where they are registered,
so a program behind a capability gate is warmed only on devices that register it.
None of the three needs hardware ray tracing. Warmup failures log and permit normal lookup to retry.
Approximately 171 ms of compilation moves into initialization.

Paired mode-3/worker/async probes (`o1-before-final/`, `o1-warm/`):

| Event | Before: pipeline creation / CPU frame | After: pipeline creation / CPU frame |
| --- | --- | --- |
| Temporal history settles, frame 20 | 161.636 / 176.896 ms | 0.236 / 15.426 ms |
| Voxel debug switch, frame 28 | 11.204 / 14.733 ms | 0.752 / 3.961 ms |
| Resize, frame 12 | 0.884 / 23.720 ms | 0.332 / 23.754 ms |
| Delayed GI entry, frame 30 in separate probe | 19.726 / 33.067 ms | 20.559 / 33.061 ms |

Delayed GI entry still creates 22 other pipelines, and resize cost is largely outside
pipeline creation. Warmup passes the targeted hitch gate without eliminating all spikes.
The required separate [asynchronous-compilation plan](RHIPipelineCompilationFollowup.md)
specifies pending/ready/failed entries, immutable worker inputs, RHI-thread publication,
fallback rendering, shutdown/error ownership, an explicit native-creation invariant
exception and `RHIResult<RHIPipeline*>`. Asynchronous compilation is not implemented here.

## Final acceptance

Accepted on 2026-10-17. `build/rvfix/accepted/results.json` records **64/64 successful
jobs**: ten Debug/Release test-suite runs, two exception-policy checks, all 48 smoke
combinations and four final Release profiles. No VUID or synchronization-hazard
messages occurred. Smoke logs also contain no nonzero missing-barrier, stage/access/
range/layout coverage or read-before-write diagnostics, and no error/critical log entries.

`python tools/format_active.py --check` passes for all 363 active C++ files;
`git diff --check` passes. Formatting and source-audit evidence is in
`build/rvfix-format-final.log`, `build/rvfix-final-audit.log`, and
`build/rvfix-acceptance-summary.log`. Documentation links resolve.
The CommonTest symlink skip and opt-in workloads are explained above. The measured
neutral optimizations, retained blend defaults, resize policy and separate asynchronous
follow-up are explicit decisions, not unfinished implementation phases.

The supplied plan remains unchanged. Source, tests and documentation are left uncommitted
for review. Broader vendor acceptance and implementing the asynchronous follow-up remain
outside this plan's scope.
