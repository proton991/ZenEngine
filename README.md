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