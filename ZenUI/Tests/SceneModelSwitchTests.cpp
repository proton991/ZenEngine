#include "SceneRendererDemo.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Platform/ConfigLoader.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace zen
{
struct SceneRendererModelTestAccess
{
    static bool Start(SceneRendererDemo& demo, const std::string& root, const std::string& path)
    {
        glfwHideWindow(demo.m_pWindow->GetHandle());

        const bool started = demo.LoadModel(path, false);

        if (started)
        {
            demo.m_modelState.basePath    = root;

            demo.m_modelState.models      = asset::DiscoverGLTFModels(root);

            demo.m_modelState.currentPath = path;

            demo.m_modelState.revision    = 1;
        }

        return started;
    }

    static Mat4 Projection(const SceneRendererDemo& demo)
    {
        return demo.m_camera->GetProjectionMatrix();
    }

    static rc::RenderScene* Scene(const SceneRendererDemo& demo)
    {
        return demo.m_renderScene.Get();
    }

    static const sg::AABB& Bounds(const SceneRendererDemo& demo)
    {
        return demo.m_scene->GetAABB();
    }

    static rc::LightId EditableLightId(const SceneRendererDemo& demo, uint32_t index)
    {
        return demo.m_editableLightIds[index];
    }

    static void ConfigureLighting(SceneRendererDemo& demo, const platform::ConfigLoader& config)
    {
        demo.PrepareLighting(config);
    }

    static void StepLightAnimation(SceneRendererDemo& demo, float elapsed)
    {
        demo.UpdateDynamicLight(elapsed);
    }
};
} // namespace zen

namespace
{
class ModelFixtures
{
public:
    ModelFixtures()
    {
        const int64_t stamp = std::chrono::steady_clock::now().time_since_epoch().count();

        m_root              = std::filesystem::temp_directory_path() / ("zen_model_switch_" + std::to_string(stamp));

        std::filesystem::create_directories(m_root);

        const float vertices[]{0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1};

        std::ofstream buffer(m_root / "triangle.bin", std::ios::binary);

        buffer.write(reinterpret_cast<const char*>(vertices), sizeof(vertices));

        const std::string geometry =
            R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":72,"uri":"triangle.bin"}],)"
            R"("bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36}],)"
            R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
            R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"}],)"
            R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1}}]}],)";

        Write("plain.gltf", geometry + R"("nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})");

        Write("ortho.gltf",
              geometry
                  + R"("nodes":[{"mesh":0},{"camera":0,"translation":[0.5,0.5,2]}],)"
                    R"("cameras":[{"type":"orthographic","orthographic":{"xmag":1,"ymag":1,"znear":0.1,"zfar":10}}],)"
                    R"("scenes":[{"nodes":[0,1]}],"scene":0})");

        Write("infinite.gltf",
              geometry
                  + R"("nodes":[{"mesh":0},{"camera":0,"translation":[0.5,0.5,2]}],)"
                    R"("cameras":[{"type":"perspective","perspective":{"aspectRatio":1.5,"yfov":0.8,"znear":0.1}}],)"
                    R"("scenes":[{"nodes":[0,1]}],"scene":0})");

        Write("authored-light.gltf",
              geometry
                  + R"("extensionsUsed":["KHR_lights_punctual"],)"
                    R"("extensions":{"KHR_lights_punctual":{"lights":[{"type":"point","intensity":2}]}},)"
                    R"("nodes":[{"mesh":0},{"translation":[0,0,2],"extensions":{"KHR_lights_punctual":{"light":0}}}],)"
                    R"("scenes":[{"nodes":[0,1]}],"scene":0})");

        Write("broken.gltf", "invalid glTF");
    }

    ~ModelFixtures()
    {
        std::error_code error;

        std::filesystem::remove_all(m_root, error);
    }

    std::string Root() const
    {
        return Path("");
    }

    std::string Path(const char* file) const
    {
        const std::u8string value = (m_root / file).lexically_normal().generic_u8string();

        const std::string result(reinterpret_cast<const char*>(value.data()), value.size());

        return result;
    }

private:
    void Write(const char* file, const std::string& text)
    {
        std::ofstream stream(m_root / file, std::ios::binary);

        stream << text;
    }

    std::filesystem::path m_root;
};

class SceneModelSwitchTest : public testing::TestWithParam<zen::RHIExecutionMode>
{};

static bool OutsideBounds(const zen::Vec3& position, const zen::sg::AABB& bounds)
{
    return glm::any(glm::lessThan(position, bounds.GetMin())) || glm::any(glm::greaterThan(position, bounds.GetMax()));
}

TEST_P(SceneModelSwitchTest, QueuedSwitchesResetCamerasAndFailedImportsKeepTheActiveScene)
{
    using namespace zen;

    const ModelFixtures models;

    rc::RenderConfig& config            = rc::RenderConfig::GetInstance();

    const RHIExecutionMode previousMode = config.rhiExecutionMode;

    config.rhiExecutionMode             = GetParam();

    const bool previousRayTracing       = RHIOptions::GetInstance().RayTracingEnabled();

    RHIOptions::GetInstance().SetRayTracingEnabled(false);

    SceneRendererDemo demo({"Model switch integration", false, 160, 120}, sg::CameraType::eFirstPerson);

    const bool started = SceneRendererModelTestAccess::Start(demo, models.Root(), models.Path("ortho.gltf"));

    EXPECT_TRUE(started);

    if (started)
    {
        EXPECT_TRUE(demo.EnableRuntimeUI());

        EXPECT_EQ(demo.GetRuntimeModelState().models.size(), 5u);

        EXPECT_FLOAT_EQ(SceneRendererModelTestAccess::Projection(demo)[3][3], 1.0f);

        EXPECT_TRUE(demo.Run(2, false, 2, {}, true));

        EXPECT_TRUE(demo.RequestRuntimeModel(models.Path("plain.gltf")));

        EXPECT_FALSE(demo.RequestRuntimeModel(models.Path("infinite.gltf")));

        EXPECT_TRUE(demo.Run(2, false, 2, {}, true));

        EXPECT_EQ(demo.GetRuntimeModelState().currentPath, models.Path("plain.gltf"));

        EXPECT_FLOAT_EQ(SceneRendererModelTestAccess::Projection(demo)[3][3], 0.0f);

        EXPECT_LT(SceneRendererModelTestAccess::Projection(demo)[2][2], -1.0f);

        const uint64_t revision = demo.GetRuntimeModelState().revision;

        rc::RenderScene* active = SceneRendererModelTestAccess::Scene(demo);

        EXPECT_TRUE(demo.RequestRuntimeModel(models.Path("broken.gltf")));

        EXPECT_TRUE(demo.Run(2, false, 2, {}, true));

        EXPECT_EQ(SceneRendererModelTestAccess::Scene(demo), active);

        EXPECT_EQ(demo.GetRuntimeModelState().revision, revision);

        EXPECT_FALSE(demo.GetRuntimeModelState().error.empty());

        EXPECT_TRUE(demo.GetRuntimeModelState().pendingPath.empty());

        EXPECT_TRUE(demo.RequestRuntimeModel(models.Path("infinite.gltf")));

        EXPECT_TRUE(demo.Run(2, false, 2, {}, true));

        EXPECT_FLOAT_EQ(SceneRendererModelTestAccess::Projection(demo)[2][2], -1.0f);

        EXPECT_TRUE(demo.RequestRuntimeModel(models.Path("plain.gltf")));

        EXPECT_TRUE(demo.Run(2, false, 3, {}, true));

        EXPECT_LT(SceneRendererModelTestAccess::Projection(demo)[2][2], -1.0f);

        EXPECT_TRUE(demo.GetRuntimeModelState().error.empty());

        EXPECT_EQ(demo.GetRuntimeModelState().revision, revision + 2);

        EXPECT_FALSE(demo.RequestRuntimeModel(models.Path("not-in-catalog.gltf")));
    }

    EXPECT_TRUE(demo.Destroy());

    config.rhiExecutionMode = previousMode;

    RHIOptions::GetInstance().SetRayTracingEnabled(previousRayTracing);
}

TEST_P(SceneModelSwitchTest, PresetLightsExposeTheirActualIdsAndRetainSeedsWhenRemovedAndAdded)
{
    using namespace zen;

    const ModelFixtures models;

    rc::RenderConfig& config            = rc::RenderConfig::GetInstance();

    const RHIExecutionMode previousMode = config.rhiExecutionMode;

    config.rhiExecutionMode             = GetParam();

    const bool previousRayTracing       = RHIOptions::GetInstance().RayTracingEnabled();

    RHIOptions::GetInstance().SetRayTracingEnabled(false);

    SceneRendererDemo demo({"Preset light integration", false, 160, 120}, sg::CameraType::eFirstPerson);

    const bool started = SceneRendererModelTestAccess::Start(demo, models.Root(), models.Path("plain.gltf"));

    EXPECT_TRUE(started);

    if (started)
    {
        std::istringstream defaults("scene_lighting_override=false\ndynamic_light.enabled=false\n");

        const platform::ConfigLoader lightingConfig(defaults);

        SceneRendererModelTestAccess::ConfigureLighting(demo, lightingConfig);

        rc::SceneLights& lights           = SceneRendererModelTestAccess::Scene(demo)->GetLights();

        const sg::AABB& bounds            = SceneRendererModelTestAccess::Bounds(demo);

        ui::RuntimeSceneSettings previous = demo.GetRuntimeSceneSettings();

        EXPECT_EQ(previous.lightCount, 6u);

        EXPECT_TRUE(previous.boundsPresetLights);

        EXPECT_EQ(previous.modelLightCount, 0u);

        EXPECT_EQ(lights.GetEntries().size(), 6u);

        EXPECT_FALSE(previous.animationEnabled);

        for (uint32_t index = 0; index < previous.lightCount; ++index)
        {
            SCOPED_TRACE(index);

            const rc::LightId id        = SceneRendererModelTestAccess::EditableLightId(demo, index);

            const rc::SceneLight* light = lights.Find(id);

            EXPECT_NE(light, nullptr);

            if (light != nullptr)
            {
                EXPECT_EQ(light->position, previous.lights[index].position);

                EXPECT_TRUE(OutsideBounds(light->position, bounds));

                const uint32_t axis = index / 2;

                EXPECT_TRUE((index & 1) != 0 ? light->position[axis] > bounds.GetMax()[axis]
                                             : light->position[axis] < bounds.GetMin()[axis]);
            }
        }

        const rc::LightId firstId      = SceneRendererModelTestAccess::EditableLightId(demo, 0);

        const float firstIntensity     = previous.lights[0].intensity;

        ui::RuntimeSceneSettings next  = previous;

        next.lights[0].intensity      += 2.0f;

        next.lights[0].color           = Vec3(0.2f, 0.4f, 0.8f);

        EXPECT_TRUE(demo.ApplyRuntimeSceneSettings(previous, next));

        EXPECT_EQ(lights.GetEntries().size(), 6u);

        EXPECT_EQ(SceneRendererModelTestAccess::EditableLightId(demo, 0), firstId);

        const rc::SceneLight* edited = lights.Find(firstId);

        EXPECT_NE(edited, nullptr);

        if (edited != nullptr)
        {
            EXPECT_FLOAT_EQ(edited->intensity, firstIntensity + 2.0f);

            EXPECT_EQ(edited->color, next.lights[0].color);
        }

        previous                    = demo.GetRuntimeSceneSettings();

        const rc::LightId removedId = SceneRendererModelTestAccess::EditableLightId(demo, 5);

        const Vec3 removedPosition  = previous.lights[5].position;

        next                        = previous;

        next.lightCount             = 5;

        EXPECT_TRUE(demo.ApplyRuntimeSceneSettings(previous, next));

        EXPECT_EQ(lights.GetEntries().size(), 5u);

        EXPECT_EQ(lights.Find(removedId), nullptr);

        previous = demo.GetRuntimeSceneSettings();

        EXPECT_EQ(previous.lights[5].position, removedPosition);

        next            = previous;

        next.lightCount = 6;

        EXPECT_TRUE(demo.ApplyRuntimeSceneSettings(previous, next));

        EXPECT_EQ(lights.GetEntries().size(), 6u);

        EXPECT_NE(SceneRendererModelTestAccess::EditableLightId(demo, 5), removedId);

        EXPECT_EQ(demo.GetRuntimeSceneSettings().lights[5].position, removedPosition);

        previous = demo.GetRuntimeSceneSettings();

        EXPECT_TRUE(OutsideBounds(previous.lights[6].position, bounds));

        next            = previous;

        next.lightCount = 7;

        EXPECT_TRUE(demo.ApplyRuntimeSceneSettings(previous, next));

        EXPECT_EQ(lights.GetEntries().size(), 7u);

        EXPECT_TRUE(demo.Run(1, false, 2, {}, true));

        EXPECT_TRUE(demo.RequestRuntimeModel(models.Path("broken.gltf")));

        EXPECT_TRUE(demo.Run(1, false, 2, {}, true));

        EXPECT_EQ(demo.GetRuntimeSceneSettings().lightCount, 7u);

        EXPECT_EQ(lights.GetEntries().size(), 7u);

        EXPECT_TRUE(demo.RequestRuntimeModel(models.Path("ortho.gltf")));

        EXPECT_TRUE(demo.Run(1, false, 2, {}, true));

        const ui::RuntimeSceneSettings switched = demo.GetRuntimeSceneSettings();

        EXPECT_EQ(switched.lightCount, 6u);

        EXPECT_TRUE(switched.boundsPresetLights);

        EXPECT_EQ(switched.modelLightCount, 0u);

        EXPECT_FALSE(switched.animationEnabled);

        EXPECT_EQ(SceneRendererModelTestAccess::Scene(demo)->GetLights().GetEntries().size(), 6u);

        EXPECT_FLOAT_EQ(switched.lights[0].intensity, firstIntensity);
    }

    EXPECT_TRUE(demo.Destroy());

    config.rhiExecutionMode = previousMode;

    RHIOptions::GetInstance().SetRayTracingEnabled(previousRayTracing);
}

TEST_P(SceneModelSwitchTest, ConfiguredLightSlotsRetainIdsAndAnimationWhenAnotherLightIsEdited)
{
    using namespace zen;

    const ModelFixtures models;

    rc::RenderConfig& config            = rc::RenderConfig::GetInstance();

    const RHIExecutionMode previousMode = config.rhiExecutionMode;

    config.rhiExecutionMode             = GetParam();

    const bool previousRayTracing       = RHIOptions::GetInstance().RayTracingEnabled();

    RHIOptions::GetInstance().SetRayTracingEnabled(false);

    SceneRendererDemo demo({"Configured light integration", false, 160, 120}, sg::CameraType::eFirstPerson);

    const bool started = SceneRendererModelTestAccess::Start(demo, models.Root(), models.Path("plain.gltf"));

    EXPECT_TRUE(started);

    if (started)
    {
        std::istringstream configured("scene_lighting_override=true\nlight_count=3\nlight.0.position=-2,1,0\n"
                                      "light.1.position=2,1,0\nlight.2.position=1,2,3\n"
                                      "dynamic_light.enabled=true\ndynamic_light.index=2\n"
                                      "dynamic_light.orbit_center=0,1,0\ndynamic_light.orbit_radius=0.5\n"
                                      "dynamic_light.angular_speed_degrees=45\n");

        const platform::ConfigLoader lightingConfig(configured);

        SceneRendererModelTestAccess::ConfigureLighting(demo, lightingConfig);

        rc::SceneLights& lights                 = SceneRendererModelTestAccess::Scene(demo)->GetLights();

        const ui::RuntimeSceneSettings previous = demo.GetRuntimeSceneSettings();

        EXPECT_EQ(previous.lightCount, 3u);

        EXPECT_FALSE(previous.boundsPresetLights);

        EXPECT_EQ(previous.modelLightCount, 0u);

        EXPECT_EQ(lights.GetEntries().size(), 3u);

        EXPECT_TRUE(previous.animationEnabled);

        EXPECT_EQ(previous.animatedLight, 2u);

        EXPECT_EQ(previous.lights[0].position, Vec3(-2, 1, 0));

        EXPECT_EQ(previous.lights[1].position, Vec3(2, 1, 0));

        EXPECT_EQ(previous.lights[2].position, Vec3(1, 2, 3));

        const rc::LightId firstId    = SceneRendererModelTestAccess::EditableLightId(demo, 0);

        const rc::LightId animatedId = SceneRendererModelTestAccess::EditableLightId(demo, 2);

        SceneRendererModelTestAccess::StepLightAnimation(demo, 0.25f);

        const Vec3 animatedPosition = demo.GetRuntimeSceneSettings().lights[2].position;

        EXPECT_NE(animatedPosition, previous.lights[2].position);

        ui::RuntimeSceneSettings next  = previous;

        next.lights[0].intensity      += 1.0f;

        EXPECT_TRUE(demo.ApplyRuntimeSceneSettings(previous, next));

        EXPECT_EQ(lights.GetEntries().size(), 3u);

        EXPECT_EQ(SceneRendererModelTestAccess::EditableLightId(demo, 0), firstId);

        EXPECT_EQ(SceneRendererModelTestAccess::EditableLightId(demo, 2), animatedId);

        const ui::RuntimeSceneSettings current = demo.GetRuntimeSceneSettings();

        EXPECT_EQ(current.lights[2].position, animatedPosition);

        EXPECT_FLOAT_EQ(current.lights[0].intensity, previous.lights[0].intensity + 1.0f);

        EXPECT_TRUE(current.animationEnabled);

        EXPECT_EQ(current.animatedLight, 2u);

        EXPECT_TRUE(demo.Run(1, false, 2, {}, true));
    }

    EXPECT_TRUE(demo.Destroy());

    config.rhiExecutionMode = previousMode;

    RHIOptions::GetInstance().SetRayTracingEnabled(previousRayTracing);
}

TEST_P(SceneModelSwitchTest, AuthoredModelLightIdsRemainOwnedByTheModelAndReduceEditableCapacity)
{
    using namespace zen;

    const ModelFixtures models;

    rc::RenderConfig& config            = rc::RenderConfig::GetInstance();

    const RHIExecutionMode previousMode = config.rhiExecutionMode;

    config.rhiExecutionMode             = GetParam();

    const bool previousRayTracing       = RHIOptions::GetInstance().RayTracingEnabled();

    RHIOptions::GetInstance().SetRayTracingEnabled(false);

    SceneRendererDemo demo({"Authored light integration", false, 160, 120}, sg::CameraType::eFirstPerson);

    const bool started = SceneRendererModelTestAccess::Start(demo, models.Root(), models.Path("authored-light.gltf"));

    EXPECT_TRUE(started);

    if (started)
    {
        rc::SceneLights& lights                 = SceneRendererModelTestAccess::Scene(demo)->GetLights();

        const ui::RuntimeSceneSettings previous = demo.GetRuntimeSceneSettings();

        EXPECT_EQ(previous.lightCount, 0u);

        EXPECT_FALSE(previous.boundsPresetLights);

        EXPECT_EQ(previous.modelLightCount, 1u);

        EXPECT_EQ(lights.GetEntries().size(), 1u);

        if (!lights.GetEntries().empty())
        {
            const rc::LightId authoredId  = lights.GetEntries()[0].id;

            const Vec3 authoredPosition   = lights.GetEntries()[0].light.position;

            ui::RuntimeSceneSettings next = previous;

            next.lightCount               = rc::MaxSceneLights;

            // Actual ownership still limits capacity if a stale draft omits metadata.
            next.modelLightCount = 0;

            EXPECT_FALSE(demo.ApplyRuntimeSceneSettings(previous, next));

            EXPECT_EQ(lights.GetEntries().size(), 1u);

            EXPECT_EQ(demo.GetRuntimeSceneSettings().lightCount, 0u);

            next            = previous;

            next.lightCount = 1;

            EXPECT_TRUE(demo.ApplyRuntimeSceneSettings(previous, next));

            EXPECT_EQ(lights.GetEntries().size(), 2u);

            const rc::SceneLight* authored = lights.Find(authoredId);

            EXPECT_NE(authored, nullptr);

            if (authored != nullptr)
            {
                EXPECT_EQ(authored->position, authoredPosition);
            }

            const ui::RuntimeSceneSettings current = demo.GetRuntimeSceneSettings();

            next                                   = current;

            next.lightCount                        = 0;

            EXPECT_TRUE(demo.ApplyRuntimeSceneSettings(current, next));

            EXPECT_EQ(lights.GetEntries().size(), 1u);

            EXPECT_NE(lights.Find(authoredId), nullptr);

            EXPECT_TRUE(demo.Run(1, false, 2, {}, true));
        }
    }

    EXPECT_TRUE(demo.Destroy());

    config.rhiExecutionMode = previousMode;

    RHIOptions::GetInstance().SetRayTracingEnabled(previousRayTracing);
}

INSTANTIATE_TEST_SUITE_P(SubmissionModes,
                         SceneModelSwitchTest,
                         testing::Values(zen::RHIExecutionMode::eInline, zen::RHIExecutionMode::eThreaded));
} // namespace
