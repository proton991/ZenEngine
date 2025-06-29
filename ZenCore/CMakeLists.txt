set(ZEN_CORE_HEADERS
    Include/AssetLib/TextureLoader.h
    Include/AssetLib/GLTFLoader.h
    Include/AssetLib/FastGLTFLoader.h
    Include/AssetLib/Types.h

    Include/Common/ArrayView.h
    Include/Common/BitField.h
    Include/Common/Errors.h
    Include/Common/HashMap.h
    Include/Common/Helpers.h
    Include/Common/Math.h
    Include/Common/Memory.h
    Include/Common/Mutex.h
    Include/Common/SpinLock.h
    Include/Common/ConditionVariable.h
    Include/Common/ObjectBase.h
    Include/Common/PagedAllocator.h
    Include/Common/Queue.h
    Include/Common/SmallVector.h
    Include/Common/ThreadPool.h
    Include/Common/Counter.h
    Include/Common/Intrusive.h
    Include/Common/SharedPtr.h
    Include/Common/UniquePtr.h
    Include/Common/RefCountPtr.h

    Include/Graphics/Types/Frustum.h

    Include/Graphics/RenderCore/V2/Renderer/RendererServer.h
    Include/Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h
    Include/Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h
    Include/Graphics/RenderCore/V2/Renderer/VoxelRenderer.h
    Include/Graphics/RenderCore/V2/Renderer/VoxelizerBase.h
    Include/Graphics/RenderCore/V2/Renderer/ComputeVoxelizer.h
    Include/Graphics/RenderCore/V2/Renderer/GeometryVoxelizer.h
    Include/Graphics/RenderCore/V2/Renderer/ShadowMapRenderer.h

    Include/Graphics/RenderCore/V2/RenderGraph.h
    Include/Graphics/RenderCore/V2/RenderScene.h
    Include/Graphics/RenderCore/V2/RenderObject.h
    Include/Graphics/RenderCore/V2/RenderDevice.h
    Include/Graphics/RenderCore/V2/RenderConfig.h
    Include/Graphics/RenderCore/V2/RenderCoreDefs.h
    Include/Graphics/RenderCore/V2/TextureManager.h
    Include/Graphics/RenderCore/V2/ShaderProgram.h

    Include/Graphics/RenderCore/RenderConfig.h
    Include/Graphics/RenderCore/RenderGraphDefinitions.h
    Include/Graphics/RenderCore/RenderGraph.h
    Include/Graphics/RenderCore/RenderDevice.h
    Include/Graphics/RenderCore/ResourceCache.h
    Include/Graphics/RenderCore/RenderFrame.h
    Include/Graphics/RenderCore/RenderContext.h
    Include/Graphics/RenderCore/RenderBuffers.h
    Include/Graphics/RenderCore/ShaderManager.h
    Include/Graphics/RenderCore/TextureManager.h

    Include/SceneGraph/AABB.h
    Include/SceneGraph/Node.h
    Include/SceneGraph/Component.h
    Include/SceneGraph/Light.h
    Include/SceneGraph/Material.h
    Include/SceneGraph/Mesh.h
    Include/SceneGraph/Node.h
    Include/SceneGraph/Sampler.h
    Include/SceneGraph/Scene.h
    Include/SceneGraph/SubMesh.h
    Include/SceneGraph/Texture.h
    Include/SceneGraph/Transform.h
    Include/SceneGraph/Camera.h

    Include/Systems/SceneEditor.h

    Include/Graphics/RHI/DynamicRHI.h
    Include/Graphics/RHI/RHICommands.h
    Include/Graphics/RHI/RHICommon.h
    Include/Graphics/RHI/RHIDebug.h
    Include/Graphics/RHI/RHIResource.h
    Include/Graphics/RHI/RHIDefs.h
    Include/Graphics/RHI/RHIOptions.h
    Include/Graphics/RHI/ShaderUtil.h

    Include/Graphics/VulkanRHI/Platform/VulkanPlatformCommon.h
    Include/Graphics/VulkanRHI/Platform/VulkanWindowsPlatform.h
    Include/Graphics/VulkanRHI/Platform/VulkanMacOSPlatform.h
    Include/Graphics/VulkanRHI/VulkanExtension.h
    Include/Graphics/VulkanRHI/VulkanDevice.h
    Include/Graphics/VulkanRHI/VulkanHeaders.h
    Include/Graphics/VulkanRHI/VulkanRHI.h
    Include/Graphics/VulkanRHI/VulkanCommon.h
    Include/Graphics/VulkanRHI/VulkanDebug.h
    Include/Graphics/VulkanRHI/VulkanQueue.h
    Include/Graphics/VulkanRHI/VulkanSwapchain.h
    Include/Graphics/VulkanRHI/VulkanViewport.h
    Include/Graphics/VulkanRHI/VulkanCommandBuffer.h
    Include/Graphics/VulkanRHI/VulkanCommands.h
    Include/Graphics/VulkanRHI/VulkanTypes.h
    Include/Graphics/VulkanRHI/VulkanPipeline.h
    Include/Graphics/VulkanRHI/VulkanRenderPass.h
    Include/Graphics/VulkanRHI/VulkanTexture.h
    Include/Graphics/VulkanRHI/VulkanBuffer.h
    Include/Graphics/VulkanRHI/VulkanMemory.h
    Include/Graphics/VulkanRHI/VulkanSynchronization.h
    Include/Graphics/VulkanRHI/VulkanResourceAllocator.h

    Include/Graphics/Val/Buffer.h
    Include/Graphics/Val/CommandBuffer.h
    Include/Graphics/Val/CommandPool.h
    Include/Graphics/Val/Descriptors.h
    Include/Graphics/Val/DescriptorSetLayout.h
    Include/Graphics/Val/Device.h
    Include/Graphics/Val/Framebuffer.h
    Include/Graphics/Val/GraphicsPipeline.h
    Include/Graphics/Val/Image.h
    Include/Graphics/Val/Instance.h
    Include/Graphics/Val/PhysicalDevice.h
    Include/Graphics/Val/PipelineLayout.h
    Include/Graphics/Val/PipelineState.h
    Include/Graphics/Val/Queue.h
    Include/Graphics/Val/RenderPass.h
    Include/Graphics/Val/Sampler.h
    Include/Graphics/Val/Shader.h
    Include/Graphics/Val/Swapchain.h
    Include/Graphics/Val/SyncObjPool.h
    Include/Graphics/Val/DeviceObject.h
    Include/Graphics/Val/VulkanConfig.h
    Include/Graphics/Val/VulkanDebug.h
    Include/Graphics/Val/VulkanStrings.h
    Include/Graphics/Val/ZenVal.h

    Include/Platform/FileSystem.h
    Include/Platform/GlfwWindow.h
    Include/Platform/InputController.h
    Include/Platform/NativeWindow.h
    Include/Platform/Timer.h
    Include/Platform/ConfigLoader.h
)

set(ZEN_CORE_SOURCES
    Source/AssetLib/TextureLoader.cpp
    Source/AssetLib/GLTFLoader.cpp
    Source/AssetLib/FastGLTFLoader.cpp

    Source/Graphics/RenderCore/V2/RendererServer.cpp
    Source/Graphics/RenderCore/V2/RenderGraph.cpp
    Source/Graphics/RenderCore/V2/RenderScene.cpp
    Source/Graphics/RenderCore/V2/RenderObject.cpp
    Source/Graphics/RenderCore/V2/RenderDevice.cpp
    Source/Graphics/RenderCore/V2/DeferredLightingRenderer.cpp
    Source/Graphics/RenderCore/V2/TextureManager.cpp
    Source/Graphics/RenderCore/V2/SkyboxRenderer.cpp
    Source/Graphics/RenderCore/V2/VoxelRenderer.cpp
    Source/Graphics/RenderCore/V2/ComputeVoxelizer.cpp
    Source/Graphics/RenderCore/V2/GeometryVoxelizer.cpp
    Source/Graphics/RenderCore/V2/VoxelizerBase.cpp
    Source/Graphics/RenderCore/V2/ShadowMapRenderer.cpp
    Source/Graphics/RenderCore/V2/ShaderProgram.cpp

    Source/Graphics/RenderCore/RenderGraph.cpp
    Source/Graphics/RenderCore/RenderDevice.cpp
    Source/Graphics/RenderCore/RenderFrame.cpp
    Source/Graphics/RenderCore/RenderContext.cpp
    Source/Graphics/RenderCore/TextureManager.cpp

    Source/SceneGraph/Scene.cpp
    Source/SceneGraph/Transform.cpp
    Source/SceneGraph/Camera.cpp

    Source/Systems/SceneEditor.cpp

    Source/Graphics/RHI/RHIFactory.cpp

    Source/Graphics/VulkanRHI/Platform/VulkanWindowsPlatform.cpp
    Source/Graphics/VulkanRHI/Platform/VulkanMacOSPlatform.cpp
    Source/Graphics/VulkanRHI/VulkanExtension.cpp
    Source/Graphics/VulkanRHI/VulkanDevice.cpp
    Source/Graphics/VulkanRHI/VulkanContext.cpp
    Source/Graphics/VulkanRHI/VulkanDebug.cpp
    Source/Graphics/VulkanRHI/VulkanQueue.cpp
    Source/Graphics/VulkanRHI/VulkanSwapchain.cpp
    Source/Graphics/VulkanRHI/VulkanViewport.cpp
    Source/Graphics/VulkanRHI/VulkanCommandBuffer.cpp
    Source/Graphics/VulkanRHI/VulkanCommands.cpp
    Source/Graphics/VulkanRHI/VulkanTypes.cpp
    Source/Graphics/VulkanRHI/VulkanPipeline.cpp
    Source/Graphics/VulkanRHI/VulkanRenderPass.cpp
    Source/Graphics/VulkanRHI/VulkanTexture.cpp
    Source/Graphics/VulkanRHI/VulkanBuffer.cpp
    Source/Graphics/VulkanRHI/VulkanMemory.cpp
    Source/Graphics/VulkanRHI/VulkanSynchronization.cpp

    Source/Graphics/Val/Buffer.cpp
    Source/Graphics/Val/CommandBuffer.cpp
    Source/Graphics/Val/CommandPool.cpp
    Source/Graphics/Val/Descriptors.cpp
    Source/Graphics/Val/DescriptorSetLayout.cpp
    Source/Graphics/Val/Device.cpp
    Source/Graphics/Val/Framebuffer.cpp
    Source/Graphics/Val/GraphicsPipeline.cpp
    Source/Graphics/Val/Image.cpp
    Source/Graphics/Val/Instance.cpp
    Source/Graphics/Val/PhysicalDevice.cpp
    Source/Graphics/Val/PipelineLayout.cpp
    Source/Graphics/Val/PipelineState.cpp
    Source/Graphics/Val/Queue.cpp
    Source/Graphics/Val/RenderPass.cpp
    Source/Graphics/Val/Sampler.cpp
    Source/Graphics/Val/Shader.cpp
    Source/Graphics/Val/Swapchain.cpp
    Source/Graphics/Val/SyncObjPool.cpp
    Source/Graphics/Val/VulkanDebug.cpp

    Source/Platform/FileSystem.cpp
    Source/Platform/GlfwWindow.cpp
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
    glfw
    spdlog
    gli
    glm
    vma
    spriv-reflect
    spirv-cross-glsl
    volk
    span
    stb
    tinygltf
    fastgltf
)

############################################################
#                    Build Shaders                         #
############################################################
set(SHADER_SRC_PATH ${PROJECT_SOURCE_DIR}/Data/Shaders/)
set(SHADER_SPV_PATH ${PROJECT_SOURCE_DIR}/Data/SpvShaders/)
file(GLOB_RECURSE ZEN_SHADER_FILES
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
        DEPENDS ${GLSL})
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
