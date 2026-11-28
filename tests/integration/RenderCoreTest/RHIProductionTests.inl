#include "Utils/Errors.h"
TEST_F(RenderCoreTest, BufferHelpersRejectAllocationFailuresAndRetry)
{
    const uint8_t data[16]{};

    for (uint32_t kind = 0; kind < 5; ++kind)
    {
        rhi->failBufferCreationAt = rhi->bufferCreations + 1;
        RHIBuffer* failed         = nullptr;

        switch (kind)
        {
            case 0: failed = device->CreateVertexBuffer(sizeof(data), data); break;
            case 1: failed = device->CreateIndexBuffer(sizeof(data), data); break;
            case 2: failed = device->CreateUniformBuffer(sizeof(data), data, "failed_uniform"); break;
            case 3: failed = device->CreateStorageBuffer(sizeof(data), data, "failed_storage"); break;
            case 4: failed = device->CreateIndirectBuffer(sizeof(data), data, "failed_indirect"); break;
        }

        EXPECT_EQ(failed, nullptr);
    }

    RHIBuffer* retry = device->CreateStorageBuffer(sizeof(data), data, "retry_storage");
    ASSERT_NE(retry, nullptr);

    device->DestroyBuffer(retry);
}

TEST_F(RenderCoreTest, StagingAllocationFailureDoesNotPublishAnInvalidBlock)
{
    StagingBufferManager manager(64, 128);
    StagingAllocation    allocation;
    rhi->failBufferCreationAt = rhi->bufferCreations + 1;

    EXPECT_EQ(manager.Allocate(16, 4, &allocation), StagingFlushAction::eFailed);
    EXPECT_EQ(allocation.pBuffer, nullptr);
    EXPECT_EQ(manager.Allocate(16, 4, &allocation), StagingFlushAction::eNone);
    ASSERT_NE(allocation.pBuffer, nullptr);

    manager.Release(allocation, {});
    manager.Destroy();
}

TEST(RHIIndexOffsetTest, CommandsAndGraphDescriptorsPreserveOffsetsBeyondFourGiB)
{
    constexpr uint64_t offset = (uint64_t(1) << 32) + 256;
    static_assert(std::is_same_v<decltype(RHIDrawIndexedArgs::indexBufferOffset), uint64_t>);
    static_assert(std::is_same_v<decltype(RHIDrawIndexedIndirectArgs::indexBufferOffset), uint64_t>);

    RDGGraphicsPassDesc pass;
    pass.BindIndexBuffer(static_cast<RHIBuffer*>(nullptr), DataFormat::eR32UInt, offset);
    EXPECT_EQ(pass.geometryBuffer.indexBufferOffset, offset);
}
