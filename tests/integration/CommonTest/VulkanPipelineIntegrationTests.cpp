#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "VulkanPipelineTestFixture.h"

namespace
{
TEST_F(VulkanPipelineIntegrationTest, SpecializedVoxelWorkgroupsClearAnEntireNonAlignedVolume)
{
    const RHIGPUInfo& deviceInfo         = session->rhi.QueryGPUInfo();

    const VkPhysicalDeviceLimits& limits = session->rhi.GetDevice()->GetPhysicalDeviceProperties().limits;

    EXPECT_EQ(deviceInfo.maxComputeWorkGroupInvocations, limits.maxComputeWorkGroupInvocations);

    EXPECT_EQ(deviceInfo.maxStorageBufferRange, limits.maxStorageBufferRange);

    EXPECT_EQ(deviceInfo.supportFragmentStoresAndAtomics,
              session->rhi.GetDevice()->GetPhysicalDeviceFeatures().fragmentStoresAndAtomics != VK_FALSE);

    EXPECT_EQ(deviceInfo.supportIndependentBlend,
              session->rhi.GetDevice()->GetPhysicalDeviceFeatures().independentBlend != VK_FALSE);

    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        EXPECT_EQ(deviceInfo.maxComputeWorkGroupSize[axis], limits.maxComputeWorkGroupSize[axis]);

        EXPECT_EQ(deviceInfo.maxComputeWorkGroupCount[axis], limits.maxComputeWorkGroupCount[axis]);
    }

    constexpr uint32_t dimension = 9;

    for (uint32_t invocations : {64u, 128u, 256u, 512u})
    {
        if (invocations <= deviceInfo.maxComputeWorkGroupInvocations)
        {
            SCOPED_TRACE(invocations);

            RHIGPUInfo policyInfo                     = deviceInfo;

            policyInfo.maxComputeWorkGroupInvocations = invocations;

            const glm::uvec3 size                     = rc::ResolveVoxelVolumeWorkgroupSize(policyInfo);

            const glm::uvec3 groups                   = rc::GetVoxelVolumeDispatchGroups(dimension, policyInfo);

            RHIShaderCreateInfo shaderInfo{};

            shaderInfo.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);

            zen::test::LoadShaderModule(shaderInfo, RHIShaderStage::eCompute,
                                        std::filesystem::path(SPV_SHADER_PATH) / "VoxelGI/clear_owners.comp.spv");

            shaderInfo.specializationConstants = {{ZEN_VOXEL_VOLUME_GROUP_X_ID, static_cast<int>(size.x)},
                                                  {ZEN_VOXEL_VOLUME_GROUP_Y_ID, static_cast<int>(size.y)},
                                                  {ZEN_VOXEL_VOLUME_GROUP_Z_ID, static_cast<int>(size.z)}};

            RHIShader* shader                  = session->rhi.CreateShader(shaderInfo);

            ASSERT_NE(shader, nullptr);

            shaders.push_back(shader);

            RHIPipeline* pipeline = session->rhi.CreatePipeline(RHIComputePipelineCreateInfo{shader});

            ASSERT_NE(pipeline, nullptr);

            pipelines.push_back(pipeline);

            // The owner volume, then the class owners with their classes stacked along z (voxel_surface_class.glsl).
            VulkanTexture* volumes[2] = {nullptr, nullptr};

            for (uint32_t volume = 0; volume < 2; ++volume)
            {
                RHITextureCreateInfo textureInfo{};

                textureInfo.type   = RHITextureType::e3D;

                textureInfo.format = DataFormat::eR32UInt;

                textureInfo.width = textureInfo.height = dimension;

                textureInfo.depth                      = volume == 0 ? dimension : dimension * ZEN_VOXEL_SURFACE_CLASSES;

                textureInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eTransferDst,
                                                RHITextureUsageFlagBits::eTransferSrc);

                VulkanTexture* texture = static_cast<VulkanTexture*>(session->rhi.CreateTexture(textureInfo));

                ASSERT_NE(texture, nullptr);

                textures.push_back(texture);

                Transition(texture, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

                const VkClearColorValue zero{};

                const VkImageSubresourceRange range = texture->GetVkSubresourceRange();

                vkCmdClearColorImage(Commands(), texture->GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);

                Transition(texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

                volumes[volume] = texture;
            }

            context->RHIBindPipeline(pipeline);

            RHIBatchedShaderParameters parameters;

            parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 0), volumes[0]->GetDefaultView(),
                                        nullptr, 0);

            parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 2), volumes[1]->GetDefaultView(),
                                        nullptr, 0);

            context->RHISetShaderParameters(parameters);

            context->RHIDispatch(groups.x, groups.y, groups.z);

            for (VulkanTexture* texture : volumes)
            {
                Transition(texture, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

                RHIBufferCreateInfo bufferInfo{};

                bufferInfo.size         = uint64_t(dimension) * dimension * texture->GetDepth() * sizeof(uint32_t);

                bufferInfo.allocateType = RHIBufferAllocateType::eCPURead;

                bufferInfo.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);

                VulkanBuffer* buffer = static_cast<VulkanBuffer*>(session->rhi.CreateBuffer(bufferInfo));

                ASSERT_NE(buffer, nullptr);

                buffers.push_back(buffer);

                VkBufferImageCopy copy{};

                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};

                copy.imageExtent      = {dimension, dimension, texture->GetDepth()};

                vkCmdCopyImageToBuffer(Commands(), texture->GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       buffer->GetVkBuffer(), 1, &copy);
            }
        }
    }

    SubmitAndWait(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

    for (RHIBuffer* buffer : buffers)
    {
        const uint32_t* values = reinterpret_cast<const uint32_t*>(buffer->Map());

        ASSERT_NE(values, nullptr);

        for (uint64_t voxel = 0; voxel < buffer->GetRequiredSize() / sizeof(uint32_t); ++voxel)
        {
            EXPECT_EQ(values[voxel], UINT32_MAX) << voxel;
        }

        buffer->Unmap();
    }
}

} // namespace
