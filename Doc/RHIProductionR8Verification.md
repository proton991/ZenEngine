# RHI production readiness: R8 verification

Status: Workflow implemented; hardware acceptance outstanding, 2026-10-02. Part of [section 8](RHIImprovementPlan.md#8-production-readiness-todos).

## Result

tools/verify_rhi_production.py runs the RHI/RenderCore suites and optional 12-case smoke matrix with synchronization validation and JSON/log artifacts. .github/workflows/rhi-software.yml configures Windows software-driver CI with a pinned Vulkan SDK and SwiftShader revision. The README explicitly declares macOS/MoltenVK and other unvalidated platforms unsupported.

## Verification and limits

AMD hardware is available and tested locally. The user confirmed that no NVIDIA or Intel machines/access are available. CI has been authored but has not executed on GitHub; no remote job or pass is claimed. Run both configurations on NVIDIA and Intel before closing R8.

The [shared production verification record](RHIProductionVerification.md) contains the exact environment, full Debug/Release counts, smoke/capture results and paired frame measurements. Local build/test artifacts are under [build/rhi-production/](../build/rhi-production/). These results apply to the complete working tree, including the user's pre-existing R5 and deferred work; they do not attribute a measured cost to this item alone.

The workflow uses the [LunarG unattended SDK installation options](https://vulkan.lunarg.com/doc/view/1.4.304.1/windows/getting_started.html) and the official [SwiftShader CMake build](https://github.com/google/swiftshader/blob/master/README.md). Its runner and YAML were validated locally; SDK installation and the software-driver build still need an actual CI run.
