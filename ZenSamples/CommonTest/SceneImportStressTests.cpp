#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "AssetLib/FastGLTFLoader.h"
#include "Graphics/RenderCore/V2/SceneLighting.h"
#include "SceneGraph/Scene.h"
#include "SceneGraph/SceneAnimation.h"
#include "Systems/SceneEditor.h"

using namespace zen;

namespace
{
class StressSceneFile
{
public:
    explicit StressSceneFile(const std::string& document)
    {
        const int64_t stamp = std::chrono::steady_clock::now().time_since_epoch().count();

        m_path              = std::filesystem::temp_directory_path() / ("zen_scene_stress_" + std::to_string(stamp) + ".gltf");

        std::ofstream file(m_path, std::ios::binary);

        file.write(document.data(), static_cast<std::streamsize>(document.size()));

        if (!file)
        {
            throw std::runtime_error("Cannot write the temporary glTF stress fixture");
        }
    }

    ~StressSceneFile()
    {
        std::error_code error;

        std::filesystem::remove(m_path, error);
    }

    std::string GetPath() const
    {
        const std::string result = m_path.string();

        return result;
    }

private:
    std::filesystem::path m_path;
};

void ExpectFiniteStressMatrix(const Mat4& matrix)
{
    for (uint32_t column = 0; column < 4; ++column)
    {
        for (uint32_t row = 0; row < 4; ++row)
        {
            EXPECT_TRUE(std::isfinite(matrix[column][row]));
        }
    }
}
} // namespace

TEST(SceneImportStress, EmptySceneHasFiniteBoundsAndCanNormalizeAndDeform)
{
    const StressSceneFile file(R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[]}],"scene":0})");

    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_NO_THROW(loader.LoadFromFile(file.GetPath(), &scene));

    EXPECT_TRUE(scene.GetNodes().empty());

    EXPECT_EQ(scene.GetRenderableCount(), 0u);

    EXPECT_TRUE(loader.GetVertices().empty());

    EXPECT_TRUE(loader.GetIndices().empty());

    EXPECT_EQ(scene.GetAABB().GetMin(), Vec3(0));

    EXPECT_EQ(scene.GetAABB().GetMax(), Vec3(0));

    sys::SceneEditor::CenterAndNormalizeScene(&scene);

    HeapVector<asset::Vertex> deformed;

    EXPECT_TRUE(sg::ApplySceneDeformations(scene, VectorView<const asset::Vertex>(loader.GetVertices()), deformed));

    EXPECT_TRUE(deformed.empty());

    EXPECT_EQ(scene.GetAABB().GetMin(), Vec3(0));

    EXPECT_EQ(scene.GetAABB().GetMax(), Vec3(0));
}

TEST(SceneImportStress, ImportsHierarchyDeeperThanTenThousandWithoutRecursiveStackTraversal)
{
    constexpr uint32_t nodeCount = 10001;

    std::ostringstream document;

    document << R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"scene":0,"nodes":[)";

    for (uint32_t node = 0; node < nodeCount; ++node)
    {
        if (node != 0)
        {
            document << ',';
        }

        document << R"({"translation":[1,0,0])";

        if (node + 1 < nodeCount)
        {
            document << ",\"children\":[" << node + 1 << ']';
        }

        document << '}';
    }

    document << "]}";

    const StressSceneFile file(document.str());

    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_NO_THROW(loader.LoadFromFile(file.GetPath(), &scene));

    ASSERT_EQ(scene.GetNodes().size(), nodeCount);

    sg::Node* leaf = scene.GetNodes().back().Get();

    ASSERT_TRUE(leaf->HasComponent<sg::Transform>());

    const Mat4 world = leaf->GetComponent<sg::Transform>()->GetWorldMatrix();

    ExpectFiniteStressMatrix(world);

    EXPECT_EQ(Vec3(world[3]), Vec3(nodeCount, 0, 0));

    EXPECT_EQ(leaf->GetData().modelMatrix, world);

    EXPECT_TRUE(leaf->IsVisible());

    scene.GetNodes().front()->visible = false;

    EXPECT_FALSE(leaf->IsVisible());

    scene.GetNodes().front()->visible = true;

    EXPECT_TRUE(leaf->IsVisible());
}

TEST(SceneImportStress, ZeroScaleAndSingularMatrixKeepNormalTransformsFinite)
{
    const StressSceneFile file(
        R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0,1]}],"scene":0,"nodes":[{"scale":[0,0,0]},{"matrix":[0,0,0,0,0,1,0,0,0,0,1,0,3,4,5,1]}]})");

    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_NO_THROW(loader.LoadFromFile(file.GetPath(), &scene));

    ASSERT_EQ(scene.GetNodes().size(), 2u);

    for (const UniquePtr<sg::Node>& node : scene.GetNodes())
    {
        ExpectFiniteStressMatrix(node->GetData().modelMatrix);

        ExpectFiniteStressMatrix(node->GetData().normalMatrix);

        EXPECT_EQ(node->GetData().normalMatrix, Mat4(1));
    }

    sg::Node singular(0, "zero_matrix");

    singular.SetData(0, Mat4(0));

    ExpectFiniteStressMatrix(singular.GetData().normalMatrix);

    EXPECT_EQ(singular.GetData().normalMatrix, Mat4(1));
}

TEST(SceneImportStress, LightVisibilityUsesOwnersAndAncestorsWithoutAddingFallbackLights)
{
    sg::Scene scene;

    UniquePtr<sg::Node> parent      = MakeUnique<sg::Node>(0, "LightParent");

    UniquePtr<sg::Node> child       = MakeUnique<sg::Node>(1, "InheritedLight");

    UniquePtr<sg::Node> independent = MakeUnique<sg::Node>(2, "IndependentLight");

    child->SetParent(parent.Get());

    parent->AddChild(child.Get());

    sg::LightProperties inheritedProperties;

    inheritedProperties.position   = Vec3(1, 0, 0);

    inheritedProperties.intensity  = 7.0f;

    UniquePtr<sg::Light> inherited = sg::Light::CreatePointLight("Inherited", inheritedProperties);

    sg::LightProperties independentProperties;

    independentProperties.position  = Vec3(2, 0, 0);

    independentProperties.intensity = 13.0f;

    UniquePtr<sg::Light> other      = sg::Light::CreatePointLight("Independent", independentProperties);

    child->AddComponent(inherited.Get());

    independent->AddComponent(other.Get());

    scene.AddComponent(std::move(inherited));

    scene.AddComponent(std::move(other));

    zen::HeapVector<UniquePtr<sg::Node>> nodes;

    nodes.push_back(std::move(parent));

    nodes.push_back(std::move(child));

    nodes.push_back(std::move(independent));

    scene.SetNodes(std::move(nodes));

    HeapVector<rc::SceneLight> lights = rc::BuildSceneLights(scene);

    ASSERT_EQ(lights.size(), 2u);

    EXPECT_TRUE(lights[0].enabled);

    EXPECT_TRUE(lights[1].enabled);

    scene.GetNodes()[0]->visible = false;

    lights                       = rc::BuildSceneLights(scene);

    ASSERT_EQ(lights.size(), 2u);

    EXPECT_FALSE(lights[0].enabled);

    EXPECT_TRUE(lights[1].enabled);

    EXPECT_FLOAT_EQ(lights[0].intensity, 7.0f);

    EXPECT_FLOAT_EQ(lights[1].intensity, 13.0f);

    scene.GetNodes()[0]->visible = true;

    scene.GetNodes()[1]->visible = false;

    scene.GetNodes()[2]->visible = false;

    lights                       = rc::BuildSceneLights(scene);

    ASSERT_EQ(lights.size(), 2u);

    EXPECT_FALSE(lights[0].enabled);

    EXPECT_FALSE(lights[1].enabled);

    rc::SceneLights published;

    for (const rc::SceneLight& light : lights)
    {
        ASSERT_NE(published.Add(light), 0u);
    }

    rc::SceneUniformData uniforms;

    published.WriteUniforms(uniforms);

    EXPECT_FLOAT_EQ(uniforms.lightInfo.x, 0.0f);

    EXPECT_EQ(uniforms.lights[0].colorIntensity, Vec4(0));

    scene.GetNodes()[1]->visible = true;

    lights                       = rc::BuildSceneLights(scene);

    ASSERT_EQ(lights.size(), 2u);

    EXPECT_TRUE(lights[0].enabled);

    EXPECT_FALSE(lights[1].enabled);
}
