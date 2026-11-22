TEST_P(EditorRendering, SpatialQualificationFailurePreservesActiveSceneAndAllowsRetry)
{
    EditorRenderingScope      scope(GetParam());
    EditorRenderingValidation validation;
    ASSERT_TRUE(scope.editor.Init());
    ASSERT_TRUE(scope.editor.ResizeViewport(160, 120));
    ASSERT_TRUE(scope.editor.Load(Fixture("inspection.gltf")));
    ASSERT_NO_FATAL_FAILURE(ConfigureAssetRendering(scope.editor));
    ASSERT_TRUE(Frame(*scope.device, scope.editor));
    const rc::RenderScene* active     = scope.editor.GetViewport().GetRenderScene();
    const uint64_t         generation = scope.editor.GetScene().GetGeneration();
    const NodeId           selection{generation, 1};
    scope.editor.GetSelection().SelectNode(selection);
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / ("zen_spatial_qualification_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directory(directory, error)) << error.message();
    const std::filesystem::path package = directory / "spatial.zenasset";
    ASSERT_NO_FATAL_FAILURE(CookPageRenderingPackage(package, false, true, true, true));
    EXPECT_EQ(scope.device->GetTextureSamplingParameters().y, 0);
    test::SpatialQualificationPipelineFailure::calls = 0;
    {
        ScopedLogCapture                                     log;
        test::ScopedVulkanCall<PFN_vkCreateComputePipelines> failure(vkCreateComputePipelines,
                                                                     test::SpatialQualificationPipelineFailure::Create);
        EXPECT_FALSE(scope.editor.Load(package.string()));
        EXPECT_NE(log.GetText().find("Spatial texture sampler qualification failed"), std::string::npos);
        EXPECT_EQ(test::SpatialQualificationPipelineFailure::calls, 1u);
    }
    EXPECT_FALSE(scope.editor.GetError().empty());
    EXPECT_EQ(scope.device->GetTextureSamplingParameters().y, 0);
    EXPECT_FALSE(scope.device->AreSubmissionsBlocked());
    EXPECT_EQ(scope.editor.GetViewport().GetRenderScene(), active);
    EXPECT_EQ(scope.editor.GetScene().GetGeneration(), generation);
    EXPECT_EQ(scope.editor.GetSelection().GetNode(), selection);
    EXPECT_TRUE(Frame(*scope.device, scope.editor));
    ASSERT_TRUE(scope.editor.Load(package.string())) << scope.editor.GetError();
    EXPECT_NE(scope.device->GetTextureSamplingParameters().y, 0);
    EXPECT_NE(scope.editor.GetScene().GetGeneration(), generation);
    EXPECT_NE(scope.editor.GetViewport().GetRenderScene(), active);
    EXPECT_TRUE(Frame(*scope.device, scope.editor));
    std::filesystem::remove_all(directory, error);
    EXPECT_FALSE(error) << error.message();
}
