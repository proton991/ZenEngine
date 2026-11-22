TEST(SpatialTextureSampling, RequiresOneConsistentRuleAcrossBothAxes)
{
    Vec4 observations[] = {Vec4(0.5f, 0.75f, 0, 0), Vec4(0.5f, 0.75f, 0, 0), Vec4(0.5f, 0.5f, 0, 0),
                           Vec4(0.25f, 0.5f, 0, 0), Vec4(0.25f, 0.5f, 0, 0), Vec4(0.25f, 0.5f, 0, 0)};
    EXPECT_EQ(ClassifySpatialTextureSampling(MakeVecView<const Vec4>(observations, 6)), SpatialTextureRounding::eHalfUp);
    observations[0] = observations[2];
    EXPECT_EQ(ClassifySpatialTextureSampling(MakeVecView<const Vec4>(observations, 6)), SpatialTextureRounding::eTiesToEven);
    observations[3].x = 0.125f;
    EXPECT_EQ(ClassifySpatialTextureSampling(MakeVecView<const Vec4>(observations, 6)), SpatialTextureRounding::eUnqualified);
}

TEST(SpatialTextureSampling, RejectsAmbiguousMalformedAndNonfiniteObservations)
{
    Vec4 observations[] = {Vec4(0.5f), Vec4(0.5f), Vec4(0.5f)};
    EXPECT_EQ(ClassifySpatialTextureSampling({}), SpatialTextureRounding::eUnqualified);
    EXPECT_EQ(ClassifySpatialTextureSampling(MakeVecView<const Vec4>(observations, 2)), SpatialTextureRounding::eUnqualified);
    EXPECT_EQ(ClassifySpatialTextureSampling(MakeVecView<const Vec4>(observations, 3)), SpatialTextureRounding::eUnqualified);
    for (float value : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
    {
        observations[0].x = observations[1].x = value;
        EXPECT_EQ(ClassifySpatialTextureSampling(MakeVecView<const Vec4>(observations, 3)),
                  SpatialTextureRounding::eUnqualified);
    }
}

TEST_F(RenderCoreTest, SpatialQualificationRejectsUnsupportedPrecisionBeforeAllocation)
{
    const uint32_t before = rhi->textureCreations;
    for (uint32_t precision : {0u, 24u})
    {
        rhi->info.subTexelPrecisionBits = precision;
        EXPECT_FALSE(device->QualifySpatialTextureSampling());
        EXPECT_EQ(device->GetTextureSamplingParameters().y, 0);
        EXPECT_EQ(rhi->textureCreations, before);
        EXPECT_EQ(ShaderProgramManager::GetInstance().RequestShaderProgram("SpatialSamplingQualificationSP"), nullptr);
    }
}

TEST_F(RenderCoreTest, SpatialQualificationDoesNotCacheShaderOrResourceFailure)
{
    rhi->info.subTexelPrecisionBits = 8;
    rhi->failShaderCreation         = true;
    EXPECT_FALSE(device->QualifySpatialTextureSampling());
    EXPECT_EQ(ShaderProgramManager::GetInstance().RequestShaderProgram("SpatialSamplingQualificationSP"), nullptr);
    EXPECT_EQ(device->GetTextureSamplingParameters().y, 0);
    const uint32_t before      = rhi->textureCreations;
    rhi->failShaderCreation    = false;
    rhi->failTextureCreationAt = before + 1;
    EXPECT_FALSE(device->QualifySpatialTextureSampling());
    EXPECT_NE(ShaderProgramManager::GetInstance().RequestShaderProgram("SpatialSamplingQualificationSP"), nullptr);
    EXPECT_EQ(device->GetTextureSamplingParameters().y, 0);
    EXPECT_EQ(rhi->textureCreations, before + 1);
    // A failed allocation must allow another qualification attempt.
    rhi->failTextureCreationAt = before + 2;
    EXPECT_FALSE(device->QualifySpatialTextureSampling());
    EXPECT_EQ(rhi->textureCreations, before + 2);
    EXPECT_EQ(device->GetTextureSamplingParameters().y, 0);
}
