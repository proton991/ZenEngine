# RHI production readiness: R9 verification

Status: Implemented; internal to RenderCore, 2026-10-02. Part of [section 8](RHIImprovementPlan.md#8-production-readiness-todos).

## Result

The user chose the internal-RHI option. The README records borrowed raw pointers, caller-owned GPU retention, native cache invalidation and bindless epoch obligations. GPU-safe retirement remains in RenderCore; late use after executor destruction is invalid.

## Verification and limits

Audited ZenUI/Source/UIRenderer.cpp: buffer/texture lifetime uses RenderDevice; the sampler belongs to its cache and graph imports retain frame use. SceneRendererDemoLightingCapture.cpp and SceneRendererDemoVoxelCapture.cpp flush the RHI thread and wait idle before readback, then destroy through RenderDevice. Native fixtures retain resources through completion/definite discard and release before backend shutdown. Existing retirement, extraction, pending-ticket and native bindless tests cover these contracts.

The [shared production verification record](RHIProductionVerification.md) contains the exact environment, full Debug/Release counts, smoke/capture results and paired frame measurements. Local build/test artifacts are under [build/rhi-production/](../build/rhi-production/). These results apply to the complete working tree, including the user's pre-existing R5 and deferred work; they do not attribute a measured cost to this item alone.
