# RHI production readiness: R15 verification

Status: Implemented, 2026-10-02. Part of [section 8](RHIImprovementPlan.md#8-production-readiness-todos).

## Result

Rendering keeps an UNORM backbuffer when the surface only offers its RGBA8/BGRA8 sRGB counterpart. Equal-size presentation copies compatible texel bits; format-changing blits are rejected. GetSwapchainFormat reports the logical rendering format.

## Verification and limits

The selection test forces each sRGB-only surface list and checks native sRGB image selection plus logical UNORM rendering format. Normal-mode captures remain compared against the saved baseline. A real sRGB-only monitor/surface was not available; selection is injected.

The [shared production verification record](RHIProductionVerification.md) contains the exact environment, full Debug/Release counts, smoke/capture results and paired frame measurements. Local build/test artifacts are under [build/rhi-production/](../build/rhi-production/). These results apply to the complete working tree, including the user's pre-existing R5 and deferred work; they do not attribute a measured cost to this item alone.

The presentation path uses the compatible-format rules of [vkCmdCopyImage](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyImage.html) to preserve texel bits.
