set(ZEN_CORE_HEADERS
        Include/Common/Base.h
        Include/Common/Error.h
        Include/Common/Exception.h
        Include/Common/Logging.h
        Include/Common/SharedPtr.h
        Include/Common/UniquePtr.h
        Include/Common/Helpers.h

        Include/Graphics/Utils/GLSLCompiler.h
        Include/Graphics/Utils/SpirvReflection.h
        Include/Graphics/Utils/ResourceHashing.h
        Include/Graphics/Utils/ResourceCache.h

        Include/Graphics/Vulkan/BufferVK.h
        Include/Graphics/Vulkan/CommandbufferVK.h
        Include/Graphics/Vulkan/CommandPoolVK.h
        Include/Graphics/Vulkan/DeviceContextVK.h
        Include/Graphics/Vulkan/DeviceResource.h
        Include/Graphics/Vulkan/DeviceVK.h
        Include/Graphics/Vulkan/Framebuffer.h
        Include/Graphics/Vulkan/ImageVK.h
        Include/Graphics/Vulkan/RenderPassVK.h
        Include/Graphics/Vulkan/ShaderModuleVK.h
        Include/Graphics/Vulkan/SwapchainVK.h
        Include/Graphics/Vulkan/PipelineVK.h
        Include/Graphics/Vulkan/PipelineState.h
        Include/Graphics/Vulkan/DescriptorSetLayoutVK.h
        Include/Graphics/Vulkan/PipelineLayoutVK.h
        Include/Graphics/Vulkan/ZenVulkan.h

        Include/Platform/FileSystem.h
        Include/Platform/GlfwWindow.h
        Include/Platform/InputController.h
        Include/Platform/NativeWindow.h
        )

set(ZEN_CORE_SOURCES
        Source/Graphics/Utils/GLSLCompiler.cpp
        Source/Graphics/Utils/SpirvReflection.cpp
        Source/Graphics/Utils/ResourceCache.cpp

        Source/Graphics/Vulkan/BufferVK.cpp
        Source/Graphics/Vulkan/CommandbufferVK.cpp
        Source/Graphics/Vulkan/CommandPoolVK.cpp
        Source/Graphics/Vulkan/DeviceContextVK.cpp
        Source/Graphics/Vulkan/DeviceVK.cpp
        Source/Graphics/Vulkan/Framebuffer.cpp
        Source/Graphics/Vulkan/ImageVK.cpp
        Source/Graphics/Vulkan/RenderPassVK.cpp
        Source/Graphics/Vulkan/ShaderModuleVK.cpp
        Source/Graphics/Vulkan/SwapchainVK.cpp
        Source/Graphics/Vulkan/PipelineVK.cpp
        Source/Graphics/Vulkan/DescriptorSetLayoutVK.cpp
        Source/Graphics/Vulkan/PipelineLayoutVK.cpp

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
        ${Vulkan_LIBRARIES}
        glfw
        spdlog
        glm
        vma
        glslang
        SPIRV
        spirv-cross-glsl
        glslang-default-resource-limits
        )
