if (BUILD_TESTING)
add_executable(ThreadPoolTest ThreadPoolTest/main.cpp)
add_executable(FlatHashMapTest FlatHashMapTest/FlatHashMapTests.cpp)
add_executable(VulkanRHITest
    CommonTest/ShaderReflectionTests.cpp
    CommonTest/VulkanSynchronizationTests.cpp
    CommonTest/VulkanLifetimeTrackerTests.cpp
    CommonTest/VulkanResourceCreationTests.cpp
    CommonTest/VulkanVerificationTests.cpp
    CommonTest/VulkanQueueSelectionTests.cpp
)
target_link_libraries(VulkanRHITest PRIVATE ZenCore gtest_main)
# Explicit integration target: requires a Vulkan device and validation layer.
add_executable(VulkanRHIIntegrationTest
    CommonTest/RHIProgressIntegrationTests.cpp
    CommonTest/RHIWindowThreadingTests.cpp
    CommonTest/VulkanDescriptorIntegrationTests.cpp
    CommonTest/VulkanDescriptorLifetimeTests.cpp
    CommonTest/VulkanQueueIntegrationTests.cpp
    CommonTest/VulkanCommandBufferLifetimeTests.cpp
    CommonTest/VulkanGPUTimingIntegrationTests.cpp
    CommonTest/VulkanPipelineIntegrationTests.cpp
    CommonTest/VulkanProductionFailureTests.cpp
    CommonTest/VulkanBindingIntegrationTests.cpp
    CommonTest/VulkanSwapchainIntegrationTests.cpp
    CommonTest/VulkanCapabilityIntegrationTests.cpp
    CommonTest/VulkanAttachmentIntegrationTests.cpp
    CommonTest/VulkanRecordingIntegrationTests.cpp
    CommonTest/VulkanBindlessRetirementIntegrationTests.cpp
    CommonTest/VulkanAsyncComputeIntegrationTests.cpp
    CommonTest/VulkanUploadIntegrationTests.cpp
    CommonTest/BindlessSceneCapacityIntegrationTests.cpp
)
target_link_libraries(VulkanRHIIntegrationTest PRIVATE ZenCore gtest_main)
# Compile reflection fixtures as test inputs, without adding them to engine shader assets.
set(RDG_REFLECTION_TEST_PATH "${CMAKE_CURRENT_BINARY_DIR}/rdg-reflection/")
set(RDG_REFLECTION_TEST_BINARIES)
foreach(SHADER rdg_access.comp rdg_shared.vert rdg_shared.frag
        descriptor_storage.comp descriptor_array.comp descriptor_bindings.comp
        descriptor_uniform.comp descriptor_uniform_large.comp descriptor_uniform_limit.comp
        pipeline_specialization.comp pipeline.vert pipeline.frag pipeline_depth.frag
        binding_sampler.comp binding_uniform_array.comp binding_bindless.comp binding_bindless.frag binding_push_constants.comp
        recording_uniform.frag recording_uniform.comp recording_vertex.vert async_resources.comp binding_texel.comp)
    set(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/CommonTest/Shaders/${SHADER}")
    set(OUTPUT "${RDG_REFLECTION_TEST_PATH}${SHADER}.spv")
    add_custom_command(OUTPUT "${OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RDG_REFLECTION_TEST_PATH}"
        COMMAND ${GLSL_VALIDATOR} --target-env spirv1.3 -V "${SOURCE}" -o "${OUTPUT}"
        DEPENDS "${SOURCE}" VERBATIM)
    list(APPEND RDG_REFLECTION_TEST_BINARIES "${OUTPUT}")
endforeach()
add_custom_target(RDGReflectionTestShaders DEPENDS ${RDG_REFLECTION_TEST_BINARIES})
add_dependencies(VulkanRHITest RDGReflectionTestShaders)
add_dependencies(VulkanRHIIntegrationTest RDGReflectionTestShaders)
target_compile_definitions(VulkanRHITest PRIVATE RDG_REFLECTION_TEST_PATH="${RDG_REFLECTION_TEST_PATH}")
target_compile_definitions(VulkanRHIIntegrationTest PRIVATE RDG_REFLECTION_TEST_PATH="${RDG_REFLECTION_TEST_PATH}")

add_executable(RenderCoreTest
    ../ZenCore/Source/Graphics/RenderCore/V2/DebugVisualization.cpp
    RenderCoreTest/RenderCoreTests.cpp
    RenderCoreTest/RDGMetricsTests.cpp
    RenderCoreTest/RDGProfilingTests.cpp
    RenderCoreTest/RHIGPUTimingTests.cpp
    RenderCoreTest/RHIGPUFrameTimingTests.cpp
    RenderCoreTest/VoxelResourcePlanningTests.cpp
    RenderCoreTest/VoxelGISettingsTests.cpp
    ../ZenCore/Source/Memory/Memory.cpp
    ../ZenCore/Source/Graphics/RHI/DynamicRHI.cpp
    ../ZenCore/Source/Graphics/RHI/RHICommandList.cpp
    ../ZenCore/Source/Graphics/RHI/RHIResource.cpp
    ../ZenCore/Source/Graphics/RHI/RHICommandListExecutor.cpp
    ../ZenCore/Source/Graphics/RHI/RHIGPUFrameTiming.cpp
    ../ZenCore/Source/Graphics/RHI/RHIThread.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/RenderSubmissionHistory.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/ShaderProgram.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/TextureManager.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/RenderGraph.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/RDGMetrics.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/StagingUploadQueue.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/RDGPassCompiler.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/RDGResourceManager.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/SceneLighting.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/VoxelResourcePlanning.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/VoxelGISettings.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/VoxelizerBase.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/VoxelGIRenderer.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/SceneShadowRenderer.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/DeferredLightingRenderer.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/SkyboxRenderer.cpp
    ../ZenCore/Source/Graphics/RenderCore/V2/RenderSceneResources.cpp
)
target_include_directories(RenderCoreTest PRIVATE
    ${PROJECT_SOURCE_DIR}/ZenCore/Include
)
target_compile_definitions(RenderCoreTest PRIVATE
    $<TARGET_PROPERTY:ZenCore,INTERFACE_COMPILE_DEFINITIONS>
)
target_link_libraries(RenderCoreTest PRIVATE gtest_main spdlog::spdlog gli glm spirv-reflect)
# The shared fake-backend test translation unit exceeds the standard COFF section limit.
set_source_files_properties(RenderCoreTest/RenderCoreTests.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/bigobj>")
add_dependencies(RenderCoreTest SpvShaders)
add_executable(LRUCacheTest LRUCacheTest/LRUCacheTests.cpp)
target_include_directories(LRUCacheTest PRIVATE ${PROJECT_SOURCE_DIR}/ZenCore/Include)
target_link_libraries(LRUCacheTest PRIVATE gtest_main spdlog::spdlog)
add_executable(InputControllerTest CommonTest/InputControllerTests.cpp)
target_link_libraries(InputControllerTest PRIVATE ZenCore gtest_main)
add_executable(ConfigLoaderTest CommonTest/ConfigLoaderTests.cpp)
target_link_libraries(ConfigLoaderTest PRIVATE ZenCore gtest_main)

add_executable(CommonTest
    CommonTest/CameraInputTests.cpp
    CommonTest/GLTFModelCatalogTests.cpp
    CommonTest/SceneImportTests.cpp
    CommonTest/SceneAnimationTests.cpp
    CommonTest/SceneImportCompatibilityTests.cpp
    CommonTest/SceneImportStressTests.cpp
    CommonTest/SceneImportMorphAttributeTests.cpp
    CommonTest/ScenePhysicalUnitsTests.cpp
    CommonTest/SceneImportPaddingTests.cpp
    CommonTest/SceneAnimationPointerTests.cpp
    CommonTest/CleanupRegressionTests.cpp
    CommonTest/SmallVectorTests.cpp
    CommonTest/NameIDTests.cpp
    CommonTest/MemoryTests.cpp
    CommonTest/ErrorTests.cpp
    CommonTest/LockTests.cpp
    CommonTest/PagedAllocatorTest.h
    CommonTest/PagedAllocatorTest.cpp
)
add_executable(SmartPtrTest
    SmartPtrTest/SharedPtrTests.cpp
    SmartPtrTest/SharedPtrLifetimeTests.cpp
    SmartPtrTest/UniquePtrTests.cpp
    SmartPtrTest/IntrusivePtrTests.cpp
    SmartPtrTest/RefCountPtrTests.cpp
)
add_executable(GLTFCorpusImport CommonTest/GLTFCorpusImport.cpp)

target_link_libraries(GLTFCorpusImport PRIVATE ZenCore)
endif ()


add_executable(scene_renderer_demo
        VulkanRHIDemo/SceneRenderer/SceneRendererDemo.cpp
        VulkanRHIDemo/SceneRenderer/SceneRendererDemoProfiling.cpp
        VulkanRHIDemo/SceneRenderer/SceneRendererDemoVoxelCapture.cpp
        VulkanRHIDemo/SceneRenderer/SceneRendererDemoLightingCapture.cpp
        VulkanRHIDemo/SceneRenderer/SceneRendererDemo.h)
target_link_libraries(scene_renderer_demo ZenCore)

# The optional runtime overlay; ZenUI is configured before the samples.
if (ZEN_BUILD_RUNTIME_UI)
    target_sources(scene_renderer_demo PRIVATE
        VulkanRHIDemo/SceneRenderer/SceneRendererDemoUI.cpp
        VulkanRHIDemo/SceneRenderer/SceneRendererDemoModels.cpp)
    target_compile_definitions(scene_renderer_demo PRIVATE ZEN_RUNTIME_UI=1)
    target_link_libraries(scene_renderer_demo ZenRuntimeUI)

    if (BUILD_TESTING)
        add_executable(SceneModelSwitchTest
            VulkanRHIDemo/SceneRenderer/SceneModelSwitchTests.cpp
            VulkanRHIDemo/SceneRenderer/SceneRendererDemo.cpp
            VulkanRHIDemo/SceneRenderer/SceneRendererDemoModels.cpp
            VulkanRHIDemo/SceneRenderer/SceneRendererDemoUI.cpp
            VulkanRHIDemo/SceneRenderer/SceneRendererDemoProfiling.cpp
            VulkanRHIDemo/SceneRenderer/SceneRendererDemoVoxelCapture.cpp
            VulkanRHIDemo/SceneRenderer/SceneRendererDemoLightingCapture.cpp)
        target_include_directories(SceneModelSwitchTest PRIVATE VulkanRHIDemo/SceneRenderer)
        target_compile_definitions(SceneModelSwitchTest PRIVATE ZEN_RUNTIME_UI=1 ZEN_SCENE_MODEL_TEST=1)
        target_link_libraries(SceneModelSwitchTest PRIVATE ZenRuntimeUI gtest_main)
        add_test(NAME SceneModelSwitchTest COMMAND SceneModelSwitchTest)
        set_tests_properties(SceneModelSwitchTest PROPERTIES LABELS "vulkan;integration" TIMEOUT 120)
    endif ()
endif ()

# Native Cone visibility regression, isolated from suite-global Vulkan fixtures.
if (BUILD_TESTING)
add_executable(ConeVoxelGIIntegrationTest CommonTest/ConeVoxelGIIntegrationTests.cpp)
target_link_libraries(ConeVoxelGIIntegrationTest PRIVATE ZenCore gtest_main)
add_dependencies(ConeVoxelGIIntegrationTest SpvShaders)

target_link_libraries(ThreadPoolTest ZenCore)
target_link_libraries(FlatHashMapTest ZenCore gtest_main)
target_link_libraries(SmartPtrTest ZenCore gtest_main)
target_link_libraries(CommonTest ZenCore gtest_main)
endif ()


# Applications

if (BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if (Python3_Interpreter_FOUND)
        add_test(NAME NoEngineExceptions COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/check_no_exceptions.py)
        set_tests_properties(NoEngineExceptions PROPERTIES LABELS "unit;policy" TIMEOUT 30)
    endif ()
    foreach(TEST_TARGET CommonTest SmartPtrTest FlatHashMapTest LRUCacheTest
            InputControllerTest ConfigLoaderTest VulkanRHITest RenderCoreTest)
        add_test(NAME ${TEST_TARGET} COMMAND ${TEST_TARGET})
        set_tests_properties(${TEST_TARGET} PROPERTIES LABELS "unit" TIMEOUT 120)
    endforeach()
    add_test(NAME VulkanRHIIntegrationTest COMMAND VulkanRHIIntegrationTest)
    set_tests_properties(VulkanRHIIntegrationTest PROPERTIES LABELS "vulkan;integration" TIMEOUT 300)
endif ()

if (BUILD_TESTING)
    # Native acceptance tests deliberately inspect the private window adapter.
    target_include_directories(VulkanRHIIntegrationTest PRIVATE ${PROJECT_SOURCE_DIR}/ZenCore/Source)
    target_link_libraries(VulkanRHIIntegrationTest PRIVATE ZenWindowBackendInternal)
    add_executable(WindowPlatformTest CommonTest/WindowPlatformTests.cpp)
    target_link_libraries(WindowPlatformTest PRIVATE ZenCore ZenWindowBackendInternal gtest_main)
    add_test(NAME WindowPlatformTest COMMAND WindowPlatformTest)
    set_tests_properties(WindowPlatformTest PROPERTIES LABELS "native" TIMEOUT 60)
endif ()
