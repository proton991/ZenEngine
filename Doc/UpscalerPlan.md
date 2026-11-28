# Upscaler Plan: DLSS and FSR

Goal: ship ZenEngine at 4K output with a temporal upscaler: DLSS Super Resolution on NVIDIA RTX GPUs, FSR on
every other Vulkan GPU, and the current native image when neither is available. Quality comes first. An
upscaled frame must not be noisier, blurrier or laggier than today's native frame in the GI three-case
check. Frame time is this phase's second target.

## Where the engine stands (2026-11-27)

- **Native 4K cost.** The three-case run on New Sponza (HW RT GI, RTX 5080, commit `d69f51af`) gives:

  | Case | FPS | ms |
  |---|---|---|
  | Still | 49.5 | 20.2 |
  | Pan | 33.4 | 30.0 |
  | Light ball | 29.8 | 33.5 |

  Most of that time should scale with rendered pixels (G-buffer, receiver visibility, 162,000 screen probes,
  gather, lighting). That is an expectation; U2 measures it.
- **No temporal anti-aliasing, no camera jitter, no render scale.** Every pass renders at the view's extent.
  [RenderCoreImprovementPlan.md](RenderCoreImprovementPlan.md) (Phase 2, step 4) holds render scale back until
  a pass upscales the lit result.
- **Motion vectors already exist on the hybrid path.**
  - The G-buffer writes `offscreen_motion` (RG32F, previous minus current NDC) from per-node previous model
    matrices (`BuildPreviousModels`), see `offscreen_surface.glsl:76`.
  - In pixels it is `motion * 0.5 * extent`, the same direction DLSS and FSR expect (current to previous).
  - Gaps:
    - Sky and other pixels without G-buffer geometry keep the clear value 0, which is wrong under camera
      rotation.
    - Forward translucent surfaces write no motion.
    - Scenes without the hybrid G-buffer have no motion target at all.
- **Two composition paths.**
  - The deferred path tone-maps (Reinhard, then sRGB) inside the lighting shader and writes the swapchain
    directly. It already has a linear-output switch (`gbuffer.worldOrigin.w`).
  - The forward path renders `forward_hdr`, tone-maps the opaque result to `forward_display_linear`
    (Reinhard, no sRGB), blends translucent layers in that display space, then encodes sRGB in
    `ForwardToneMap`.
  - The light ball and light markers draw after tone mapping into the view's color and depth targets.
- **Depth and exposure.** Depth is standard `[0, 1]` (`GLM_FORCE_DEPTH_ZERO_TO_ONE`, `eLess`), not reversed.
  There is no exposure: the Reinhard curve takes scene radiance directly.
- **ZenRHI interop.**
  - `GetVulkanDeviceHandles()` returns the instance, physical device and device.
  - Images, image views and command buffers have no public native accessors. The README already defines the
    contract for native recording: call `InvalidateCachedState()` afterwards.
  - Device extensions are a fixed list in `VulkanExtension.cpp`, with no host hook.
- **GI assumes a fixed pixel grid when the camera stops.**
  - `HybridGIRenderer.cpp:1143` (`unchangedReceivers`) compares projection-view matrices for exact equality.
  - The pixel history, receiver visibility (4×4 reuse), probe sources (8×8 reuse), placement and the glossy
    temporal pass reuse history 1:1 when it holds.
  - With jitter it never holds, so the still-camera result (RMS 0.025, 0.000% of pixels above 4 levels) is
    at risk.

## What shipped engines do

- **NVIDIA's own Vulkan integration**, read from the local RTXGI 2.9.0 checkout (`E:\Dev\RTXGI`, Donut
  `src/render/DLSS-VK.cpp`):
  - It calls NGX directly, without Streamline.
  - Required extensions are queried before instance and device creation (`DLSS::GetRequiredVulkanExtensions`,
    which drops `VK_EXT_buffer_device_address` because the core feature is used) and passed as *optional*
    extensions.
  - `NVSDK_NGX_VULKAN_Init` takes the three device handles; capability comes from
    `NVSDK_NGX_Parameter_SuperSampling_Available`.
  - One feature is created per input and output extent with `IsHDR | MVLowRes` (plus `DepthInverted` for
    reverse-Z).
  - Each frame it moves inputs to shader-read and the output to storage, then fills `NVSDK_NGX_Resource_VK`
    from the image, view, format and extent.
  - It passes the pixel jitter offset and a reset flag, calls `NVSDK_NGX_VULKAN_EvaluateFeature_C` on the
    native command buffer, and clears the RHI's cached state.
  - Ray Reconstruction is the same feature with `NVSDK_NGX_Feature_RayReconstruction`, plus diffuse albedo,
    specular albedo and packed normal-roughness guides.
  - Its SDK copy is DLSS 310.9 (`nvngx_dlss.dll`, `nvngx_dlssd.dll`). The headers mark
    `NVSDK_NGX_VULKAN_RequiredExtensions` deprecated in favour of
    `NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements` and `...DeviceExtensionRequirements`.
- **Donut's jitter.** `PlanarView` applies the pixel offset as an NDC translation after projection and keeps
  "no offset" matrices for motion vectors. The pathtracer's motion vectors use `matWorldToClipNoOffset` for
  both frames. Points behind the previous camera get an off-screen vector, not zero.
- **DLSS mode scales** (RTXGI sample `UI.h`):

  | Mode | Scale |
  |---|---|
  | DLAA | 1 |
  | Quality | 1/1.5 |
  | Balanced | 1/1.68 |
  | Performance | 1/2 |
  | Ultra Performance | 1/3 |
- **AMD FSR** ([upscaler manual](https://gpuopen.com/manuals/fsr_sdk/techniques/super-resolution-upscaler/),
  [FSR SDK](https://gpuopen.com/amd-fsr-sdk/)):
  - The same FSR API covers FSR 3.1.x and the ML upscaler (FSR 4.1). FSR SDK 2.4.0 (October 2026) added
    Vulkan at parity with DX12
    ([release notes](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases)).
  - The ML path runs on Radeon RX 6000 and newer and on listed Ryzen AI APUs. Other GPUs, NVIDIA included,
    get FSR 3.1.6.
  - Vulkan setup takes `VkDevice`, `VkPhysicalDevice` and `vkGetDeviceProcAddr`. Resources go through
    `ffxApiGetResourceVK`, inputs in `SHADER_READ_ONLY_OPTIMAL`, and the manual lists no extra extensions.
  - Ship `amd_fidelityfx_loader_vk.dll` and `amd_fidelityfx_upscaler_vk.dll`.
  - Recommendations:
    - Jitter: Halton(2,3) with `ceil(8 · ratio²)` phases.
    - Mip bias: `log2(render / display) − 1`.
    - Motion vectors: unjittered, in pixels via `motionVectorScale`.
    - Masks: a reactive mask (R8) for alpha-blended surfaces without motion, or the
      `GenerateReactiveMask` helper from opaque-only and final colour.
- **Lumen** relies on TSR to remove its per-frame probe-placement jitter. It places screen probes on the
  jittered G-buffer and accepts sub-pixel motion in its temporal filters rather than requiring exact reuse.

## Decisions

1. **Integrate DLSS through NGX directly, following Donut's `DLSS-VK.cpp`; don't adopt Streamline.**
   Streamline adds instance and device interposition that ZenRHI does not need. Frame generation and Reflex
   are out of scope.
2. **Use FSR through the FSR API**, letting it pick FSR 4 or 3.1 by capability. Don't write an in-house
   TAA: FSR 3.1 runs on every Vulkan 1.2 GPU this engine targets. "Off" (native, no AA) stays available and
   remains the default until the gates below pass.
3. **Select by capability, never by vendor or device ID:**
   1. DLSS when NGX reports Super Resolution available.
   2. Otherwise FSR when the FSR context creates.
   3. Otherwise Off.
4. **Upscale the display-linear image, not HDR.**
   - The forward path blends translucent layers in Reinhard display space, so a pre-tone-map input would
     change their look.
   - Input: Reinhard-compressed linear colour at render resolution with translucents composited. DLSS gets
     no `IsHDR`, FSR no `HIGH_DYNAMIC_RANGE`; exposure is 1.
   - After the upscaler, at output resolution: sRGB encode, light markers and UI.
   - The light ball moves into the scene pass at render resolution, with its own motion vectors.
5. **Motion vectors stay previous minus current, computed from unjittered matrices.** The scale to pixels is
   `0.5 · render extent`, and both SDKs take low-resolution motion vectors.
6. **Screen probes stay 8 render pixels apart.** At Quality a probe covers 12 output pixels. Denser probes are
   tried only if the gates miss.

## Stages

Each stage ends with a validation-layer launch and the short checks named in it. Full three-case runs happen
only at stage gates (about 10 minutes each). Close RTSS before every native run.

### U0. SDK intake

- Add `ZEN_WITH_DLSS` and `ZEN_WITH_FSR` CMake options, both off by default until U4 and U5 pass.
  - Pin the DLSS SDK at the commit RTXGI uses (`a8ed84e2`, SDK 310.9) and FSR SDK 2.4.0 as
    `zen_declare_dependency` archives with SHA-256.
  - Copy each runtime DLL next to `ZenApp.exe`.
  - This needs your approval: it downloads the archives. The DLSS files already exist under
    `E:\Dev\RTXGI\External\Donut\thirdparty\DLSS`; a `ZEN_DLSS_SDK_DIR` override can point there instead of
    downloading.
- Licences:
  - DLSS: the SDK's `LICENSE.txt` and the RTX UI guidelines in its `doc/` folder.
  - FSR: AMD's binary licence for the DLLs; the source files carry MIT-style terms.
  - Record what redistribution requires.
- **Exit:** the engine builds with each option on and off, and `ZenApp.exe` starts with the DLLs present
  and with them missing.

### U1. ZenRHI interop (ZenRHI submodule first)

- **Optional extensions.** `RHIDeviceDesc` gains a host callback that adds instance extensions before
  `vkCreateInstance` and device extensions after physical-device selection. Unsupported extensions are
  skipped and logged. NGX's feature-requirement queries feed it.
- **Native handles.** `VulkanInterop.h` gains `GetVulkanImage`, `GetVulkanImageView` (with format and
  subresource range) and `GetVulkanCommandBuffer`, all borrowed, under the existing lifetime contract.
- **RDG external pass.**
  - The pass declares sampled and storage textures, and RDG transitions them to `SHADER_READ_ONLY_OPTIMAL`
    and `GENERAL`.
  - Its callback receives the native command buffer and handles, and RDG calls `InvalidateCachedState()`
    afterwards.
  - It runs on the graphics queue and is never culled once it has outputs.
- **Exit:**
  - ZenRHI mock tests cover the extension callback and the pass's barriers.
  - A trivial native pass (`vkCmdClearColorImage` on an RDG texture) passes validation in ZenApp.
  - Push order follows AGENTS.md: ZenRHI, then the engine pointer.

### U2. Render extent separate from output extent

- **RenderView** gains a render extent alongside the output extent. Everything scene-side renders at the
  render extent: G-buffer, GI, shadows, lighting, forward and translucent passes. That includes
  `ScreenProbeGI`'s grid and the world-cache tile paths.
- **Composition restructure.**
  - The deferred path writes display-linear colour to a render-resolution RGBA16F target instead of the
    swapchain.
  - Both paths end in one upscale stage. Its first implementation is a bilinear copy, so render scale works
    before any SDK.
  - Then an output-resolution display pass (sRGB), light markers and UI.
- **Light ball** moves before the upscale stage (render-resolution colour, depth and motion).
- **Mip bias.** Material samplers take `log2(render / output) − 1`, recreated when the ratio changes.
  G-buffer, lighting and post samplers stay at 0. Confirm the DLSS guide's value in U4.
- **ZenApp:** add `--upscaler=off|dlss|fsr|auto` and `--upscale-mode=native|quality|balanced|performance|ultra`.
  Log render and output extents at startup and in capture and profile metadata, as AGENTS.md requires.
- **Exit:**
  - At scale 1.0 with upscaler off, captures are byte-identical to `d69f51af`.
  - At 1/1.5 with the bilinear stage, the three cases run without validation errors.
  - GI reports the reduced probe count and world-cache tile count.
  - VRAM and the world-cache size are logged at both scales.

### U3. Jitter and complete motion vectors

- **Jitter.** The camera keeps an unjittered projection and adds a pixel offset as an NDC translation, as in
  Donut. Vulkan's NDC y points down, unlike Donut's convention, so check the y sign with a still scene: the
  upscaled image must converge sharp.
  - Sequence: Halton(2,3) with `ceil(8 · (output / render)²)` phases (FSR's formula). DLAA and native AA use
    8. Confirm DLSS's recommendation from its programming guide in U4.
  - Off when the upscaler is off.
- **Motion vectors.**
  - Computed from unjittered current and previous clip positions.
  - Sky and empty pixels get camera-only motion from depth at the far plane.
  - Forward translucent pixels keep the opaque motion behind them and write coverage to an R8 reactive
    target, used as FSR's reactive mask and DLSS's bias-current-colour mask.
  - Points behind the previous camera get an off-screen vector, as in Donut.
  - Scenes without the hybrid G-buffer write motion from their depth pass.
- **GI under jitter.** GI reconstructs positions with the jittered matrices, the ones that match depth. It
  decides "camera unchanged" from the unjittered matrices.
  - When only the jitter changed, history passes reproject by the jitter difference instead of falling to
    the moving-camera path.
  - Probe placement keeps its world-space anchors (relocation already tolerates sub-pixel moves).
  - Inventory every user of `projViewMatrix` and `previousProjectionView`:
    - `ReceiverVisibility`
    - `ScreenProbeSources`
    - `ScreenProbePlace`
    - `ScreenProbeGather`
    - pixel history
    - `GlossyTemporal`
    - ReSTIR reuse
    - forward passes
- **Motion-vector check tool.** Warp the previous output by the motion vectors and compare it with the
  current frame on a static scene with a panning camera, and on the animated-node fixture. The residual
  outside disocclusions must stay at noise level.
- **Exit:**
  - The motion-vector check passes, including sky under rotation and an animated node.
  - With upscaler off, images are unchanged.
  - The GI still-camera case with jitter on, measured on the GI bounce output, stays within A/A noise of
    jitter off.

### U4. DLSS Super Resolution

- Follow `DLSS-VK.cpp`:
  - `NVSDK_NGX_VULKAN_Init_with_ProjectID` at device creation.
  - Capability from `SuperSampling_Available`.
  - One feature per view and per (render extent, output extent, mode). The editor re-creates it only once a
    viewport size has been stable for a few frames.
  - Evaluate in the U1 external pass with:
    - colour (display-linear), depth, motion with `InMVScale = 0.5 · render extent`, the bias-current-colour
      mask and jitter in pixels
    - reset on camera cuts, scene load, extent or mode change, and GI `InvalidateHistory`
    - render preset left at default
- **Memory ordering.** Create the feature before the world cache sizes itself from free VRAM, or reserve its
  measured size, so DLSS can never push GI into a smaller cache or out of memory.
- The startup warm-up's hidden frames also run DLSS, so frame 1 already has upscaler history.
- **Gates** (New Sponza, 4K output, interleaved A/B against Off at native):

  | Measure | DLAA | Quality |
  |---|---|---|
  | Still camera, RMS / pixels > 4 levels | ≤ 0.025 / ≤ 0.01% | same |
  | Pan, pixels > 10% off | ≤ 1.22% | ≤ 1.22% |
  | Light ball, pixels > 10% off | ≤ 0.89% | ≤ 0.89% |
  | First frame after warm-up, pixels > 10% off | ≤ 0.05% | ≤ 0.05% |
  | 180° snap, bright pixels per frame | ≤ native | ≤ native |
  | FPS, still / pan / ball (proposed targets) | report | ≥ 60 / ≥ 45 / ≥ 40 |

  - Pass your eye check of the light ball's reflections, the lantern glass and a fast turn in the editor
    (no ghost trails).
  - Nsight GPU Trace at 3840×2160 output with the render extent read from capture metadata, per AGENTS.md.

### U5. FSR

- FSR API context on the Vulkan backend.
  - Flags: standard depth, no HDR, exposure 1.
  - Inputs: the U3 reactive mask and jitter from the API's own phase and offset queries.
  - Version: FSR picks 4 or 3.1. Log which one.
- On this machine FSR runs as 3.1.6 on the RTX 5080. The ML path (RX 6000+, listed Ryzen AI APUs) cannot be
  verified here. It needs an AMD GPU run before FSR 4 can be called supported.
- **Gates:**
  - Same table as U4, with FSR 3.1 compared against Off.
  - Any miss is reported with numbers for you to accept or reject; FSR 3.1 is expected to shimmer more than
    DLSS.
  - Validation clean with both SDKs built in.

### U6. Selection, settings and documentation

- `--upscaler=auto` applies the capability order above.
- An `engine.cfg` content option sets the default mode, and the editor gets a mode selector.
- Captures record upscaler, mode, SDK and model version, render extent and reset count.
- [EngineProfiling.md](EngineProfiling.md) gains the upscaled profiling arguments.
- CPU tests:
  - Jitter sequence and phase count.
  - Mode-to-extent table, including odd output sizes.
  - Mip-bias formula.
  - Selection with mocked capabilities.
  - Resets on each trigger.
- **Exit:** the default turns to `auto` only after you accept the U4 and U5 results.

### U7. Optional: DLSS Ray Reconstruction

- RR is NVIDIA's mature answer to the noise this engine still shows: first-frame noise on newly revealed
  surfaces and reflection glints.
- RTXGI's sample pairs RR with a radiance cache (SHaRC).
- Evaluate it on NVIDIA only, with these inputs:
  - colour: lighting with GI
  - diffuse albedo: `offscreen_albedo`
  - specular albedo: split-sum environment BRDF from F0 and roughness
  - packed normal-roughness, depth and motion
- Compare two inputs: today's filtered GI, and GI with the screen-probe spatial and pixel filters reduced.
- **Keep it only if:**
  - the three-case pan and light-ball numbers and the reflection-glint count drop at least 30% against U4
    Quality
  - the still camera holds
  - frame time is at most U4's
- RR does not replace the GI denoiser on other GPUs.

## Risks

- **Jitter versus GI's exact reuse.** This is the largest risk. If the still camera loses its 0.000% result
  under jitter, the fallback is Lumen's approach: accept sub-pixel reprojection in GI and let the upscaler's
  history absorb the remaining shimmer. Measure before choosing.
- **World-cache convergence at lower render resolution.** Cache updates come from screen tiles, so at
  Quality about 44% as many tiles feed the cache each frame. The light-ball case shows whether its response
  slows. The fix is a per-frame update budget independent of resolution.
- **Display-space input.** Reinhard-compressed input loses some highlight detail in the upscaler's history.
  If highlights ghost or clip, feed HDR to the opaque path and keep translucent layers on the reactive mask.
- **Editor viewports** resize often. Re-creating a feature stalls (Donut waits for the GPU), hence the
  stable-size rule.
- **FSR 4 untested here.** Ship it as "FSR (version chosen by driver)", and say in the docs which versions
  were measured.

## Not in this phase

Frame generation, Reflex, dynamic resolution, HDR display output, and an in-house TAA.

## Sources

- Local: `E:\Dev\RTXGI` (v2.9.0):
  - `External/Donut/src/render/DLSS-VK.cpp`, `DLSS.h`, `src/engine/View.cpp`
  - `Samples/Pathtracer/Pathtracer.cpp`, `UI.h`, `Shaders/Pathtracer.hlsl`
  - `External/Donut/thirdparty/DLSS/include/nvsdk_ngx_vk.h`, `nvsdk_ngx_helpers_vk.h`, `nvsdk_ngx_defs.h`
- [AMD FSR upscaler manual](https://gpuopen.com/manuals/fsr_sdk/techniques/super-resolution-upscaler/)
- [AMD FSR SDK](https://gpuopen.com/amd-fsr-sdk/)
- [FSR API](https://gpuopen.com/manuals/fsr_sdk/getting-started/ffx-api/)
- [FidelityFX SDK releases](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases)
- [Khronos: FSR SDK 2.4 brings FSR 4 to Vulkan](https://www.khronos.org/news/permalink/amd-fsr-sdk-2.4-brings-fsr-4-upscaling-and-frame-generation-to-vulkan)
- [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS)
- [NVIDIA vk_denoise_dlssrr sample](https://github.com/nvpro-samples/vk_denoise_dlssrr)
