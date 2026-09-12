# UIRenderer RDG Implementation Plan

## Goal

Add Dear ImGui based engine UI as a RenderCore V2 renderer, following the same ownership and dispatch model as `SkyboxRenderer`, `Deferr  zxdΩΩΩΩzedLightingRenderer`, and the voxel renderers.

The UI pass should be appended by `RendererServer::DispatchRenderWorkloads()` and should participate in RenderGraph resource tracking. VulkanRHI should only execute normal RHI commands; it should not expose ImGui, workload, or command-buffer details to RenderCore.

## Non Goals

* Do not integrate `imgui_impl_vulkan` as the renderer backend.
* Do not draw UI directly inside `VulkanViewport::Present()`.
* Do not expose `VulkanWorkload`, `VkCommandBuffer`, or other low level Vulkan objects to the upper RHI/RenderCore layer.
* Do not add UI boundaries to RenderGraph just to split workloads. Workload finalization remains a VulkanRHI recording detail.

## Module Layout

Proposed files:

```text
ZenCore/Include/Graphics/RenderCore/V2/Renderer/UIRenderer.h
ZenCore/Source/Graphics/RenderCore/V2/UIRenderer.cpp
Data/Shaders/UI/imgui.vert
Data/Shaders/UI/imgui.frag
```

Recommended dependency split:

* `imgui` core is linked into `ZenCore`.
* A small platform input bridge feeds ImGui IO from either `GlfwWindowImpl` callbacks or the existing `KeyboardMouseInput` state.
* The Vulkan backend file `imgui_impl_vulkan.cpp` is not compiled.

Using `imgui_impl_glfw.cpp` for input is acceptable as a first step if it stays a platform bridge. It should not imply using the Vulkan renderer backend.

## Renderer Ownership

`RendererServer` owns `UIRenderer`, matching other renderer ownership:

```cpp
class RendererServer
{
private:
    UIRenderer* m_pUIRenderer{nullptr};
};
```

Lifecycle:

```cpp
void RendererServer::Init()
{
    ...
    m_pUIRenderer = ZEN_NEW() UIRenderer(m_pRenderDevice, m_pViewport);
    m_pUIRenderer->Init();
}

void RendererServer::Destroy()
{
    m_pUIRenderer->Destroy();
    ZEN_DELETE(m_pUIRenderer);
    ...
}

void RendererServer::ViewportResizeCallback()
{
    ...
    m_pUIRenderer->OnResize();
}
```

## Frame Flow

`RendererServer::DispatchRenderWorkloads()` should append UI nodes to the frame RDG after
the scene renderer nodes:

```cpp
void RendererServer::DispatchRenderWorkloads()
{
    m_pScene->Update();

    RenderGraph* pFrameRDG = m_pRenderDevice->GetCurrentFrameRDG();
    pFrameRDG->Begin();

    m_pSkyboxRenderer->PrepareRenderWorkload();
    m_pDeferredLightingRenderer->PrepareRenderWorkload();

    m_pUIRenderer->BeginFrame();
    m_pUIRenderer->BuildUI();
    m_pUIRenderer->PrepareRenderWorkload();

    pFrameRDG->End();
    m_pRenderDevice->ExecuteRenderGraph(m_pViewport);
}
```

For voxel mode, append UI nodes after the last scene/voxel renderer has appended its nodes:

```cpp
m_pSkyboxRenderer->PrepareRenderWorkload();
m_pShadowMapRenderer->PrepareRenderWorkload();
m_pVoxelizer->PrepareRenderWorkload();
m_pVoxelGIRenderer->PrepareRenderWorkload();
m_pUIRenderer->PrepareRenderWorkload();
```

## UIRenderer Responsibilities

`UIRenderer` owns:

* ImGui context lifetime, unless a broader editor system later owns it.
* Font atlas texture, sampler, and descriptor set.
* ImGui graphics pipeline.
* Per-frame vertex/index buffers for `ImDrawData`.
* One graphics pass appended to the current frame RDG that renders to the viewport color backbuffer.

Suggested public API:

```cpp
class UIRenderer
{
public:
    UIRenderer(RenderDevice* pRenderDevice, RHIViewport* pViewport);

    void Init();
    void Destroy();
    void BeginFrame();
    void BuildUI();
    void PrepareRenderWorkload();
    void OnResize();
};
```

`BuildUI()` can start with `ImGui::ShowDemoWindow()` and then move toward engine panels: render graph viewer, resource viewer, GPU workload stats, scene hierarchy, material inspector, and frame timing.

## RDG Pass Shape

The UI pass renders over the existing scene, so the viewport color backbuffer must use load/store:

```cpp
GraphicsPassBuilder builder(m_pRenderDevice);
m_gfxPass =
    builder
        .SetShaderProgramName("UIRenderSP")
        .AddViewportColorRT(m_pViewport,
                            RHIRenderTargetLoadOp::eLoad,
                            RHIRenderTargetStoreOp::eStore)
        .SetPipelineState(uiPso)
        .SetFramebufferInfo(m_pViewport)
        .SetTag("UIDraw")
        .Build();
```

No depth target is needed for the first implementation.

Pipeline state:

* alpha blending enabled
* depth test disabled
* cull mode disabled
* dynamic viewport enabled
* dynamic scissor enabled
* triangle list topology
* vertex layout compatible with `ImDrawVert`

Expected vertex inputs:

```text
location 0: pos   float2
location 1: uv    float2
location 2: color unorm8x4
```

Push constants can carry the orthographic scale/translation used by ImGui's vertex shader.

## Resource Transitions

Because UI is an RDG graphics pass, RenderGraph sees this access:

```text
previous scene RDG writes viewport color backbuffer
UI pass in frame RDG loads and writes viewport color backbuffer
present path copies viewport color backbuffer to swapchain image
```

This lets RDG/RHI emit normal transitions for the viewport color texture, instead of hiding the UI draw inside `VulkanViewport::Present()`. The UI pass should not manually transition Vulkan images.

## Recording ImGui Draw Data

`PrepareRenderWorkload()` should:

1. Call `ImGui::Render()`.
2. Read `ImGui::GetDrawData()`.
3. Return early if `TotalVtxCount == 0` or `TotalIdxCount == 0`.
4. Upload vertex and index data to per-frame buffers.
5. Append UI draw nodes to the current frame RDG.
6. For each `ImDrawCmd`, emit:
   * set scissor node from `ClipRect`
   * bind pipeline node if needed
   * bind vertex/index buffers
   * set push constants for projection
   * draw indexed node with `ElemCount`, `IdxOffset`, and `VtxOffset`

The current RDG supports scissor, viewport, push constants, vertex buffer binding, index buffer binding, and indexed draws. The first version should assume the font atlas descriptor set is fixed for the whole UI pass.

## Texture Support Phases

Phase 1:

* Support only the font atlas texture.
* Treat unexpected `ImDrawCmd::TextureId` values as unsupported and log once.

Phase 2:

* Add a small texture registration API:

```cpp
ImTextureID UIRenderer::RegisterTexture(RHITexture* pTexture, RHISampler* pSampler);
void UIRenderer::UnregisterTexture(ImTextureID id);
```

* Extend RDG or RHI command recording so descriptor sets can change per draw command.

This keeps the initial implementation small and avoids changing RenderGraph command semantics too early.

## Buffer Upload Strategy

Use one buffer pair per frame in flight:

```text
FrameResource
    RHIBuffer* pVertexBuffer
    RHIBuffer* pIndexBuffer
    uint32_t vertexCapacityBytes
    uint32_t indexCapacityBytes
```

Resize geometrically when ImGui data outgrows capacity. Keep buffers mapped if the current buffer usage supports persistent CPU writes; otherwise upload through the existing RenderDevice staging path.

Index format depends on `ImDrawIdx`:

```text
sizeof(ImDrawIdx) == 2 -> uint16 index format
sizeof(ImDrawIdx) == 4 -> uint32 index format
```

## Input Handling

The UI system must feed:

* display size
* delta time
* mouse position/buttons
* wheel
* keyboard keys
* text input

When ImGui wants input, scene/camera controls should ignore it:

```cpp
ImGuiIO& io = ImGui::GetIO();
if (io.WantCaptureMouse)
{
    // skip camera mouse handling
}
if (io.WantCaptureKeyboard)
{
    // skip editor/game keyboard handling
}
```

Input is best handled by a platform bridge near `GlfwWindowImpl` or `InputController`, not inside VulkanRHI.

## Resize Handling

On viewport resize:

* update `io.DisplaySize`
* rebuild or refresh the UI graphics pass if its rendering layout references the old backbuffer
* keep ImGui context and font texture alive

The font atlas does not need to be recreated on swapchain resize.

## Implementation Steps

1. Add Dear ImGui core as an external target.
2. Add `UIRenderer` files and CMake entries.
3. Create the ImGui context and input bridge.
4. Build the font atlas as an `RHITexture` and descriptor set.
5. Add UI shaders and pipeline state.
6. Add per-frame vertex/index upload buffers.
7. Build a one-pass UI graphics pass targeting viewport color with `eLoad/eStore`.
8. Append UI nodes last in `RendererServer::DispatchRenderWorkloads()`.
9. Gate scene input with `io.WantCaptureMouse` and `io.WantCaptureKeyboard`.
10. Add basic debug panels after `ImGui::ShowDemoWindow()` proves the path.

## Open Decisions

* Whether ImGui context lifetime should belong to `UIRenderer` or a later editor subsystem.
* Whether to use `imgui_impl_glfw` for the first input bridge or feed ImGui from `KeyboardMouseInput`.
* Whether RenderGraph should get a generic per-draw descriptor bind node before supporting arbitrary `ImTextureID`.
* Whether UI should always be enabled in engine builds or guarded by a compile option such as `ZEN_ENABLE_IMGUI`.
