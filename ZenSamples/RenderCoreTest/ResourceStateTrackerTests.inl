TEST_F(RenderCoreTest, TrackerOverlayReadsThroughAndRollsBackInvalidation)
{
    RHITexture* texture = Texture(2);

    TestBuffer* buffer  = Buffer();

    ResourceStateTracker base;

    base.UpdateTextureState(texture, RHIAccessMode::eRead, RHITextureUsage::eSampled,
                            BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eFragmentShader));

    base.UpdateBufferState(buffer, RHIAccessMode::eReadWrite,
                           BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eStorageBuffer),
                           BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eComputeShader));

    RDGResourceContent contents;

    contents.status                 = RDGContentStatus::eDefined;

    contents.textureSubresources[1] = RDGContentStatus::eUndefined;

    base.SetContents(texture, contents);

    contents.textureSubresources.clear();

    contents.bufferRanges.push_back({0, 16, RDGContentStatus::eUndefined});

    base.SetContents(buffer, contents);

    const uint64_t revision = base.GetRevision();

    {
        ResourceStateTracker overlay(nullptr, &base);

        EXPECT_EQ(overlay.GetTextureState(texture).usage, RHITextureUsage::eSampled);

        EXPECT_TRUE(overlay.GetBufferState(buffer).writer.hasWriter);

        EXPECT_EQ(overlay.GetContents(texture).textureSubresources.at(1), RDGContentStatus::eUndefined);

        ASSERT_EQ(overlay.GetContents(buffer).bufferRanges.size(), 1u);

        EXPECT_EQ(overlay.GetContents(buffer).bufferRanges[0].end, 16u);

        EXPECT_TRUE(RDGSubmissionTestAccess::HasHistory(overlay, texture->GetStableId()));

        overlay.RemoveResourceState(texture->GetStableId());

        overlay.RemoveResourceState(buffer->GetStableId());

        EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(overlay, texture->GetStableId()));

        EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(overlay, buffer->GetStableId()));

        EXPECT_EQ(overlay.GetTextureState(texture).usage, RHITextureUsage::eNone);

        EXPECT_EQ(overlay.GetBufferState(buffer).accessMode, RHIAccessMode::eNone);

        EXPECT_EQ(overlay.GetContents(texture).status, RDGContentStatus::eUnknown);

        EXPECT_TRUE(overlay.GetContents(buffer).bufferRanges.empty());

        overlay.UpdateTextureState(texture, RHIAccessMode::eReadWrite, RHITextureUsage::eStorage,
                                   BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eComputeShader));

        EXPECT_EQ(overlay.GetTextureState(texture).usage, RHITextureUsage::eStorage);

        EXPECT_EQ(overlay.GetContents(texture).status, RDGContentStatus::eUnknown);

        EXPECT_TRUE(overlay.GetContents(texture).textureSubresources.empty());

        overlay.SetContents(buffer, contents);

        EXPECT_EQ(overlay.GetBufferState(buffer).accessMode, RHIAccessMode::eNone);

        EXPECT_EQ(overlay.GetContents(buffer).status, RDGContentStatus::eDefined);

        EXPECT_EQ(base.GetRevision(), revision);
    }

    EXPECT_EQ(base.GetRevision(), revision);

    EXPECT_EQ(base.GetTextureState(texture).usage, RHITextureUsage::eSampled);

    EXPECT_EQ(base.GetContents(texture).textureSubresources.at(1), RDGContentStatus::eUndefined);

    EXPECT_TRUE(base.GetBufferState(buffer).writer.hasWriter);

    EXPECT_EQ(base.GetContents(buffer).bufferRanges.size(), 1u);

    device->DestroyTexture(texture);

    device->DestroyBuffer(buffer);
}

TEST_F(RenderCoreTest, TrackerOverlayCommitTouchesOnlyItsDeltaAndAdvancesRevisionOnce)
{
    RHITexture* untouched = Texture();

    RHITexture* removed   = Texture();

    TestBuffer* changed   = Buffer();

    TestBuffer* added     = Buffer();

    ResourceStateTracker base;

    RDGResourceContent defined;

    defined.status = RDGContentStatus::eDefined;

    base.SetContents(untouched, defined);

    base.SetContents(removed, defined);

    base.UpdateBufferState(changed, RHIAccessMode::eRead,
                           BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eVertexBuffer),
                           BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eVertexInput));

    base.SetContents(changed, defined);

    const uint64_t revision            = base.GetRevision();

    const RDGResourceContent* original = RDGSubmissionTestAccess::StoredContents(base, untouched->GetStableId());

    ASSERT_NE(original, nullptr);

    ResourceStateTracker overlay(nullptr, &base);

    overlay.RemoveResourceState(removed->GetStableId());

    overlay.UpdateBufferState(changed, RHIAccessMode::eReadWrite,
                              BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eStorageBuffer),
                              BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eComputeShader));

    overlay.UpdateBufferState(added, RHIAccessMode::eReadWrite,
                              BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer),
                              BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eTransfer));

    overlay.SetContents(added, defined);

    EXPECT_EQ(base.GetRevision(), revision);

    EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(base, added->GetStableId()));

    EXPECT_TRUE(RDGSubmissionTestAccess::HasHistory(base, removed->GetStableId()));

    base.CommitOverlay(std::move(overlay));

    EXPECT_EQ(base.GetRevision(), revision + 1);

    EXPECT_EQ(RDGSubmissionTestAccess::StoredContents(base, untouched->GetStableId()), original);

    EXPECT_EQ(original->status, RDGContentStatus::eDefined);

    EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(base, removed->GetStableId()));

    EXPECT_EQ(base.GetBufferState(changed).accessMode, RHIAccessMode::eReadWrite);

    EXPECT_EQ(base.GetContents(changed).status, RDGContentStatus::eUnknown);

    EXPECT_TRUE(base.GetBufferState(added).usage.HasFlag(RHIBufferUsageFlagBits::eTransferDstBuffer));

    EXPECT_EQ(base.GetContents(added).status, RDGContentStatus::eDefined);

    device->DestroyTexture(untouched);

    device->DestroyTexture(removed);

    device->DestroyBuffer(changed);

    device->DestroyBuffer(added);
}

TEST_F(RenderCoreTest, AccelerationStructureStateInvalidationRollsBackWithItsOverlay)
{
    RHIAccelerationStructureCreateInfo info;
    info.size = 4096;
    RHIResourcePtr<RHIAccelerationStructure> structure =
        RHIResourcePtr<RHIAccelerationStructure>::Adopt(rhi->CreateAccelerationStructure(info));
    ResourceStateTracker base;
    base.UpdateAccelerationStructureState(
        structure.Get(), RHIAccessMode::eReadWrite,
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eAccelerationStructureBuild));
    RDGResourceContent contents;
    contents.status = RDGContentStatus::eDefined;
    base.SetContents(structure.Get(), contents);
    const uint64_t revision = base.GetRevision();
    {
        ResourceStateTracker overlay(nullptr, &base);
        EXPECT_TRUE(overlay.GetAccelerationStructureState(structure.Get()).writer.hasWriter);
        EXPECT_EQ(overlay.GetContents(structure.Get()).status, RDGContentStatus::eDefined);
        overlay.RemoveResourceState(structure->GetStableId());
        EXPECT_EQ(overlay.GetAccelerationStructureState(structure.Get()).accessMode, RHIAccessMode::eNone);
        EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(overlay, structure->GetStableId()));
        overlay.UpdateAccelerationStructureState(structure.Get(), RHIAccessMode::eRead,
                                                 BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eComputeShader));
        EXPECT_EQ(overlay.GetContents(structure.Get()).status, RDGContentStatus::eUnknown);
        EXPECT_FALSE(overlay.GetAccelerationStructureState(structure.Get()).writer.hasWriter);
    }
    EXPECT_EQ(base.GetRevision(), revision);
    EXPECT_EQ(base.GetAccelerationStructureState(structure.Get()).accessMode, RHIAccessMode::eReadWrite);
    EXPECT_TRUE(base.GetAccelerationStructureState(structure.Get())
                    .writer.access.HasFlag(RHIAccessFlagBits::eAccelerationStructureWrite));
    EXPECT_EQ(base.GetContents(structure.Get()).status, RDGContentStatus::eDefined);
}

TEST_F(RenderCoreTest, AccelerationStructureStateCommitAndCopyPreserveBufferState)
{
    RHIAccelerationStructureCreateInfo info;
    info.size = 4096;
    RHIResourcePtr<RHIAccelerationStructure> changed =
        RHIResourcePtr<RHIAccelerationStructure>::Adopt(rhi->CreateAccelerationStructure(info));
    RHIResourcePtr<RHIAccelerationStructure> removed =
        RHIResourcePtr<RHIAccelerationStructure>::Adopt(rhi->CreateAccelerationStructure(info));
    TestBuffer*          buffer = Buffer();
    ResourceStateTracker base;
    base.UpdateBufferState(buffer, RHIAccessMode::eRead,
                           BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eVertexBuffer),
                           BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eVertexInput));
    base.UpdateAccelerationStructureState(
        removed.Get(), RHIAccessMode::eReadWrite,
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eAccelerationStructureBuild));
    const uint64_t       revision = base.GetRevision();
    ResourceStateTracker overlay(nullptr, &base);
    overlay.RemoveResourceState(removed->GetStableId());
    overlay.UpdateAccelerationStructureState(changed.Get(), RHIAccessMode::eRead,
                                             BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eComputeShader));
    base.CommitOverlay(std::move(overlay));
    EXPECT_EQ(base.GetRevision(), revision + 1);
    EXPECT_EQ(base.GetAccelerationStructureState(removed.Get()).accessMode, RHIAccessMode::eNone);
    EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(base, removed->GetStableId()));
    EXPECT_EQ(base.GetAccelerationStructureState(changed.Get()).accessMode, RHIAccessMode::eRead);
    EXPECT_TRUE(
        base.GetAccelerationStructureState(changed.Get()).pipelineStages.HasFlag(RHIPipelineStageFlagBits::eComputeShader));
    EXPECT_TRUE(base.GetBufferState(buffer).usage.HasFlag(RHIBufferUsageFlagBits::eVertexBuffer));
    ResourceStateTracker copied(base);
    ResourceStateTracker assigned;
    assigned = copied;
    EXPECT_EQ(assigned.GetAccelerationStructureState(changed.Get()).accessMode, RHIAccessMode::eRead);
    EXPECT_TRUE(assigned.GetBufferState(buffer).usage.HasFlag(RHIBufferUsageFlagBits::eVertexBuffer));
    assigned.RemoveResourceState(changed->GetStableId());
    EXPECT_EQ(copied.GetAccelerationStructureState(changed.Get()).accessMode, RHIAccessMode::eRead);
    device->DestroyBuffer(buffer);
}
