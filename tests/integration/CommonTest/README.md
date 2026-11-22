# Engine rendering integration tests

`VulkanRHITest` checks the shipped engine shaders. `VulkanRHIIntegrationTest` exercises the engine window, threaded resize and presentation paths with Vulkan validation. `RenderCoreTest` covers render-graph scheduling, frame slots and resource retirement against a controlled backend.

The lost-surface and initial-zero-extent surface-creation failure tests use the narrow `VulkanViewportTestAccess` friend to run the existing resize transition on the RHI thread with a simulated surface-creation error. SDL resolves its own native Vulkan entry points, so replacing the engine's volk function pointers cannot inject an `SDL_Vulkan_CreateSurface` failure. These tests verify cause preservation and safe failure handling at the window/RHI thread handoff; they do not inject a failure inside SDL itself.

From a configured MSVC build, run the registered suites with CTest.

```powershell
ctest --test-dir build/x64-windows-msvc-debug -R '^(VulkanRHITest|VulkanRHIIntegrationTest|RenderCoreTest|CommonTest)$' --output-on-failure
```

The [standalone Vulkan test guide](../../../ZenRHI/tests/README.md) covers backend fixtures and the native swapchain tests. See the [ownership and verification record](../../../Doc/LibrarySplitVerification.md) for the rest of the split.
