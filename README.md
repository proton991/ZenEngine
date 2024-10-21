## RenderGraph based Vulkan Renderer

This is a personal project build by myself while learning Vulkan API. It contains 2 parts:

1. `val`: Vulkan Abstraction Layer
2. rendering: high level rendering framework build on `val`:
    * `RenderGraph`: Manage passes, dependencies, resources, barriers.
    * `RenderContext`: Manage frame acquirement and frame presentation.

## M0-2023.10.2

**Current Status:**

Finally! Hello, Triangle! (single pass, fixed shader without input)

![image-20231002194228644](./Doc/imgs/HelloTriangle.png)

**Next steps:**

1. Vertex inputs.
2. Uniform buffers.
3. Load Meshes.
4. Load Textures.
5. More passes.
6. More reasonable barriers.
7. Merging passes, use Subpass dependency instead of pipeline barriers.

## M1-2024.1.3

Implemented Simple PBR with static point lights.
![Sponza](./Doc/imgs/PBR-Simple.png)

Main Features(Implemented):

1. Vulkan Abstraction Layer
2. Simple RenderGraph.
3. Scene Graph.
4. PBR with static point lights.

**Next Steps**

1. Refactor Vulkan Abstraction Layer
    * Main Goal: Robust Pipeline Management, Support for Raytracing.

2. Refactor RenderGraph
    * Main Goal: Barrier Management, Aliasing, Reorder, Multi-RDGPass, Support for Raytracing.

3. Hybrid Rendering.
4. Dynamic Diffuse Global Illumination (DDGI).

## M2-2024.10.21

**Current Status:**

Implemented VulkanRHI and Render framework V2 from scratch (Basic structure and functionality
implemented, but may contain bugs). Already implemented 2 sample applications based
on [Vulkan Samples](https://github.com/SaschaWillems/Vulkan).

**hello_triangle**(with texture)

![hello_triangle_textured](./Doc/imgs/hello_triangle_textured.png)

**gears**

![gears](./Doc/imgs/gears.png)

**Next steps:**

Re-write **all** [Vulkan Samples](https://github.com/SaschaWillems/Vulkan) using ZenEngine's VulkanRHI and RenderCoreV2. 