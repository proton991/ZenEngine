#include "Editor/Model/EditorRenderingState.h"
#include <gtest/gtest.h>
#include <limits>

namespace zen::editor
{
TEST(RenderingSettings, LightBallHasFiniteLocalRangeAndCanHoldItsPosition)
{
    rc::CameraLightSettings ball;

    EXPECT_TRUE(rc::ValidateCameraLight(ball));

    EXPECT_LT(ball.range, 0.2f);

    ball.enabled = true;

    EXPECT_EQ(rc::CameraLightShadowFaces(ball), 6u);

    const Vec3 eye(1, 2, 3);

    const Vec3 forward(0, 0, -1);

    EXPECT_EQ(rc::CameraLightPosition(ball, eye, forward), eye + forward * ball.followDistance);

    ball.position     = rc::CameraLightPosition(ball, eye, forward);

    ball.followCamera = false;

    EXPECT_EQ(rc::CameraLightPosition(ball, Vec3(10), Vec3(1, 0, 0)), ball.position);

    ball.range = 0;

    EXPECT_FALSE(rc::ValidateCameraLight(ball));

    ball.range  = 0.1f;

    ball.radius = ball.range;

    EXPECT_FALSE(rc::ValidateCameraLight(ball));

    ball.radius     = 0.005f;

    ball.position.x = std::numeric_limits<float>::infinity();

    EXPECT_FALSE(rc::ValidateCameraLight(ball));

    ball.enabled = false;

    EXPECT_EQ(rc::CameraLightShadowFaces(ball), 0u);
}

TEST(RenderingSettings, InvalidDraftPreservesAppliedRevisionAndCanBeReverted)
{
    EditorRenderingState state;

    state.Initialize({});

    const uint64_t applied      = state.GetAppliedRevision();

    rc::RenderingSettings draft = state.GetDraft();

    draft.environment.intensity = std::numeric_limits<float>::quiet_NaN();

    EXPECT_FALSE(state.Stage(draft));

    EXPECT_TRUE(state.IsPending());

    EXPECT_EQ(state.GetAppliedRevision(), applied);

    EXPECT_EQ(state.GetApplied().environment.intensity, 1.0f);

    EXPECT_FALSE(state.GetError().empty());

    state.Commit();

    EXPECT_EQ(state.GetAppliedRevision(), applied);

    state.Revert();

    EXPECT_FALSE(state.IsPending());

    EXPECT_EQ(state.GetDraft().environment.intensity, 1.0f);
}

TEST(RenderingSettings, ResourcesRequireApplyButScalarsDoNot)
{
    EditorRenderingState state;

    state.Initialize({});

    rc::RenderingSettings draft     = state.GetDraft();

    draft.gi.cone.indirectIntensity = 2.0f;

    ASSERT_TRUE(state.Stage(draft));

    EXPECT_FALSE(state.NeedsResourceApply());

    draft.gi.resolution = 64;

    ASSERT_TRUE(state.Stage(draft));

    EXPECT_TRUE(state.NeedsResourceApply());

    const uint64_t previousApplied = state.GetAppliedRevision();

    state.CommitEnvironmentTexture("candidate.hdr");

    EXPECT_GT(state.GetAppliedRevision(), previousApplied);

    EXPECT_TRUE(state.IsPending());

    EXPECT_EQ(state.GetApplied().gi.resolution, 256u);

    EXPECT_EQ(state.GetDraft().environment.texturePath, "candidate.hdr");

    state.Commit();

    EXPECT_FALSE(state.IsPending());

    EXPECT_EQ(state.GetApplied().gi.resolution, 64u);
}

TEST(RenderingSettings, PreviewPublishesEditsWhileResourceChangesWaitForApply)
{
    EditorRenderingState state;

    rc::RenderingSettings initial;

    initial.gi.resolution = 64;

    state.Initialize(initial);

    rc::RenderingSettings draft     = state.GetDraft();

    draft.gi.resolution             = 128;

    draft.gi.shadowMapResolution    = 512;

    draft.gi.averagedReflectance    = true;

    draft.gi.reflectanceBudgetBytes = 64ull * 1024 * 1024;

    draft.gi.cone.indirectIntensity = 3.0f;

    draft.environment.intensity     = 2.0f;

    draft.debug.output              = rc::DebugOutput::eVoxelSlice;

    draft.debug.slice               = 100;

    ASSERT_TRUE(state.Stage(draft));

    ASSERT_TRUE(state.NeedsResourceApply());

    const rc::RenderingSettings preview = state.GetPreview();

    std::string error;

    EXPECT_TRUE(rc::ValidateRenderingSettings(preview, error)) << error;

    EXPECT_FALSE(rc::RequiresRenderingResourceApply(state.GetApplied(), preview));

    EXPECT_EQ(preview.gi.resolution, 64u);

    EXPECT_EQ(preview.gi.shadowMapResolution, 1024u);

    EXPECT_FALSE(preview.gi.averagedReflectance);

    EXPECT_EQ(preview.gi.reflectanceBudgetBytes, 0u);

    EXPECT_EQ(preview.debug.slice, 63u);

    EXPECT_FLOAT_EQ(preview.gi.cone.indirectIntensity, 3.0f);

    EXPECT_FLOAT_EQ(preview.environment.intensity, 2.0f);

    const uint64_t applied = state.GetAppliedRevision();

    state.CommitPreview();

    EXPECT_GT(state.GetAppliedRevision(), applied);

    EXPECT_FLOAT_EQ(state.GetApplied().environment.intensity, 2.0f);

    EXPECT_EQ(state.GetApplied().gi.resolution, 64u);

    EXPECT_TRUE(state.IsPending());

    EXPECT_TRUE(state.NeedsResourceApply());

    // Revert discards only what has not been published.
    state.Revert();

    EXPECT_FALSE(state.IsPending());

    EXPECT_EQ(state.GetDraft().gi.resolution, 64u);

    EXPECT_FLOAT_EQ(state.GetDraft().environment.intensity, 2.0f);

    // Without averaged reflectance the budget is not a resource, so it previews.
    draft                           = state.GetDraft();

    draft.gi.resolution             = 128;

    draft.gi.reflectanceBudgetBytes = 32ull * 1024 * 1024;

    ASSERT_TRUE(state.Stage(draft));

    EXPECT_EQ(state.GetPreview().gi.reflectanceBudgetBytes, draft.gi.reflectanceBudgetBytes);

    state.Commit();

    EXPECT_EQ(state.GetApplied().gi.resolution, 128u);
}

TEST(RenderingSettings, SmallerVoxelGridClampsTheSelectedSliceBeforePreviewAndApply)
{
    EditorRenderingState state;

    rc::RenderingSettings initial;

    initial.gi.resolution = 128;

    initial.debug.output  = rc::DebugOutput::eVoxelSlice;

    initial.debug.slice   = 100;

    state.Initialize(initial);

    rc::RenderingSettings draft = state.GetDraft();

    draft.gi.resolution         = 64;

    ASSERT_TRUE(state.Stage(draft)) << state.GetError();

    EXPECT_EQ(state.GetDraft().debug.slice, 63u);

    EXPECT_TRUE(state.NeedsResourceApply());

    state.CommitPreview();

    EXPECT_EQ(state.GetApplied().gi.resolution, 128u);

    EXPECT_EQ(state.GetApplied().debug.slice, 63u);

    EXPECT_TRUE(state.NeedsResourceApply());

    ASSERT_TRUE(state.Stage(state.GetDraft()));

    state.Commit();

    EXPECT_EQ(state.GetApplied().gi.resolution, 64u);

    EXPECT_EQ(state.GetApplied().debug.slice, 63u);

    EXPECT_FALSE(state.IsPending());

    draft             = state.GetDraft();

    draft.debug.slice = 64;

    // Invalid slice input without a grid change still reports a validation error.
    EXPECT_FALSE(state.Stage(draft));

    state.Revert();

    draft               = state.GetDraft();

    draft.gi.resolution = 0;

    EXPECT_FALSE(state.Stage(draft));

    EXPECT_EQ(state.GetDraft().debug.slice, 63u);
}

TEST(RenderingSettings, LightReplacementRetainsIdentityAndMonotonicRevision)
{
    rc::SceneLights lights;

    HeapVector<rc::LightEntry> entries;

    for (const rc::SceneLight& light : rc::BuildBoundsLightPreset(sg::AABB(Vec3(-0.5f), Vec3(0.5f))))
    {
        entries.push_back({lights.Add(light), light});
    }

    const uint64_t previousRevision = lights.GetRevision();

    const rc::LightId first         = entries.front().id;

    entries.front().light.intensity = 0.0f;

    ASSERT_TRUE(lights.Replace(entries));

    EXPECT_GT(lights.GetRevision(), previousRevision);

    EXPECT_EQ(entries.front().id, first);

    entries.clear();

    ASSERT_TRUE(lights.Replace(entries));

    EXPECT_TRUE(lights.GetEntries().empty());

    entries.push_back({0, rc::SceneLight()});

    ASSERT_TRUE(lights.Replace(entries));

    EXPECT_GT(entries.front().id, 6u);

    const uint64_t revision = lights.GetRevision();

    entries.push_back(entries.front());

    EXPECT_FALSE(lights.Replace(entries));

    EXPECT_EQ(lights.GetRevision(), revision);
}

TEST(RenderingSettings, PresetsAndShadowPreflight)
{
    const sg::AABB bounds(Vec3(-2, -1, -3), Vec3(4, 5, 6));

    const HeapVector<rc::SceneLight> lights = rc::BuildBoundsLightPreset(bounds, true);

    ASSERT_EQ(lights.size(), 8u);

    for (const rc::SceneLight& light : lights)
    {
        EXPECT_TRUE(rc::SceneLights::Validate(light));

        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            EXPECT_TRUE(light.position[axis] < bounds.GetMin()[axis] || light.position[axis] > bounds.GetMax()[axis]);
        }

        EXPECT_NEAR(glm::dot(light.direction, glm::normalize(bounds.GetCenter() - light.position)), 1.0f, 1e-5f);
    }

    EXPECT_EQ(rc::CountShadowFaces(lights), 48u);

    EXPECT_EQ(rc::EstimateShadowBytes(2048, 192), 3ull * 1024 * 1024 * 1024);

    EXPECT_FALSE(rc::ValidateShadowMemory(2048, 192, 2ull * 1024 * 1024 * 1024));

    EXPECT_TRUE(rc::ValidateShadowMemory(128, 48, 64ull * 1024 * 1024));
}

TEST(RenderingSettings, SceneChangeResetsLightsButPreservesPendingGIAndEnvironment)
{
    EditorRenderingState state;

    state.Initialize({});

    rc::RenderingSettings draft = state.GetDraft();

    draft.gi.resolution         = 64;

    ASSERT_TRUE(state.Stage(draft));

    state.CommitEnvironmentTexture("studio.hdr");

    state.ChangeScene("second.gltf", Vec3(2), 0.5f, {});

    EXPECT_EQ(state.GetDraft().scenePath, "second.gltf");

    EXPECT_TRUE(state.GetDraft().lights.empty());

    EXPECT_TRUE(state.NeedsResourceApply());

    EXPECT_EQ(state.GetDraft().environment.texturePath, "studio.hdr");
}
} // namespace zen::editor
