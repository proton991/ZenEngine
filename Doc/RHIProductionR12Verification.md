# RHI production readiness: R12 verification

Status: Implemented, 2026-10-02. Part of [section 8](RHIImprovementPlan.md#8-production-readiness-todos).

## Result

VK_EXT_memory_budget enables VMA budget tracking. Stats expose size, usage, budget and device-local flag per heap. Allocations request VMA's within-budget policy. At 90% of a nonzero device-local budget, RenderCore trims cached graph resources at a frame boundary while retaining active/exported objects and serial retirement.

Policy update, 2026-10-03: optional allocation failure returns null/status and permits a caller-selected fallback. Failure of required frame work is terminal in every execution mode under [R17 option B](RHIProductionR17Verification.md); dependent work is cancelled and accepted GPU ownership is retained through cleanup. The budget policy does not imply automatic recovery or eviction of active resources.

## Verification and limits

The native test checks extension availability and populated native heaps. Pressure threshold/unavailable telemetry coverage is combined with existing pool-trim tests that preserve in-flight/active/exported resources and R2's allocation-failure injection. Budget values are estimates; this run does not deliberately exhaust physical VRAM.

The [shared production verification record](RHIProductionVerification.md) contains the exact environment, full Debug/Release counts, smoke/capture results and paired frame measurements. Local build/test artifacts are under [build/rhi-production/](../build/rhi-production/). These results apply to the complete working tree, including the user's pre-existing R5 and deferred work; they do not attribute a measured cost to this item alone.

Budget values follow [VK_EXT_memory_budget](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_memory_budget.html): they are changing estimates, not a reservation or a guarantee of successful allocation.
