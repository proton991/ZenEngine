TEST_F(VulkanBindlessRetirementIntegrationTest, SceneHeapResetRequiresDrainedRecordingsAndPublishesReplacementPixels)
{
    RHIPipeline* pipeline    = Compute("binding_bindless.comp.spv");

    VulkanTexture* first     = Texture();

    RHISampler* firstSampler = Sampler();

    VulkanBuffer* output     = Buffer();

    Initialize(first);

    SubmitAndWait(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

    const RHIBindlessHandle image   = session->rhi.RegisterBindlessResource(first->GetDefaultView(), 0);

    const RHIBindlessHandle sampler = session->rhi.RegisterBindlessResource(firstSampler, 0);

    ASSERT_TRUE(image.IsValid());

    ASSERT_TRUE(sampler.IsValid());

    const VkDescriptorSet set = session->rhi.GetBindlessDescriptorPoolManager()->GetGlobalBindlessSet();

    const uint64_t recording  = context->RHICaptureBindlessEpoch();

    EXPECT_FALSE(session->rhi.ResetBindlessResources());

    EXPECT_TRUE(session->rhi.IsBindlessResourceRegistered(image));

    EXPECT_EQ(first->GetRefCount(), 2u);

    context->RHIReleaseBindlessEpoch(recording);

    SetOutput(pipeline, output, 2, 0);

    PushIndex(pipeline, 0);

    context->RHIDispatch(1, 1, 1);

    // Native work that has been recorded but not submitted also prevents heap replacement.
    EXPECT_FALSE(session->rhi.ResetBindlessResources());

    EXPECT_TRUE(session->rhi.IsBindlessResourceRegistered(sampler));

    SubmitAndWait();

    CheckPixel(output, 0xFF0000FFu);

    ASSERT_TRUE(session->rhi.ResetBindlessResources());

    EXPECT_EQ(session->rhi.GetBindlessDescriptorPoolManager()->GetGlobalBindlessSet(), set);

    EXPECT_FALSE(session->rhi.IsBindlessResourceRegistered(image));

    EXPECT_FALSE(session->rhi.IsBindlessResourceRegistered(sampler));

    EXPECT_EQ(first->GetRefCount(), 1u);

    EXPECT_EQ(first->GetDefaultView()->GetRefCount(), 1u);

    EXPECT_EQ(firstSampler->GetRefCount(), 1u);

    session->rhi.DestroyTexture(first);

    textures.back() = nullptr;

    session->rhi.DestroySampler(firstSampler);

    samplers.back()                     = nullptr;

    VulkanTexture* replacement          = Texture();

    RHISampler* replacementSampler      = Sampler();

    const RHIBindlessHandle nextImage   = session->rhi.RegisterBindlessResource(replacement->GetDefaultView(), 0);

    const RHIBindlessHandle nextSampler = session->rhi.RegisterBindlessResource(replacementSampler, 0);

    ASSERT_TRUE(nextImage.IsValid());

    ASSERT_TRUE(nextSampler.IsValid());

    EXPECT_NE(nextImage.generation, image.generation);

    EXPECT_NE(nextSampler.generation, sampler.generation);

    EXPECT_FALSE(session->rhi.UnregisterBindlessResource(image));

    EXPECT_TRUE(session->rhi.IsBindlessResourceRegistered(nextImage));

    Initialize(replacement, true);

    SetOutput(pipeline, output, 2, 0);

    PushIndex(pipeline, 0);

    context->RHIDispatch(1, 1, 1);

    SubmitAndWait();

    CheckPixel(output, 0xFF00FF00u);

    ASSERT_TRUE(session->rhi.ResetBindlessResources());

    // Unflushed writes must not keep freed scene objects alive or touch them after a reset.
    VulkanTexture* abandoned = Texture();

    ASSERT_TRUE(session->rhi.RegisterBindlessResource(abandoned->GetDefaultView(), 0).IsValid());

    ASSERT_TRUE(session->rhi.ResetBindlessResources());

    EXPECT_EQ(abandoned->GetRefCount(), 1u);

    session->rhi.DestroyTexture(abandoned);

    textures.back() = nullptr;

    session->rhi.GetBindlessDescriptorPoolManager()->Flush();
}
