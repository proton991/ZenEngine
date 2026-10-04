#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "AssetLib/FastGLTFLoader.h"
#include "SceneGraph/Scene.h"
#include "SceneGraph/Camera.h"
#include "Systems/SceneEditor.h"
#include "Graphics/RenderCore/V2/SceneLighting.h"

using namespace zen;

namespace
{
std::string SceneFixture(const char* name)
{
    return (std::filesystem::path(__FILE__).parent_path() / "Assets" / name).string();
}

void ExpectVectorNear(const Vec3& actual, const Vec3& expected)
{
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        EXPECT_NEAR(actual[axis], expected[axis], 1e-5f);
    }
}
} // namespace

TEST(SceneImport, ImportsOnlyActiveLightInstancesAndCameraNodesInGLTFAndGLB)
{
    for (const char* file : {"complete_scene.gltf", "complete_scene.glb", "matrix_scene.gltf"})
    {
        sg::Scene scene;

        asset::FastGLTFLoader loader;

        ASSERT_TRUE(loader.LoadFromFile(SceneFixture(file), &scene)) << loader.GetError();

        const zen::HeapVector<sg::Light*> lights = scene.GetComponents<sg::Light>();

        ASSERT_EQ(lights.size(), 2u);

        EXPECT_EQ(lights[0]->GetType(), sg::Point);

        ExpectVectorNear(lights[0]->GetProperties().position, Vec3(10, 2, 4));

        EXPECT_FLOAT_EQ(lights[0]->GetProperties().range, 4.0f);

        EXPECT_FLOAT_EQ(lights[0]->GetProperties().intensity, 80.0f);

        ExpectVectorNear(Vec3(lights[1]->GetProperties().direction), Vec3(-1, 0, 0));

        const zen::HeapVector<sg::SceneCamera*> cameras = scene.GetComponents<sg::SceneCamera>();

        ASSERT_EQ(cameras.size(), 2u);

        EXPECT_TRUE(cameras[0]->infiniteFar);

        EXPECT_TRUE(cameras[0]->fixedAspect);

        EXPECT_FLOAT_EQ(cameras[0]->verticalFov, 1.0f);

        ExpectVectorNear(Vec3(cameras[0]->worldMatrix[3]), Vec3(10, 0, 8));

        EXPECT_TRUE(cameras[1]->orthographic);

        EXPECT_FLOAT_EQ(cameras[1]->xmag, 2.0f);

        EXPECT_FLOAT_EQ(cameras[1]->ymag, 3.0f);

        EXPECT_EQ(scene.GetRenderableCount(), 1u);

        EXPECT_EQ(scene.GetNodes().size(), 7u);
    }
}

TEST(SceneImport, NormalizationKeepsMeshesLightsAndCamerasTogether)
{
    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(SceneFixture("complete_scene.gltf"), &scene)) << loader.GetError();

    sys::SceneEditor::CenterAndNormalizeScene(&scene);

    ExpectVectorNear(scene.GetAABB().GetMin(), Vec3(-0.5f, -0.5f, 0));

    ExpectVectorNear(scene.GetAABB().GetMax(), Vec3(0.5f, 0.5f, 0));

    const zen::HeapVector<sg::Light*> lights = scene.GetComponents<sg::Light>();

    ExpectVectorNear(lights[0]->GetProperties().position, Vec3(0, 0.5f, 0.875f));

    EXPECT_FLOAT_EQ(lights[0]->GetProperties().range, 1.0f);

    EXPECT_FLOAT_EQ(lights[0]->GetProperties().intensity, 5.0f);

    EXPECT_FLOAT_EQ(lights[1]->GetProperties().intensity, 2.0f);

    const sg::Node* mesh = scene.GetRenderableNodes().front();

    EXPECT_TRUE(mesh->deformationInWorldSpace);

    // Skinning already applied the joint world matrix; normalization now maps those vertices.
    EXPECT_EQ(mesh->GetData().modelMatrix, glm::scale(Mat4(1), Vec3(0.25f)) * glm::translate(Mat4(1), Vec3(-10, 0, -0.5f)));

    const sg::SceneCamera* camera = scene.GetComponents<sg::SceneCamera>().front();

    ExpectVectorNear(Vec3(camera->worldMatrix[3]), Vec3(0, 0, 1.875f));

    EXPECT_FLOAT_EQ(camera->nearPlane, 0.025f);

    const HeapVector<rc::SceneLight> rendered = rc::BuildSceneLights(scene);

    ASSERT_EQ(rendered.size(), 2u);

    EXPECT_EQ(rendered[0].type, rc::SceneLightType::ePoint);

    EXPECT_EQ(rendered[0].position, lights[0]->GetProperties().position);

    EXPECT_FLOAT_EQ(rendered[0].intensity, 5.0f);
}

TEST(SceneImport, ExplicitBoundsPresetCoversSixFacesAndStaysOutsideWorldGeometryBounds)
{
    for (const sg::AABB& bounds :
         {sg::AABB(Vec3(-3, -1, -7), Vec3(8, 5, 2)), sg::AABB(Vec3(0), Vec3(0)), sg::AABB(Vec3(-2, 0, -2), Vec3(2, 0, 2))})
    {
        sg::Scene scene;

        scene.GetAABB() = bounds;

        EXPECT_TRUE(rc::BuildSceneLights(scene).empty());

        const HeapVector<rc::SceneLight> lights = rc::BuildBoundsLightPreset(bounds);

        ASSERT_EQ(lights.size(), 6u);

        for (uint32_t face = 0; face < 6; ++face)
        {
            const rc::SceneLight& light = lights[face];

            const uint32_t axis         = face / 2;

            EXPECT_TRUE(rc::SceneLights::Validate(light));

            EXPECT_TRUE(glm::any(glm::lessThan(light.position, bounds.GetMin()))
                        || glm::any(glm::greaterThan(light.position, bounds.GetMax())));

            EXPECT_GT(glm::dot(light.direction, bounds.GetCenter() - light.position), 0);

            for (uint32_t component = 0; component < 3; ++component)
            {
                if (component != axis)
                {
                    EXPECT_FLOAT_EQ(light.position[component], bounds.GetCenter()[component]);
                }
            }
        }
    }
}

TEST(SceneImport, UnlimitedSpotRangeAndNinetyDegreeConeAreAccepted)
{
    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(SceneFixture("spot_scene.gltf"), &scene)) << loader.GetError();

    const HeapVector<rc::SceneLight> lights = rc::BuildSceneLights(scene);

    ASSERT_EQ(lights.size(), 1u);

    EXPECT_EQ(lights[0].type, rc::SceneLightType::eSpot);

    EXPECT_FLOAT_EQ(lights[0].range, 0.0f);

    EXPECT_FLOAT_EQ(lights[0].outerAngleDegrees, 90.0f);

    EXPECT_TRUE(rc::SceneLights::Validate(lights[0]));

    rc::SceneLights collection;

    EXPECT_NE(collection.Add(lights[0]), 0u);

    rc::SceneUniformData uniforms;

    collection.WriteUniforms(uniforms);

    EXPECT_FLOAT_EQ(uniforms.lightInfo.x, 1.0f);

    EXPECT_FLOAT_EQ(uniforms.lights[0].positionRange.w, 0.0f);
}

TEST(SceneImport, RetainsRichMaterialBindingsTransformsAndColorSpaces)
{
    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(SceneFixture("complete_scene.gltf"), &scene)) << loader.GetError();

    const sg::Material& material = *scene.GetComponents<sg::Material>().front();

    const sg::Sampler* sampler   = scene.GetComponents<sg::Sampler>().front();

    EXPECT_EQ(sampler->minFilter, sg::TextureFilter::Nearest);

    EXPECT_EQ(sampler->magFilter, sg::TextureFilter::Nearest);

    EXPECT_EQ(sampler->mipFilter, sg::TextureFilter::Linear);

    EXPECT_TRUE(sampler->useMipmaps);

    EXPECT_EQ(sampler->wrapS, sg::SamplerAddressMode::ClampToEdge);

    EXPECT_EQ(sampler->wrapT, sg::SamplerAddressMode::MirroredRepeat);

    EXPECT_FLOAT_EQ(material.data.textureTransforms[0].row0.w, 1.0f);

    EXPECT_TRUE(material.unlit);

    EXPECT_TRUE(material.doubleSided);

    EXPECT_FLOAT_EQ(material.occlusionStrength, 0.3f);

    EXPECT_FLOAT_EQ(material.data.materialProperties.x, 0.3f);

    EXPECT_FLOAT_EQ(material.data.materialProperties.y, 1.0f);

    EXPECT_EQ(material.texCoordSets.baseColor, 1u);

    ExpectVectorNear(Vec3(material.data.textureTransforms[0].row0), Vec3(0, -3, 0.2f));

    ExpectVectorNear(Vec3(material.data.textureTransforms[0].row1), Vec3(2, 0, 0.3f));

    EXPECT_FLOAT_EQ(material.features.ior, 1.8f);

    EXPECT_FLOAT_EQ(material.features.dispersion, 0.05f);

    EXPECT_FLOAT_EQ(material.features.specular, 0.6f);

    EXPECT_FLOAT_EQ(material.features.clearcoat, 0.8f);

    EXPECT_FLOAT_EQ(material.features.clearcoatNormalTexture.scale, 0.4f);

    EXPECT_FLOAT_EQ(material.features.transmission, 0.5f);

    EXPECT_FLOAT_EQ(material.features.thickness, 2.0f);

    EXPECT_FLOAT_EQ(material.features.iridescenceThicknessMax, 500.0f);

    EXPECT_FLOAT_EQ(material.features.anisotropy, 0.9f);

    EXPECT_FLOAT_EQ(material.features.diffuseTransmission, 0.3f);

    ASSERT_NE(material.features.specularColorTexture.texture, nullptr);

    ASSERT_NE(material.features.clearcoatTexture.texture, nullptr);

    EXPECT_EQ(material.features.specularColorTexture.texture->format, asset::Format::R8G8B8A8_SRGB);

    EXPECT_EQ(material.features.clearcoatTexture.texture->format, asset::Format::R8G8B8A8_UNORM);

    EXPECT_NE(material.features.specularColorTexture.texture, material.features.clearcoatTexture.texture);
}

TEST(SceneImport, SceneOwnsSkinAnimationAndMorphPayloadAfterLoaderDestruction)
{
    sg::Scene scene;
    {
        asset::FastGLTFLoader loader;

        ASSERT_TRUE(loader.LoadFromFile(SceneFixture("complete_scene.gltf"), &scene)) << loader.GetError();
    }

    const sg::SceneAssetData& data = scene.GetAssetData();

    ASSERT_EQ(data.skins.size(), 1u);

    EXPECT_EQ(data.skins[0].joints[0], 7u);

    EXPECT_EQ(data.skins[0].inverseBindMatrices[0], Mat4(1.0f));

    EXPECT_EQ(scene.GetRenderableNodes()[0]->skinIndex, 0);

    EXPECT_FLOAT_EQ(scene.GetRenderableNodes()[0]->morphWeights[0], 0.25f);

    ASSERT_EQ(data.animations.size(), 1u);

    EXPECT_EQ(data.animations[0].channels[0].node, 1u);

    EXPECT_EQ(data.animations[0].channels[0].path, sg::AnimationPath::Translation);

    EXPECT_FLOAT_EQ(data.animations[0].samplers[0].times[1], 1.0f);

    EXPECT_FLOAT_EQ(data.animations[0].samplers[0].values[5], 3.0f);

    ASSERT_EQ(data.morphPrimitives.size(), 1u);

    EXPECT_FLOAT_EQ(data.morphPrimitives[0].weights[0], 0.5f);

    EXPECT_EQ(data.morphPrimitives[0].targets[0].positions[1], Vec3(0, 0, 1));
}

TEST(SceneImport, PerspectiveAndOrthographicCamerasPublishAuthoredMatrices)
{
    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(SceneFixture("complete_scene.gltf"), &scene)) << loader.GetError();

    const zen::HeapVector<sg::SceneCamera*> cameras = scene.GetComponents<sg::SceneCamera>();

    UniquePtr<sg::Camera> camera                    = sg::Camera::CreateUnique(Vec3(0, 0, 1), Vec3(0), 1.0f);

    camera->SetupFromSceneCamera(*cameras[0], 2.0f);

    ExpectVectorNear(camera->GetPos(), Vec3(10, 0, 8));

    EXPECT_FLOAT_EQ(camera->GetProjectionMatrix()[2][2], -1.0f);

    EXPECT_FLOAT_EQ(camera->GetProjectionMatrix()[3][2], -0.1f);

    const Mat4 perspective = camera->GetProjectionMatrix();

    camera->UpdateAspect(3.0f);

    EXPECT_EQ(perspective, camera->GetProjectionMatrix());

    camera->SetupFromSceneCamera(*cameras[1], 2.0f);

    EXPECT_FLOAT_EQ(camera->GetProjectionMatrix()[0][0], 0.5f);

    EXPECT_FLOAT_EQ(camera->GetProjectionMatrix()[1][1], -1.0f / 3.0f);

    const sg::CameraUniformData* uniforms = reinterpret_cast<const sg::CameraUniformData*>(camera->GetUniformData());

    EXPECT_EQ(uniforms->projViewMatrix, uniforms->proj * uniforms->view);
}

TEST(SceneImport, ReloadReplacesNodesComponentsAndPayloadAndRejectsOtherFormats)
{
    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(SceneFixture("complete_scene.gltf"), &scene)) << loader.GetError();

    ASSERT_TRUE(loader.LoadFromFile(SceneFixture("normal_material.gltf"), &scene)) << loader.GetError();

    EXPECT_TRUE(scene.GetComponents<sg::Light>().empty());

    EXPECT_TRUE(scene.GetComponents<sg::SceneCamera>().empty());

    EXPECT_TRUE(scene.GetAssetData().animations.empty());

    EXPECT_EQ(scene.GetRenderableCount(), 1u);

    EXPECT_EQ(scene.GetNodes().size(), 1u);

    EXPECT_EQ(loader.GetVertices().size(), 3u);

    EXPECT_TRUE(rc::BuildSceneLights(scene).empty());

    EXPECT_FALSE(loader.LoadFromFile("scene.obj", &scene));

    EXPECT_NE(loader.GetError().find("Unsupported scene format"), std::string::npos) << loader.GetError();

    EXPECT_FALSE(loader.LoadFromFile("missing.gltf", &scene));
}

TEST(SceneImport, TriangleStripsConvertToTriangleLists)
{
    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(SceneFixture("strip_scene.gltf"), &scene)) << loader.GetError();

    EXPECT_EQ(loader.GetIndices(), std::vector<uint32_t>({0, 1, 2}));
}

TEST(SceneImport, QuantizedPositionsAndAssetsWithoutSceneAreSupported)
{
    for (const char* fixture : {"quantized_scene.gltf", "forest_scene.gltf"})
    {
        sg::Scene scene;

        asset::FastGLTFLoader loader;

        ASSERT_TRUE(loader.LoadFromFile(SceneFixture(fixture), &scene)) << loader.GetError();

        ASSERT_EQ(loader.GetVertices().size(), 3u);

        EXPECT_EQ(loader.GetVertices()[0].pos, Vec4(-1, -1, 0, 1));

        EXPECT_EQ(scene.GetRenderableCount(), 1u);

        EXPECT_TRUE(rc::BuildSceneLights(scene).empty());
    }
}

TEST(SceneImport, ZeroIntensityAndInstancedLightsArePreserved)
{
    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(SceneFixture("zero_light_scene.gltf"), &scene)) << loader.GetError();

    const HeapVector<rc::SceneLight> lights = rc::BuildSceneLights(scene);

    ASSERT_EQ(lights.size(), 2u);

    EXPECT_FLOAT_EQ(lights[0].intensity, 0.0f);

    EXPECT_NE(lights[0].position, lights[1].position);
}

TEST(SceneImport, UndecodableTextureFailsTheImportAndTheLoaderRecovers)
{
    std::ifstream source(SceneFixture("complete_scene.gltf"), std::ios::binary);

    std::string json((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());

    const std::string prefix = "data:image/png;base64,";

    const size_t begin       = json.find(prefix);

    ASSERT_NE(begin, std::string::npos);

    // Three zero bytes match no supported image format.
    json.replace(begin + prefix.size(), json.find('"', begin) - begin - prefix.size(), "AAAA");

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "zen_undecodable_texture.gltf";

    std::ofstream(path, std::ios::binary) << json;

    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(SceneFixture("complete_scene.gltf"), &scene)) << loader.GetError();

    const sg::Node* previous = scene.GetRenderableNodes().front();

    // Textures decode on worker threads; their failure must reach the caller.
    EXPECT_FALSE(loader.LoadFromFile(path.string(), &scene));

    EXPECT_NE(loader.GetError().find("Failed to decode glTF texture"), std::string::npos) << loader.GetError();

    EXPECT_EQ(scene.GetRenderableNodes().front(), previous);

    // The next import starts without the earlier error.
    EXPECT_TRUE(loader.LoadFromFile(SceneFixture("complete_scene.gltf"), &scene)) << loader.GetError();

    EXPECT_TRUE(loader.GetError().empty());

    std::filesystem::remove(path);
}

TEST(SceneImport, FailedImportsDoNotPublishPartialScenesOrTraverseCycles)
{
    sg::Scene scene;

    asset::FastGLTFLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(SceneFixture("complete_scene.gltf"), &scene)) << loader.GetError();

    const sg::Node* previous                  = scene.GetRenderableNodes().front();

    const std::vector<asset::Vertex> vertices = loader.GetVertices();

    const sg::Texture* defaultTexture         = scene.GetDefaultTextures().pBaseColor;

    EXPECT_FALSE(loader.LoadFromFile(SceneFixture("cyclic_scene.gltf"), &scene));

    EXPECT_EQ(scene.GetRenderableNodes().front(), previous);

    EXPECT_FALSE(loader.LoadFromFile(SceneFixture("unsupported_uv_scene.gltf"), &scene));

    EXPECT_EQ(scene.GetRenderableNodes().front(), previous);

    for (const char* fixture : {"invalid_texture_scene.gltf", "invalid_morph_scene.gltf"})
    {
        EXPECT_FALSE(loader.LoadFromFile(SceneFixture(fixture), &scene));

        EXPECT_EQ(scene.GetRenderableNodes().front(), previous);
    }

    EXPECT_EQ(scene.GetComponents<sg::Light>().size(), 2u);

    EXPECT_EQ(scene.GetDefaultTextures().pBaseColor, defaultTexture);

    ASSERT_EQ(loader.GetVertices().size(), vertices.size());

    EXPECT_EQ(loader.GetVertices()[0].pos, vertices[0].pos);
}

TEST(SceneImport, AutomaticCameraFramesFlatAndPointSizedModelsFromOutside)
{
    for (const sg::AABB& bounds : {sg::AABB(Vec3(-1, -1, 0), Vec3(1, 1, 0)), sg::AABB(Vec3(0), Vec3(0))})
    {
        UniquePtr<sg::Camera> camera = sg::Camera::CreateUnique(Vec3(0, 0, 1), Vec3(0), 1.0f);

        camera->SetupOnAABB(bounds);

        EXPECT_GT(camera->GetPos().y, bounds.GetMax().y);

        for (uint32_t column = 0; column < 4; ++column)
        {
            for (uint32_t row = 0; row < 4; ++row)
            {
                EXPECT_TRUE(std::isfinite(camera->GetViewMatrix()[column][row]));
            }
        }
    }
}
