set(ZEN_CORE_HEADERS
    Include/Common/Errors.h
    Include/Common/Helpers.h
    Include/Common/MemAlloc.h
    Include/Common/ObjectBase.h
    Include/Common/SharedPtr.h
    Include/Common/UniquePtr.h

    Include/Graphics/Rendering/RenderGraphDefinitions.h
    Include/Graphics/Rendering/RenderGraph.h
    Include/Graphics/Rendering/RenderDevice.h
    Include/Graphics/Rendering/ResourceCache.h

    Include/Graphics/Val/Buffer.h
    Include/Graphics/Val/CommandBuffer.h
    Include/Graphics/Val/CommandPool.h
    Include/Graphics/Val/Descriptors.h
    Include/Graphics/Val/DescriptorSetLayout.h
    Include/Graphics/Val/Device.h
    Include/Graphics/Val/Fence.h
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
    Include/Graphics/Val/Semaphore.h
    Include/Graphics/Val/Shader.h
    Include/Graphics/Val/Swapchain.h
    Include/Graphics/Val/DeviceObject.h
    Include/Graphics/Val/VulkanConfig.h
    Include/Graphics/Val/VulkanDebug.h
    Include/Graphics/Val/VulkanStrings.h
    Include/Graphics/Val/ZenVal.h

    Include/Platform/FileSystem.h
    Include/Platform/GlfwWindow.h
    Include/Platform/InputController.h
    Include/Platform/NativeWindow.h
    )

set(ZEN_CORE_SOURCES
    Source/Graphics/Rendering/RenderGraph.cpp
    Source/Graphics/Rendering/RenderDevice.cpp

    Source/Graphics/Val/Buffer.cpp
    Source/Graphics/Val/CommandBuffer.cpp
    Source/Graphics/Val/CommandPool.cpp
    Source/Graphics/Val/Descriptors.cpp
    Source/Graphics/Val/DescriptorSetLayout.cpp
    Source/Graphics/Val/Device.cpp
    Source/Graphics/Val/Fence.cpp
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
    Source/Graphics/Val/Semaphore.cpp
    Source/Graphics/Val/Shader.cpp
    Source/Graphics/Val/Swapchain.cpp
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
    glm
    vma
    spirv-cross-glsl
    volk
    )

############################################################
#                    Build Shaders                         #
############################################################
set(SHADER_SRC_PATH ${PROJECT_SOURCE_DIR}/Data/Shaders/)
set(SHADER_SPV_PATH ${PROJECT_SOURCE_DIR}/Data/SpvShaders/)
file(GLOB ZEN_SHADER_FILES
    "${SHADER_SRC_PATH}/*.frag"
    "${SHADER_SRC_PATH}/*.vert"
    "${SHADER_SRC_PATH}/*.comp"
    )

message(STATUS "Spv Shader path: ${SHADER_SPV_PATH}")

foreach (GLSL ${ZEN_SHADER_FILES})
  get_filename_component(FILE_NAME ${GLSL} NAME)
  set(SPIRV "${SHADER_SPV_PATH}/${FILE_NAME}.spv")
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
target_compile_definitions(ZenCore PUBLIC SPV_SHADER_PATH= "${SHADER_SPV_PATH}")