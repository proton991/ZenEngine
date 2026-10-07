#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/Shared/LightingCapture.h"
#include "Graphics/RenderCore/V2/Renderer/GBuffer.h"
#include "Graphics/RenderCore/V2/ComputeDispatch.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
using namespace zen;
using namespace zen::rc;

class ConeVoxelGIIntegrationTest : public testing::TestWithParam<uint32_t>
{
protected:
    RenderDevice*            device{nullptr};
    HeapVector<RHIBuffer*>   buffers;
    HeapVector<RHITexture*>  textures;
    VkDebugUtilsMessengerEXT messenger{};
    VkInstance               instance{};

    static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                     VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                     void*)
    {
        ADD_FAILURE() << data->pMessage;

        return VK_FALSE;
    }

    void SetUp() override
    {
        RHIOptions::GetInstance().SetRayTracingEnabled(false);

        RHIOptions::GetInstance().SetGPUProfilerMarkers(true);

        device = ZEN_NEW()
            RenderDevice(RHIAPIType::eVulkan, 2, (GetParam() & 1) ? RHIExecutionMode::eThreaded : RHIExecutionMode::eInline,
                         (GetParam() & 2) ? AsyncComputeMode::eAuto : AsyncComputeMode::eDisabled);

        device->Init(nullptr);

        EXPECT_FALSE(GVulkanRHI->GetDevice()->GetExtensionFlags().hasAccelerationStructure);

        EXPECT_FALSE(GVulkanRHI->GetDevice()->GetExtensionFlags().hasRayQuery);

        EXPECT_FALSE(GVulkanRHI->GetDevice()->GetExtensionFlags().hasRaytracingPipeline);

        instance = GVulkanRHI->GetInstance();

        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};

        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;

        info.pfnUserCallback = Validation;

        ASSERT_EQ(vkCreateDebugUtilsMessengerEXT(instance, &info, nullptr, &messenger), VK_SUCCESS);

        ShaderProgramManager::GetInstance().BuildShaderPrograms(device);
    }

    void TearDown() override
    {
        device->FlushRHIThread();

        device->WaitForIdle();

        for (RHIBuffer* buffer : buffers)
        {
            device->DestroyBuffer(buffer);
        }

        for (RHITexture* texture : textures)
        {
            device->DestroyTexture(texture);
        }

        ShaderProgramManager::GetInstance().Destroy();

        vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);

        device->Destroy();

        ZEN_DELETE(device);

        RHIOptions::GetInstance().SetRayTracingEnabled(true);

        RHIOptions::GetInstance().SetGPUProfilerMarkers(false);
    }

    RHIBuffer* Buffer(uint32_t bytes, RHIBufferAllocateType type, const void* data = nullptr)
    {
        RHIBufferCreateInfo info;

        info.size         = bytes;

        info.allocateType = type;

        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer,
                                 RHIBufferUsageFlagBits::eTransferDstBuffer);

        RHIBuffer* buffer = device->CreateBuffer(info);

        buffers.push_back(buffer);

        if (data != nullptr)
        {
            uint8_t* mapped = buffer->Map();

            EXPECT_NE(mapped, nullptr);

            if (mapped != nullptr)
            {
                std::memcpy(mapped, data, bytes);

                buffer->Unmap();
            }
        }

        return buffer;
    }
};

template <class T> HeapVector<T> ReadStaticBuffer(RHIBuffer* buffer)
{
    HeapVector<T> result(buffer->GetRequiredSize() / sizeof(T));

    const uint8_t* data = buffer->Map();

    EXPECT_NE(data, nullptr);

    if (data != nullptr)
    {
        std::memcpy(result.data(), data, buffer->GetRequiredSize());

        buffer->Unmap();
    }

    return result;
}

#include "ConeVoxelVisibilityTests.inl"
#include "ConeEnvironmentTests.inl"
#include "HybridEstimatorTests.inl"
#include "CompactGBufferTests.inl"

INSTANTIATE_TEST_SUITE_P(SubmissionModes, ConeVoxelGIIntegrationTest, testing::Values(0u, 1u, 2u, 3u));
} // namespace
