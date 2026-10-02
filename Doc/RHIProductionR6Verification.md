# RHI production readiness: R6 verification

Status: Implemented, 2026-10-02. Part of [section 8](RHIImprovementPlan.md#8-production-readiness-todos).

## Result

RHIThread contains unexpected asynchronous exceptions, marks the worker failed and cancels normal work. Cancellation completes batch tickets and timing results. Cleanup drains before the backend finalizer; mode/stop/failure flags are atomic. Queue tasks are moved out without copying their callables. Queue/abandoned-work destruction precedes descriptor/uniform managers and VMA. Backend teardown rejects live external resources or outstanding command-context pools as an ownership violation.

## Verification and limits

Inline/threaded tests cover task exceptions, cancellation, cleanup, finalizer order, cleanup admitted while Stop drains, and frame timing after worker failure. Full native tests include uncertain workload teardown and subprocess checks that outstanding resources/contexts abort before their backend ownership is destroyed. Texel-view and viewport tests verify bool/null failure contracts. Synchronous Invoke still forwards exceptions to its caller; cleanup exceptions deliberately abort.

The [shared production verification record](RHIProductionVerification.md) contains the exact environment, full Debug/Release counts, smoke/capture results and paired frame measurements. Local build/test artifacts are under [build/rhi-production/](../build/rhi-production/). These results apply to the complete working tree, including the user's pre-existing R5 and deferred work; they do not attribute a measured cost to this item alone.
