set(ZEN_CORE_HEADERS
    Include/Graphics/RenderCore/V2/Renderer/DebugVisualization.h
    Include/Graphics/RenderCore/V2/RenderingSettings.h
    Include/ObjectBase.h

    Include/AssetLib/TextureLoader.h
    Include/AssetLib/FastGLTFLoader.h
    Include/AssetLib/GLTFModelCatalog.h
    Include/AssetLib/Types.h

    Include/Templates/ArrayView.h
    Include/Templates/BitField.h
    Include/Templates/BitMask.h
    Include/Templates/HashMap.h
    Include/Templates/Queue.h
    Include/Templates/SmallVector.h
    Include/Templates/VectorView.h
    Include/Templates/ArenaVector.h
    Include/Templates/HeapVector.h
    Include/Templates/NameID.h
    Include/Templates/FlatHashMap.h
    Include/Templates/ObjectPool.h
    Include/Templates/FrameTimeline.h
    Include/Templates/LRUCache.h

    Include/Utils/ConditionVariable.h
    Include/Utils/Counter.h
    Include/Utils/Errors.h
    Include/Utils/MetricsLogger.h
    Include/Utils/Helpers.h
    Include/Utils/Intrusive.h
    Include/Utils/Mutex.h
    Include/Utils/RefCountPtr.h
    Include/Utils/SharedPtr.h
    Include/Utils/SpinLock.h
    Include/Utils/ThreadPool.h
    Include/Utils/UniquePtr.h

    Include/Math/Math.h

    Include/Memory/Memory.h
    Include/Memory/PagedAllocator.h
    Include/Memory/LinearAllocator.h
    Include/Memory/PoolAllocator.h

    Include/Graphics/Common/Format.h
    Include/Graphics/Common/Color.h

    Include/Graphics/Types/Frustum.h

    Include/Graphics/RenderCore/V2/RenderGraph/RDGDefs.h
    Include/Graphics/RenderCore/V2/RenderGraph/RDGSchedule.h
    Include/Graphics/RenderCore/V2/RenderGraph/RDGMetrics.h
    Include/Graphics/RenderCore/V2/RenderGraph/RDGPassCompiler.h
    Include/Graphics/RenderCore/V2/RenderGraph/RDGResourceManager.h
    Include/Graphics/RenderCore/V2/RenderGraph/RenderGraph.h

    Include/Graphics/RenderCore/V2/Renderer/RendererServer.h
    Include/Graphics/RenderCore/V2/Renderer/RenderOverlay.h
    Include/Graphics/RenderCore/V2/Renderer/RendererUtils.h
    Include/Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h
    Include/Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h
    Include/Graphics/RenderCore/V2/Renderer/VoxelRenderer.h
    Include/Graphics/RenderCore/V2/Renderer/VoxelizerBase.h
    Include/Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h
    Include/Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h
    Include/Graphics/RenderCore/V2/Renderer/ComputeVoxelizer.h
    Include/Graphics/RenderCore/V2/Renderer/GeometryVoxelizer.h
    Include/Graphics/RenderCore/V2/Renderer/ShadowMapRenderer.h

    Include/Graphics/RenderCore/V2/RenderScene.h
    Include/Graphics/RenderCore/V2/RenderObject.h
    Include/Graphics/RenderCore/V2/RenderResource.h
    Include/Graphics/RenderCore/V2/RenderDevice.h
    Include/Graphics/RenderCore/V2/RenderSubmissionHistory.h
    Include/Graphics/RenderCore/V2/RenderConfig.h
    Include/Graphics/RenderCore/V2/ComputeDispatch.h
    Include/Graphics/RenderCore/V2/VoxelResourcePlanning.h
    Include/Graphics/RenderCore/V2/RenderCoreDefs.h
    Include/Graphics/RenderCore/V2/TextureManager.h
    Include/Graphics/RenderCore/V2/ShaderProgram.h
    Include/Graphics/RenderCore/V2/RenderFrameState.h
    Include/Graphics/RenderCore/V2/StagingUploadQueue.h


    Include/SceneGraph/AABB.h
    Include/SceneGraph/Node.h
    Include/SceneGraph/Component.h
    Include/SceneGraph/Light.h
    Include/SceneGraph/Material.h
    Include/SceneGraph/Mesh.h
    Include/SceneGraph/Sampler.h
    Include/SceneGraph/Scene.h
    Include/SceneGraph/SceneAsset.h
    Include/SceneGraph/SceneAnimation.h
    Include/SceneGraph/MaterialFeatures.h
    Include/SceneGraph/SubMesh.h
    Include/SceneGraph/Texture.h
    Include/SceneGraph/Transform.h
    Include/SceneGraph/Camera.h

    Include/Systems/SceneEditor.h

    Include/Graphics/RHI/DynamicRHI.h
    Include/Graphics/RHI/RHICommandList.h
    Include/Graphics/RHI/RHIGPUTiming.h
    Include/Graphics/RHI/RHIGPUFrameTiming.h
    Include/Graphics/RHI/RHICommandListExecutor.h
    Include/Graphics/RHI/RHIExecutionCounters.h
    Include/Graphics/RHI/RHIThread.h
    Include/Graphics/RHI/RHICommon.h
    Include/Graphics/RHI/RHIDebug.h
    Include/Graphics/RHI/RHIResource.h
    Include/Graphics/RHI/RHIShaderParameters.h
    Include/Graphics/RHI/RHIFrameState.h
    Include/Graphics/RHI/RHIOptions.h
    Include/Graphics/RHI/RHIShaderUtil.h

    Include/Graphics/VulkanRHI/Platform/VulkanPlatformCommon.h
    Include/Graphics/VulkanRHI/Platform/VulkanWindowsPlatform.h
    Include/Graphics/VulkanRHI/Platform/VulkanMacOSPlatform.h
    Include/Graphics/VulkanRHI/VulkanExtension.h
    Include/Graphics/VulkanRHI/VulkanDevice.h
    Include/Graphics/VulkanRHI/VulkanQueueSelection.h
    Include/Graphics/VulkanRHI/VulkanHeaders.h
    Include/Graphics/VulkanRHI/VulkanRHI.h
    Include/Graphics/VulkanRHI/VulkanCommon.h
    Include/Graphics/VulkanRHI/VulkanDebug.h
    Include/Graphics/VulkanRHI/VulkanQueue.h
    Include/Graphics/VulkanRHI/VulkanLifetimeTracker.h
    Include/Graphics/VulkanRHI/VulkanSwapchain.h
    Include/Graphics/VulkanRHI/VulkanViewport.h
    Include/Graphics/VulkanRHI/VulkanCommandList.h
    Include/Graphics/VulkanRHI/VulkanPlatformCommandList.h
    Include/Graphics/VulkanRHI/VulkanResourceCast.h
    Include/Graphics/VulkanRHI/VulkanTypes.h
    Include/Graphics/VulkanRHI/VulkanPipeline.h
    Include/Graphics/VulkanRHI/VulkanTexture.h
    Include/Graphics/VulkanRHI/VulkanBuffer.h
    Include/Graphics/VulkanRHI/VulkanMemory.h
    Include/Graphics/VulkanRHI/VulkanSynchronization.h
    Include/Graphics/VulkanRHI/VulkanResourceAllocator.h
    Include/Graphics/VulkanRHI/VulkanDescriptorPool.h
    Include/Graphics/VulkanRHI/VulkanDescriptorState.h


    Include/Platform/FileSystem.h
    Include/Platform/InputController.h
    Include/Platform/NativeWindow.h
    Include/Platform/Timer.h
    Include/Platform/ConfigLoader.h
)

set(ZEN_CORE_SOURCES
    Source/Graphics/RenderCore/V2/DebugVisualization.cpp
    Source/Graphics/RenderCore/V2/RenderingSettings.cpp
    Source/AssetLib/TextureLoader.cpp
    Source/AssetLib/FastGLTFLoader.cpp
    Source/AssetLib/GLTFModelCatalog.cpp

    Source/Graphics/RenderCore/V2/RendererServer.cpp
    Source/Graphics/RenderCore/V2/RenderGraph.cpp
    Source/Graphics/RenderCore/V2/RDGMetrics.cpp
    Source/Graphics/RenderCore/V2/RDGPassCompiler.cpp
    Source/Graphics/RenderCore/V2/RDGResourceManager.cpp
    Source/Graphics/RenderCore/V2/SceneLighting.cpp
    Source/Graphics/RenderCore/V2/VoxelResourcePlanning.cpp
    Source/Graphics/RenderCore/V2/VoxelGISettings.cpp
    Source/Graphics/RenderCore/V2/RendererServerSettings.cpp
    Source/Graphics/RenderCore/V2/RenderScene.cpp
    Source/Graphics/RenderCore/V2/RenderSceneResources.cpp
    Source/Graphics/RenderCore/V2/RenderSceneGeometry.cpp
    Source/Graphics/RenderCore/V2/RenderObject.cpp
    Source/Graphics/RenderCore/V2/RenderResource.cpp
    Source/Graphics/RenderCore/V2/RenderDevice.cpp
    Source/Graphics/RenderCore/V2/RenderSubmissionHistory.cpp
    Source/Graphics/RenderCore/V2/StagingUploadQueue.cpp
    Source/Graphics/RenderCore/V2/DeferredLightingRenderer.cpp
    Source/Graphics/RenderCore/V2/TextureManager.cpp
    Source/Graphics/RenderCore/V2/SkyboxRenderer.cpp
    Source/Graphics/RenderCore/V2/VoxelRenderer.cpp
    Source/Graphics/RenderCore/V2/ComputeVoxelizer.cpp
    Source/Graphics/RenderCore/V2/GeometryVoxelizer.cpp
    Source/Graphics/RenderCore/V2/VoxelizerBase.cpp
    Source/Graphics/RenderCore/V2/HybridGIRenderer.cpp
    Source/Graphics/RenderCore/V2/SceneRayQuery.cpp
    Source/Graphics/RenderCore/V2/VoxelGIRenderer.cpp
    Source/Graphics/RenderCore/V2/SceneShadowRenderer.cpp
    Source/Graphics/RenderCore/V2/ShadowMapRenderer.cpp
    Source/Graphics/RenderCore/V2/ShaderProgram.cpp


    Source/SceneGraph/Scene.cpp
    Source/SceneGraph/SceneAnimation.cpp
    Source/SceneGraph/SceneAnimationPointer.cpp
    Source/SceneGraph/Transform.cpp
    Source/SceneGraph/Camera.cpp

    Source/Systems/SceneEditor.cpp

    Source/Graphics/RHI/DynamicRHI.cpp
    Source/Graphics/RHI/RHICommandList.cpp
    Source/Graphics/RHI/RHIResource.cpp
    Source/Graphics/RHI/RHICommandListExecutor.cpp
    Source/Graphics/RHI/RHIGPUFrameTiming.cpp
    Source/Graphics/RHI/RHIThread.cpp
    Source/Graphics/RHI/RHIFactory.cpp

    Source/Graphics/VulkanRHI/Platform/VulkanWindowsPlatform.cpp
    Source/Graphics/VulkanRHI/Platform/VulkanMacOSPlatform.cpp
    Source/Graphics/VulkanRHI/VulkanExtension.cpp
    Source/Graphics/VulkanRHI/VulkanDevice.cpp
    Source/Graphics/VulkanRHI/VulkanContext.cpp
    Source/Graphics/VulkanRHI/VulkanDebug.cpp
    Source/Graphics/VulkanRHI/VulkanQueue.cpp
    Source/Graphics/VulkanRHI/VulkanLifetimeTracker.cpp
    Source/Graphics/VulkanRHI/VulkanSwapchain.cpp
    Source/Graphics/VulkanRHI/VulkanViewport.cpp
    Source/Graphics/VulkanRHI/VulkanCommandList.cpp
    Source/Graphics/VulkanRHI/VulkanTypes.cpp
    Source/Graphics/VulkanRHI/VulkanPipeline.cpp
    Source/Graphics/VulkanRHI/VulkanTexture.cpp
    Source/Graphics/VulkanRHI/VulkanBuffer.cpp
    Source/Graphics/VulkanRHI/VulkanAccelerationStructure.cpp
    Source/Graphics/VulkanRHI/VulkanMemory.cpp
    Source/Graphics/VulkanRHI/VulkanSynchronization.cpp
    Source/Graphics/VulkanRHI/VulkanDescriptorPool.cpp
    Source/Graphics/VulkanRHI/VulkanDescriptorSetState.cpp


    Source/Memory/Memory.cpp

    Source/Platform/FileSystem.cpp
    Source/Platform/NativeWindow.cpp
    Source/Graphics/VulkanRHI/Platform/VulkanWindowBridge.cpp
    Source/Platform/InputController.cpp

    Source/vk_mem_alloc.cpp
)


add_library(ZenCore)
target_sources(ZenCore PRIVATE ${ZEN_CORE_HEADERS} ${ZEN_CORE_SOURCES})
target_compile_definitions(ZenCore PUBLIC ZEN_DEBUG)
target_include_directories(ZenCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Include
    ${VULKAN_INCLUDE_DIR}
)

target_link_libraries(ZenCore PUBLIC
    spdlog
    gli
    glm
    vma
    spirv-reflect
    volk
    stb
    fastgltf
    zen_gltf_decoders
)


############################################################
#                    Build Shaders                         #
############################################################
set(SHADER_SRC_PATH ${PROJECT_SOURCE_DIR}/Data/Shaders/)
set(SHADER_SPV_PATH ${PROJECT_SOURCE_DIR}/Data/SpvShaders/)
file(GLOB_RECURSE ZEN_SHADER_FILES CONFIGURE_DEPENDS
    "${SHADER_SRC_PATH}/*.frag"
    "${SHADER_SRC_PATH}/*.vert"
    "${SHADER_SRC_PATH}/*.geom"
    "${SHADER_SRC_PATH}/*.comp"
)

message(STATUS "Spv Shader path: ${SHADER_SPV_PATH}")

foreach (GLSL ${ZEN_SHADER_FILES})
    get_filename_component(FILE_NAME ${GLSL} NAME)
    get_filename_component(FILE_PATH ${GLSL} DIRECTORY)
    file(RELATIVE_PATH RELATIVE_FILE_PATH ${SHADER_SRC_PATH} ${FILE_PATH})
    set(SPIRV "${SHADER_SPV_PATH}/${RELATIVE_FILE_PATH}/${FILE_NAME}.spv")
    message(STATUS "Compiling shader ${GLSL} -> ${SPIRV}")
    add_custom_command(
        OUTPUT ${SPIRV}
        COMMAND ${GLSL_VALIDATOR} --target-env spirv1.3 -V ${GLSL} -o ${SPIRV}
                -I${PROJECT_SOURCE_DIR}/ZenCore/Include
                --depfile ${SPIRV}.d
        DEPENDS ${GLSL}
        DEPFILE ${SPIRV}.d
        VERBATIM)
    list(APPEND SPIRV_BINARY_FILES ${SPIRV})
endforeach (GLSL)

add_custom_target(
    SpvShaders
    DEPENDS ${SPIRV_BINARY_FILES}
    SOURCES ${ZEN_SHADER_FILES}
)

## Add a custom command to delete the output file before building the shader target
#add_custom_command(
#    TARGET zen_shaders
#    PRE_BUILD
#    COMMAND ${CMAKE_COMMAND} -E remove ${SPIRV_BINARY_FILES}
#    COMMENT "Deleting shader output file"
#)


add_dependencies(ZenCore SpvShaders)
if (WIN32)
    target_compile_definitions(ZenCore PUBLIC NOMINMAX)
endif ()
target_compile_definitions(ZenCore PUBLIC SPV_SHADER_PATH="${SHADER_SPV_PATH}")

set(ZEN_TEXTURE_PATH ${PROJECT_SOURCE_DIR}/Data/Textures/)
target_compile_definitions(ZenCore PUBLIC ZEN_TEXTURE_PATH="${ZEN_TEXTURE_PATH}")
target_compile_definitions(ZenCore PUBLIC ZEN_CONFIG_PATH="${PROJECT_SOURCE_DIR}/Data/engine.cfg")

# Backend headers and third-party includes never propagate through the engine API.
target_include_directories(ZenCore PRIVATE Source)

# The window adapter boundary for backend-specific integrations outside ZenCore: the
# ImGui platform bridge and native window tests. It exposes only that header and the
# selected backend's headers, not ZenCore's other private sources.
add_library(ZenWindowBackendInternal INTERFACE)
target_include_directories(ZenWindowBackendInternal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Internal/Include)
target_link_libraries(ZenWindowBackendInternal INTERFACE zen_window_backend)
target_link_libraries(ZenCore PRIVATE ZenWindowBackendInternal)
if (ZEN_WINDOW_BACKEND STREQUAL "SDL3")
    target_sources(ZenCore PRIVATE Source/Platform/SDLWindow.cpp)
    if (APPLE)
        # OBJCXX is enabled by the top-level project for consumers that link ZenCore.
        target_sources(ZenCore PRIVATE Source/Platform/SDLWindowCocoa.mm)
        target_link_libraries(ZenCore PRIVATE "-framework AppKit")
    endif ()
else ()
    target_sources(ZenCore PRIVATE Source/Platform/GlfwWindow.cpp Source/Platform/GlfwWindowFrame.cpp)
    if (WIN32)
        target_link_libraries(ZenCore PRIVATE comctl32 dwmapi)
    endif ()
endif ()
