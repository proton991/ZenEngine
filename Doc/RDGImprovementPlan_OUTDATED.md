# RenderGraph Improvement Plan

## Current State

ZenEngine's RenderCore V2 RenderGraph already has useful foundations:

* explicit resource access records
* dependency sorting from resource reads/writes
* first-use barriers against persistent external resource state
* dependency barriers generated during executor compilation
* a compile/execute split
* basic transfer-queue eligibility for upload/copy graphs

The main remaining gap compared with mature render graphs in engines/renderers is that the current system is still centered around physical RHI resources. This limits transient resource lifetime optimization, pass culling, async queue scheduling, and high-quality validation.

## High-Level Direction

Keep renderers such as `SkyboxRenderer`, `DeferredLightingRenderer`, voxel renderers, and future `UIRenderer` responsible for declaring their own render work, but let a frame-level RDG executor compile and schedule all graphs together.

The intended shape is:

```text
RenderDevice
    owns one frame RDG

RendererServer::DispatchRenderWorkloads()
    get current frame RDG from RenderDevice
    begin frame RDG
    renderer A appends nodes
    renderer B appends nodes
    renderer C appends nodes
    UIRenderer appends nodes
    end one frame RDG

RenderDevice / RDGExecutor
    compile the frame RDG
    sort nodes from declared dependencies
    generate barriers
    execute using selected queue/command list
```

## Priority 1: Frame-Level RDG Execution

The frame path should use one RDG per frame. Renderers do not own separate frame RDGs; they append their nodes to the current frame RDG, and RDG dependency sorting/barrier generation decides the final safe node order.

Status:

* Done: `RenderDevice` owns one `frame_rdg`.
* Done: `RendererServer::DispatchRenderWorkloads()` gets the current frame RDG from `RenderDevice`, begins it, lets active renderers append nodes, ends it, then submits that single graph.
* Done: renderers fetch the current frame RDG directly through `RenderDevice` when appending nodes; they do not hold local RDG pointers.
* Done: renderers no longer expose or submit their own frame RDGs.
* Done: viewport frame execution uses `RenderDevice::ExecuteRenderGraph(RHIViewport*)`; the frame RDG is owned and selected by `RenderDevice`.
* Done: removed the redundant multi-RDG `RDGExecutor::ExecuteFrame()` path.
* Done: environment cubemap/BRDF preprocessing now appends nodes into the frame RDG instead of building separate renderer-owned RDGs.
* Done: removed the unused `RenderDevice::ExecuteRenderGraphs()` multi-RDG execution entry point.
* Done: `RDGExecutor::Execute()` compiles and executes the single RDG.
* Done: frame RDG execution summary remains one-shot for the current graph lifetime, even though the frame RDG is rebuilt every frame.
* Done: split RDG recording finalization from compilation. `RenderGraph::End()` now leaves the graph in a recorded state.
* Done: moved barrier generation into `RDGExecutor::CompileGraph()`.
* Done: removed the per-RDG init/intra barrier attachment path from `RenderGraph` V2.
* Done: standalone single-RDG execution and transfer-queue eligibility go through the same compiler path.
* Deferred: stronger frame-graph validation and targeted dependency/barrier tests or assertions are moved to Priority 9.

Completion assessment:

* Latest status, 2026-05-14: P1 runtime flow is functionally implemented. Recent cleanup removed redundant RDG parameters from frame execution, moved environment preprocessing into the frame RDG, removed multi-RDG execution entry points, and made `RDGExecutor` use pointer-style RDG APIs consistently.
* P1 implementation is complete for the simplified runtime flow: one frame RDG is recorded, compiled through `RDGExecutor`, barrier generation is owned by the executor, and execution uses one graphics command list.
* P1 runtime work is closed. The remaining validation work should be handled in Priority 9 after the logical resource model is clearer.

Barrier ownership target:

* `RenderGraph` declares resource accesses and owns recording data, but does not attach init or dependency barriers during recording finalization.
* `RDGExecutor::CompileGraph()` generates frame import barriers from external/persistent resource state and dependency barriers between consecutive accesses in the compiled frame graph order.
* Standalone single-RDG execution uses the same compiler path, avoiding two independent barrier-generation models.

Implemented frame execution path:

```cpp
void RendererServer::DispatchRenderWorkloads();
void RenderDevice::ExecuteRenderGraph(RHIViewport* pViewport);
void RDGExecutor::Execute(RenderGraph* pGraph, RHICommandList* pCmdList);
```

The current version stores one frame RDG on `RenderDevice`, lets each active renderer fetch that graph and append nodes into it, compiles the graph through `RDGExecutor`, generates barriers over physical resources, and records into one graphics command list. Later versions can add pass-level transfer/compute scheduling once validation is stronger.

Benefits:

* fewer redundant barriers between renderer-owned passes/nodes
* easier command-list/workload boundary control
* one place to reason about present/backbuffer state
* prepares the engine for UI nodes appended into the frame RDG
* prepares for async compute/transfer scheduling

## Priority 2: Logical RDG Resources

Status:

* Active. Logical handles and deferred graph allocation exist, but renderer-owned rendered resources are still mostly imported physical resources. They should migrate to RDG-created persistent resources through handle-native renderer setup, not by changing public `RenderDevice::CreateTexture*` or `RenderDevice::Create*Buffer` behavior.
* Done: added typed `RDGTextureHandle` / `RDGBufferHandle`, logical resource descriptors, imported/transient/persistent lifetime metadata, and initial import/create/extract APIs on `RenderGraph`.
* Done: added named `FindTexture` / `FindBuffer` lookups, handle-based pass resource declarations, producer/consumer dependency edges for handle-declared or transient/persistent logical resources, and validation logs for read-before-produce, persistent-without-extract, and used-before-allocation resources.
* Done: added an RDG resource allocator bridge. RDG owns the lifetime decision and asks the backend allocator for a physical `RHITexture` / `RHIBuffer` only while resolving graph resources during compile.
* Done: separated lifetime ownership. Transient graph resources are graph-owned and released on graph reset; persistent graph resources are graph-created, extracted, and owned outside the graph after execution.
* Done: restored normal `RenderDevice` texture/buffer creation. Upper RenderDevice resource helpers create physical RHI resources directly and are not routed through RDG.
* Done: imported active renderer-owned internal resources into the frame RDG bridge path, including deferred render targets, shadow maps, voxel textures/buffers, voxel GI resources, and skybox preprocessing outputs.
* Left: migrate renderer render targets and graph-owned buffers to `RenderGraph::CreateTexture` / `RenderGraph::CreateBuffer`, extract the persistent outputs after compile/execute, and keep imported physical resources only for true external inputs such as swapchain, scene assets, and compatibility paths.
* Left for later phases: full handle-native pass builders and descriptor bindings, transient resource pooling/aliasing, and versioned logical resources for order-independent multi-write scratch resources.

Current `RDGResource` wraps a physical `RHIResource*`. Mature RDGs distinguish logical graph resources from physical allocations.

This is also where true renderer setup-order independence starts. P1 builds one frame graph, but dependency direction is still inferred from the order resource accesses are recorded for purely physical resources. With logical RDG resource handles, a renderer can declare that a pass produces or consumes a named graph resource, and the compiler can connect producer/consumer dependencies even when renderer append order changes.

Multi-write scratch resources still fall back to access-order dependency construction until the graph has versioned logical resources. This avoids incorrectly treating repeated write/read pairs, such as offscreen scratch rendering followed by copy, as one unordered producer/consumer group.

Add resource categories:

```text
Imported  - external resource used by graph, such as viewport backbuffer or scene-owned texture
Transient - created and owned by RDG for temporary work, released on graph reset
Persistent - created by RDG, extracted, and kept alive by renderer/device after graph reset
```

Example API direction:

```cpp
RDGTextureHandle ImportTexture(RHITexture* pTexture, const char* name);
RDGTextureHandle CreateTexture(const RDGTextureDesc& desc, const char* name);
RHITexture* ExtractTexture(RDGTextureHandle handle);
```

Deferred creation target:

* `RenderGraph::CreateTexture` / `CreateBuffer` records only a logical graph resource and descriptor.
* `RDGExecutor::CompileGraph()` calls `RenderGraph::ResolveGraphResources()`.
* `ResolveGraphResources()` decides whether an unresolved graph resource is transient or persistent, builds the final physical descriptor from declared accesses, and asks the backend allocator to create the `RHITexture` / `RHIBuffer`.
* Transient allocations are tracked by the graph and released on graph reset.
* Persistent allocations are graph-created but extracted by the owner and must not be released with transient graph resources.

Benefits:

* transient texture/buffer pooling
* memory aliasing between non-overlapping resources
* clear ownership/lifetime model
* producer/consumer dependencies that are not tied to renderer append order
* better validation when a temporary resource escapes the graph accidentally

P2 follow-up work moved to later priorities:

* Add full handle-native pass builder and descriptor binding APIs so renderers no longer need to bind physical RHI resources while building passes.
* Add versioned logical resource handles when a resource is intentionally written multiple times in one frame.
* Add transient resource pooling and aliasing after lifetimes and pass ordering are validated.
* Remove the old physical-resource declaration path after migrated passes no longer need it.

## Priority 3: More Precise Access States

Current access is mostly `RHIAccessMode::eRead` / `eReadWrite` plus texture or buffer usage. This is enough for many cases but can over-synchronize and makes validation less expressive.

Introduce a more precise RDG access enum:

```text
ColorAttachmentRead
ColorAttachmentWrite
DepthStencilRead
DepthStencilWrite
ShaderRead
ShaderWrite
TransferRead
TransferWrite
IndirectRead
VertexBufferRead
IndexBufferRead
PresentRead
```

This should map to RHI usage/layout/access masks internally, instead of asking pass builders to encode low-level details repeatedly.

Benefits:

* clearer pass declarations
* better barrier minimization
* easier backend mapping for Vulkan/D3D12/Metal
* better debug messages

## Priority 4: Subresource State Tracking

The current graph stores `RHITextureSubResourceRange`, which is good. The next step is tracking state per relevant mip/layer/aspect range rather than one state per physical texture.

This matters for:

* mip generation
* cubemap/environment preprocessing
* shadow map arrays
* texture streaming
* partial texture updates
* read/write overlap validation

The system does not need a complex interval tree immediately. A first version can store per-resource range states and merge adjacent compatible states after each transition.

## Priority 5: Pass Declaration vs Command Recording

Current RDG has command nodes such as bind pipeline, bind vertex buffer, draw, dispatch, set scissor, and set viewport. This is explicit, but it forces RenderGraph to grow a node type for every future command.

Mature RDGs usually separate:

```text
Pass declaration: resources, queue, flags, attachments
Pass execution: callback/lambda records commands
```

Example direction:

```cpp
graph.AddGraphicsPass("UI", params,
    [](RHICommandList& cmd, const UIParams& params)
    {
        cmd.BeginRendering(params.layout);
        ...
        cmd.EndRendering();
    });
```

Benefits:

* graph compiler focuses on resources/scheduling
* renderer code can record flexible RHI commands
* easier to support ImGui draw data, variable descriptor changes, indirect pipelines, etc.
* fewer RDG command-node structs and switch cases

This can be introduced gradually. Existing command nodes can remain while new passes use callbacks.

## Priority 6: Pass Culling

Add compile-time pass culling for work that does not affect an exported/imported output.

Rules:

* keep passes that write imported or persistent resources
* keep passes with side effects, such as readback or explicit debug markers
* remove passes that only write transient resources never read later

Benefits:

* avoids dead postprocess/debug/temp passes
* makes optional renderer features cheaper to toggle
* requires logical/transient resources to be most effective

## Priority 7: Async Compute and Transfer Scheduling

Current transfer-queue eligibility is graph-level. Mature render graphs schedule at pass level.

Current upload behavior:

* Texture uploads still build a standalone `texture_upload` RDG and call `RenderDevice::ExecuteRenderGraph(RenderGraph&)`.
* That path calls `RDGExecutor::ShouldExecuteOnTransferQueue()` to decide whether the whole upload graph can run on the immediate transfer command list or must fall back to graphics.
* Buffer uploads currently bypass RDG and record `CopyBuffer()` directly into the immediate transfer command list, then notify the persistent resource state tracker.

This graph-level decision is a stepping stone, not the final design. Once RDG supports async transfer scheduling internally, upper layers should not manually choose upload/transfer/graphics command lists for upload work. Upload work should be expressed as RDG nodes, and the RDG compiler/executor should classify nodes into queue-capable workloads.

Add pass flags:

```text
Graphics
Compute
AsyncCompute
Copy
Readback
NeverCull
```

Then let the executor insert:

* queue ownership transfers
* semaphores/fences/timeline waits
* command-list grouping per queue

Future executor direction:

```text
RDG nodes
    -> dependency analysis
    -> queue capability classification
    -> graphics / compute / transfer workloads
    -> barriers, queue ownership transfers, semaphore waits/signals
    -> submit workloads in dependency order
```

In that model, `ShouldExecuteOnTransferQueue(RenderGraph*)` should disappear from the public `RenderDevice::ExecuteRenderGraph()` decision path. It may be replaced by internal node/workload queue selection inside `RDGExecutor`.

Good candidates:

* texture uploads
* buffer uploads
* GPU culling
* voxel lighting compute
* postprocess compute
* readbacks

## Priority 8: Render Pass Merging

Adjacent graphics passes with compatible render targets can be merged or at least recorded in the same command buffer scope.

Examples:

```text
Skybox writes viewport color/depth
Deferred loads/writes viewport color
UI loads/writes viewport color
```

The graph should detect when attachments, load/store ops, layouts, and dependencies allow reducing begin/end rendering churn.

Benefits:

* fewer render pass/dynamic rendering boundaries
* fewer workload finalizations if the backend finalizes at rendering boundaries
* lower command-buffer churn
* better tile memory behavior on mobile-style GPUs

## Priority 9: Validation and Diagnostics

Add stronger debug validation:

* read before write/import
* first write uses `Load` without prior contents
* resource written by multiple passes without dependency
* pass reads and writes same resource without explicit allowance
* transient resource used after lifetime ends
* persistent resource missing final state
* invalid queue usage
* incompatible subresource overlaps

Add graph dump support:

```text
DOT  - visual graph dependencies
JSON - ImGui/debug tooling
CSV  - barrier/pass stats
```

Useful runtime stats:

* pass count
* culled pass count
* resource count
* transient resource count
* barrier count
* merged pass count
* command-list count
* graphics/compute/transfer queue split

## Priority 10: Transient Resource Pooling and Aliasing

After logical resources and lifetimes exist, add a transient allocator:

* pool compatible textures/buffers across frames
* alias non-overlapping transient resources
* keep physical resources stable when dimensions/formats match
* release/recycle when graph lifetime ends

This is a later-stage optimization, but it is one of the major benefits of mature RDG systems.

## Priority 11: RDG Recording and Compile Performance

After the graph model is coherent and validation is strong, optimize the CPU cost of recording and compiling the frame RDG. The current P1 flow intentionally rebuilds the frame graph every frame, which is clean and flexible, but static renderer work can make that expensive.

Good candidates:

* cache static draw/pass descriptions for meshes, skybox, shadow map, and voxel setup when inputs have not changed
* use dirty flags so static scene render items are only regenerated when scene topology, material bindings, shaders, or render options change
* split per-frame dynamic data from mostly-static pass structure, such as camera uniforms vs mesh draw topology
* introduce lightweight pass callbacks or command templates so RDG records fewer low-level command nodes every frame
* reuse node/access storage aggressively and reserve based on previous-frame high-water marks
* profile `RenderGraph::SortNodesV2()`, `BuildCompiledNodeList()`, `RDGExecutor::AttachGraphBarriers()`, and renderer `BuildRenderGraph()` calls separately
* track CPU stats for graph recording, compile, barrier generation, and command recording
* add optional incremental compile or cached dependency graph paths for unchanged static subgraphs

The goal is not to return to independent per-renderer RDGs, but to keep the one-frame-RDG design while avoiding unnecessary per-frame rebuild work for static content.

## Suggested Implementation Order

1. Add frame-level RDG execution across all renderer RDGs.
2. Add imported/transient/persistent logical resource handles.
3. Add precise access flags and stronger validation.
4. Add subresource-aware state tracking.
5. Add optional pass execution callbacks.
6. Add pass culling.
7. Add pass-level graphics/compute/copy scheduling.
8. Add render pass merging.
9. Add graph visualization/debug tooling.
10. Add transient resource pooling and memory aliasing.
11. Optimize RDG recording/compile performance for static and mostly-static workloads.

## Notes for UIRenderer

The future `UIRenderer` should be a normal RenderCore V2 renderer that appends its RDG last in `RendererServer::DispatchRenderWorkloads()`.

The UI pass should:

* render to `RHIViewport::GetColorBackBuffer()`
* use color load/store, not clear
* avoid a depth target initially
* declare normal RDG color attachment access
* record ImGui draw data through RHI commands

This keeps UI visible to RenderGraph transitions and avoids a Vulkan-specific draw path inside `VulkanViewport::Present()`.
