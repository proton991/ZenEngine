# RHI production readiness: R1 verification

Status: Implemented, 2026-10-02. Part of [section 8](RHIImprovementPlan.md#8-production-readiness-todos).

## Result

Graphics and compute construction publish only complete native pipelines. A failed or partially populated native output is destroyed, invalid layouts return null, and RenderCore records failure without inserting a cache entry.

## Verification and limits

ScopedVulkanCall injects both pipeline creation failures, including a non-null partial output; retry succeeds. Existing RenderCore cache/retry coverage remains active.

The [shared production verification record](RHIProductionVerification.md) contains the exact environment, full Debug/Release counts, smoke/capture results and paired frame measurements. Local build/test artifacts are under [build/rhi-production/](../build/rhi-production/). These results apply to the complete working tree, including the user's pre-existing R5 and deferred work; they do not attribute a measured cost to this item alone.
