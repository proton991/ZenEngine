#include "Math/Math.h"
#include "AssetLib/SceneLoader.h"
#include "AssetLib/SceneAssetIO.h"
#include "AssetLib/SceneRuntimeBuilder.h"
#include "ZenAsset/Processor/Assembly.h"
#include "ZenAsset/Reader/Assembly.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace zen;

namespace
{
zenasset::Scene AssemblyTriangle(const std::string& name, bool sparseUV, bool deform = false)
{
    zenasset::Scene scene;
    scene.name           = name;
    scene.sourcePath     = "missing-source/" + name + ".gltf";
    scene.sourceDocument = "{\"source\":\"" + name + "\"}";
    scene.vertices.resize(3);
    scene.vertices[1].pos.x = 1;
    scene.vertices[2].pos.y = 1;
    for (zenasset::Vertex& vertex : scene.vertices)
    {
        vertex.normal  = zenasset::Vec4(0, 0, 1, 0);
        vertex.tangent = zenasset::Vec4(1, 0, 0, 1);
        vertex.weight0 = zenasset::Vec4(1, 0, 0, 0);
    }
    scene.indices            = {0, 1, 2};
    scene.vertexTexCoordSets = sparseUV ? std::vector<uint64_t>{0, 7} : std::vector<uint64_t>{1};
    scene.vertexTexCoords.resize(3);
    for (zenasset::VertexTextureCoordinates& coordinates : scene.vertexTexCoords)
    {
        coordinates = sparseUV ? zenasset::VertexTextureCoordinates{zenasset::Vec2(.2f), zenasset::Vec2(.7f)}
                               : zenasset::VertexTextureCoordinates{zenasset::Vec2(.4f)};
    }
    // Canonical rows may omit trailing sets; the assembly must zero-fill them.
    if (sparseUV)
    {
        scene.vertexTexCoords[2].resize(1);
    }
    std::shared_ptr<zenasset::ImagePixels> pixels = std::make_shared<zenasset::ImagePixels>();
    pixels->width = pixels->height = 1;
    pixels->bytes                  = {uint8_t(sparseUV ? 32 : 64), 128, 192, 255};
    scene.images.push_back({"image-" + name, pixels});
    scene.samplers.resize(1);
    scene.textures.resize(2);
    scene.textures[0].image        = zenasset::ImageId{0};
    scene.textures[0].sampler      = zenasset::SamplerId{0};
    scene.textures[0].linearSource = zenasset::TextureId{1};
    scene.textures[0].format       = zenasset::PixelFormat::RGBA8Srgb;
    scene.textures[1].image        = zenasset::ImageId{0};
    scene.materials.resize(1);
    zenasset::MaterialAsset& material           = scene.materials[0];
    material.name                               = "material-" + name;
    material.m_pBaseColorTexture                = zenasset::TextureId{0};
    material.texCoordSets.baseColor             = sparseUV ? 1 : 0;
    material.features.clearcoatTexture.texture  = zenasset::TextureId{1};
    material.features.clearcoatTexture.texCoord = sparseUV ? 1 : 0;
    scene.meshes.resize(1);
    scene.meshes[0].name = "mesh-" + name;
    zenasset::MeshPrimitiveAsset primitive;
    primitive.vertexCount = primitive.indexCount = 3;
    primitive.material                           = zenasset::MaterialId{0};
    primitive.materialSlot                       = 0;
    primitive.bounds                             = zenasset::Bounds(zenasset::Vec3(0), zenasset::Vec3(1, 1, 0));
    scene.meshes[0].primitives.push_back(primitive);
    scene.nodes.resize(3);
    scene.nodes[0].name               = "mesh-node-" + name;
    scene.nodes[0].mesh               = zenasset::MeshId{0};
    scene.nodes[0].translation.x      = sparseUV ? 2.0f : 1.0f;
    scene.nodes[0].materials          = {zenasset::MaterialId{0}};
    scene.nodes[0].children           = {zenasset::NodeId{1}};
    scene.nodes[1].name               = "joint-light-camera-" + name;
    scene.nodes[1].parent             = zenasset::NodeId{0};
    scene.nodes[1].morphWeightsSource = zenasset::NodeId{0};
    scene.nodes[1].camera             = zenasset::CameraId{0};
    scene.nodes[1].light              = zenasset::LightId{0};
    // Definitions outside the selected scene remain unparented and uninstantiated.
    scene.nodes[2].name = "unselected-root-" + name;
    scene.roots         = {zenasset::NodeId{0}};
    scene.cameras.resize(1);
    scene.lights.resize(1);
    scene.lights[0].intensity = 3;
    scene.imageBasedLights.resize(1);
    scene.imageBasedLights[0].name      = "environment-" + name;
    scene.imageBasedLights[0].intensity = sparseUV ? 2.0f : 4.0f;
    scene.imageBasedLights[0].irradianceCoefficients.resize(9, zenasset::Vec3(.5f));
    std::shared_ptr<zenasset::EnvironmentCubePixels> cube = std::make_shared<zenasset::EnvironmentCubePixels>();
    cube->size = cube->mipLevels = 1;
    cube->rgba32FloatMipFaces.resize(6);
    for (std::vector<zenasset::Vec4>& face : cube->rgba32FloatMipFaces)
    {
        face.push_back(zenasset::Vec4(1));
    }
    scene.imageBasedLights[0].specular = cube;
    scene.imageBasedLight              = 0;
    scene.materialVariants             = {"night"};
    scene.variantPrimitives.push_back({0, 0, {0}});
    scene.extraVertexAttributes.push_back(
        {0, 0, "_CUSTOM", -1, 1, 5126, false, zenasset::VertexAttributeDomain::PrimitiveVertex, {1, 2, 3}});
    if (deform)
    {
        scene.nodes[0].skin         = zenasset::SkinId{0};
        scene.nodes[0].morphWeights = {.25f};
        scene.skins.push_back({"skin-" + name, 1, {1}, {zenasset::Mat4(1)}});
        scene.meshes[0].primitives[0].influences.resize(3, {{0, 1}});
        scene.morphPrimitives.resize(1);
        scene.morphPrimitives[0].targets.resize(1);
        zenasset::MorphTargetAsset& morph = scene.morphPrimitives[0].targets[0];
        morph.positions.resize(3, zenasset::Vec3(.1f, 0, 0));
        morph.texCoords.resize(scene.vertexTexCoordSets.size());
        for (std::vector<zenasset::Vec2>& set : morph.texCoords)
        {
            set.resize(3, zenasset::Vec2(.05f));
        }
        zenasset::AnimationAsset clip;
        clip.name = "clip-" + name;
        clip.samplers.push_back({zenasset::AnimationInterpolation::Linear, 3, {0, 1}, {0, 0, 0, 1, 0, 0}});
        clip.samplers.push_back({zenasset::AnimationInterpolation::Linear, 1, {0, 1}, {.2f, .8f}});
        clip.channels.push_back({0,
                                 0,
                                 zenasset::AnimationPath::Translation,
                                 {zenasset::AnimatedObject::Node, 0, zenasset::AnimatedProperty::Translation, 3}});
        const zenasset::AnimatedObject   objects[] = {zenasset::AnimatedObject::Material, zenasset::AnimatedObject::Camera,
                                                      zenasset::AnimatedObject::Light, zenasset::AnimatedObject::Environment};
        const zenasset::AnimatedProperty properties[] = {
            zenasset::AnimatedProperty::Roughness, zenasset::AnimatedProperty::CameraFov,
            zenasset::AnimatedProperty::LightIntensity, zenasset::AnimatedProperty::EnvironmentIntensity};
        for (uint32_t index = 0; index < 4; ++index)
        {
            clip.channels.push_back({1, 0, zenasset::AnimationPath::Property, {objects[index], 0, properties[index], 1}});
        }
        scene.animations.push_back(std::move(clip));
    }
    return scene;
}

class SceneAssemblyTest : public testing::Test
{
protected:
    void SetUp() override
    {
        const int64_t stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        directory           = std::filesystem::temp_directory_path() / ("zen_assembly_tests_" + std::to_string(stamp));
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        ASSERT_FALSE(error) << error.message();
        path = directory / "scene.zenscene";
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        EXPECT_FALSE(error) << error.message();
    }

    void WriteModels(bool deform = false)
    {
        for (uint32_t index = 0; index < 2; ++index)
        {
            const std::string key = index == 0 ? "first" : "second";
            ASSERT_NO_FATAL_FAILURE(StoreModel(index, AssemblyTriangle(key, index == 0, deform)));
        }
        assembly.instances.push_back({"one", "first"});
        zenasset::AssemblyInstance second{"two", "second"};
        second.transform[0].x = second.transform[1].y = second.transform[2].z = 2;
        second.transform[3].x                                                 = 10;
        second.submitLights                                                   = false;
        assembly.instances.push_back(second);
        zenasset::AssemblyInstance repeat{"three", "first"};
        repeat.transform[3].x = -4;
        assembly.instances.push_back(repeat);
        assembly.camera      = {"two", 0};
        assembly.environment = {"two", 0};
        WriteAssembly();
    }

    void StoreModel(uint32_t index, const zenasset::Scene& scene, const zenasset::SceneWriteOptions& options = {})
    {
        const std::string           key       = index == 0 ? "first" : "second";
        const std::filesystem::path modelPath = directory / (key + ".zenasset");
        const zenasset::Status      written   = zenasset::WriteScene(modelPath.string(), scene, options);
        ASSERT_TRUE(written.Succeeded()) << written.message;
        const zenasset::PackageResult package = zenasset::OpenPackage(modelPath.string());
        ASSERT_TRUE(package.status.Succeeded()) << package.status.message;
        zenasset::AssemblyModel model;
        model.key                         = key;
        model.path                        = modelPath.filename().string();
        model.identity                    = package.package->Identity();
        const zenasset::Status summarized = zenasset::SummarizeAssemblyModel(scene, model);
        ASSERT_TRUE(summarized.Succeeded()) << summarized.message;
        if (index == assembly.models.size())
        {
            assembly.models.push_back(std::move(model));
        }
        else
        {
            assembly.models[index] = std::move(model);
        }
    }

    void WriteAssembly()
    {
        const zenasset::Status finalized = zenasset::FinalizeAssembly(assembly);
        ASSERT_TRUE(finalized.Succeeded()) << finalized.message;
        const zenasset::Status written = zenasset::WriteAssembly(path.string(), assembly);
        ASSERT_TRUE(written.Succeeded()) << written.message;
    }

    std::filesystem::path   directory;
    std::filesystem::path   path;
    zenasset::SceneAssembly assembly;
};
} // namespace

TEST_F(SceneAssemblyTest, SharesRepeatedGeometryAndRemapsMixedMaterialAndUVTables)
{
    ASSERT_NO_FATAL_FAILURE(WriteModels());
    const asset::SceneLoadResult loaded = asset::LoadScene(path.string());
    ASSERT_TRUE(loaded.scene) << loaded.error;
    ASSERT_TRUE(loaded.asset);
    EXPECT_EQ(loaded.format, "zenscene");
    const asset::SceneAsset& canonical = *loaded.asset;
    EXPECT_TRUE(canonical.sourceDocument.empty());
    ASSERT_EQ(canonical.vertices.size(), 6u);
    ASSERT_EQ(canonical.indices.size(), 6u);
    for (uint32_t index = 0; index < 6; ++index)
    {
        EXPECT_EQ(canonical.indices[index], index);
    }
    ASSERT_EQ(canonical.vertexTexCoordSets.size(), 3u);
    EXPECT_EQ(canonical.vertexTexCoordSets[0], 0u);
    EXPECT_EQ(canonical.vertexTexCoordSets[1], 1u);
    EXPECT_EQ(canonical.vertexTexCoordSets[2], 7u);
    for (uint32_t index = 0; index < 6; ++index)
    {
        ASSERT_EQ(canonical.vertexTexCoords[index].size(), 3u);
        EXPECT_EQ(canonical.vertexTexCoords[index][0], index < 3 ? Vec2(.2f) : Vec2(0));
        EXPECT_EQ(canonical.vertexTexCoords[index][1], index < 3 ? Vec2(0) : Vec2(.4f));
        EXPECT_EQ(canonical.vertexTexCoords[index][2], index < 2 ? Vec2(.7f) : Vec2(0));
    }
    ASSERT_EQ(canonical.meshes.size(), 2u);
    ASSERT_EQ(canonical.materials.size(), 2u);
    ASSERT_EQ(canonical.images.size(), 2u);
    ASSERT_EQ(canonical.textures.size(), 4u);
    ASSERT_EQ(canonical.samplers.size(), 2u);
    for (uint32_t model = 0; model < 2; ++model)
    {
        const asset::MeshPrimitiveAsset& primitive = canonical.meshes[model].primitives[0];
        EXPECT_EQ(primitive.firstVertex, 3 * model);
        EXPECT_EQ(primitive.firstIndex, 3 * model);
        EXPECT_EQ(primitive.material.value, model);
        EXPECT_EQ(canonical.materials[model].m_pBaseColorTexture.value, 2 * model);
        EXPECT_EQ(canonical.materials[model].features.clearcoatTexture.texture.value, 2 * model + 1);
        EXPECT_EQ(canonical.materials[model].texCoordSets.baseColor, model == 0 ? 2u : 1u);
        EXPECT_EQ(canonical.materials[model].features.clearcoatTexture.texCoord, model == 0 ? 2u : 1u);
        EXPECT_EQ(canonical.textures[2 * model].linearSource.value, 2 * model + 1);
        EXPECT_EQ(canonical.textures[2 * model].image.value, model);
        EXPECT_EQ(canonical.textures[2 * model].sampler.value, model);
    }
    ASSERT_EQ(canonical.materialVariants.size(), 2u);
    ASSERT_EQ(canonical.variantPrimitives.size(), 2u);
    for (uint32_t model = 0; model < 2; ++model)
    {
        EXPECT_NE(canonical.materialVariants[model].find(model == 0 ? "first" : "second"), std::string::npos);
        EXPECT_EQ(canonical.variantPrimitives[model].mesh, model);
        ASSERT_EQ(canonical.variantPrimitives[model].materials.size(), 2u);
        EXPECT_EQ(canonical.variantPrimitives[model].materials[model], static_cast<int32_t>(model));
        EXPECT_EQ(canonical.variantPrimitives[model].materials[1 - model], -1);
        EXPECT_EQ(canonical.extraVertexAttributes[model].mesh, model);
    }
    EXPECT_EQ(loaded.scene->scene->GetRenderableCount(), 3u);
    const HeapVector<sg::Node*>& nodes = loaded.scene->scene->GetRenderableNodes();
    HeapVector<float>            positions;
    for (const sg::Node* node : nodes)
    {
        positions.push_back(node->GetData().modelMatrix[3].x);
    }
    std::sort(positions.begin(), positions.end());
    EXPECT_EQ(positions[0], -2);
    EXPECT_EQ(positions[1], 2);
    EXPECT_EQ(positions[2], 12);
    EXPECT_EQ(loaded.scene->normalizationCenter, Vec3(6, 1, 0));
    EXPECT_EQ(loaded.scene->normalizationScale, 1.0f / 16);
}

TEST_F(SceneAssemblyTest, LargerGeometryLoadsFirstWithoutChangingResourceOrInstanceOrder)
{
    ASSERT_NO_FATAL_FAILURE(WriteModels(true));
    zenasset::Scene second                    = AssemblyTriangle("second", false, true);
    second.indices                            = {0, 1, 2, 0, 1, 2};
    second.meshes[0].primitives[0].indexCount = 6;
    ASSERT_NO_FATAL_FAILURE(StoreModel(1, second));
    ASSERT_NO_FATAL_FAILURE(WriteAssembly());

    const asset::SceneLoadResult loaded = asset::LoadScene(path.string());
    ASSERT_TRUE(loaded.scene) << loaded.error;
    ASSERT_TRUE(loaded.asset);
    const asset::SceneAsset& canonical = *loaded.asset;
    ASSERT_EQ(canonical.meshes.size(), 2u);
    const asset::MeshPrimitiveAsset& firstPrimitive  = canonical.meshes[0].primitives[0];
    const asset::MeshPrimitiveAsset& secondPrimitive = canonical.meshes[1].primitives[0];
    EXPECT_EQ(firstPrimitive.firstVertex, 3u);
    EXPECT_EQ(firstPrimitive.firstIndex, 6u);
    EXPECT_EQ(secondPrimitive.firstVertex, 0u);
    EXPECT_EQ(secondPrimitive.firstIndex, 0u);
    for (uint32_t model = 0; model < 2; ++model)
    {
        const asset::MeshPrimitiveAsset& primitive = canonical.meshes[model].primitives[0];
        EXPECT_EQ(primitive.material.value, model);
        EXPECT_EQ(canonical.materials[model].m_pBaseColorTexture.value, 2 * model);
        EXPECT_EQ(canonical.textures[2 * model].image.value, model);
        EXPECT_EQ(canonical.morphPrimitives[model].mesh, model);
        EXPECT_EQ(canonical.extraVertexAttributes[model].mesh, model);
        EXPECT_EQ(canonical.variantPrimitives[model].mesh, model);
        for (uint32_t index = 0; index < primitive.indexCount; ++index)
        {
            EXPECT_EQ(canonical.indices[primitive.firstIndex + index], primitive.firstVertex + index % 3);
        }
    }
    EXPECT_EQ(canonical.vertexTexCoords[firstPrimitive.firstVertex][0], Vec2(.2f));
    EXPECT_EQ(canonical.vertexTexCoords[firstPrimitive.firstVertex][2], Vec2(.7f));
    EXPECT_EQ(canonical.vertexTexCoords[secondPrimitive.firstVertex][1], Vec2(.4f));
    ASSERT_EQ(canonical.roots.size(), 3u);
    EXPECT_EQ(canonical.nodes[canonical.roots[0].value].name, "one");
    EXPECT_EQ(canonical.nodes[canonical.roots[1].value].name, "two");
    EXPECT_EQ(canonical.nodes[canonical.roots[2].value].name, "three");
    EXPECT_EQ(canonical.nodes[canonical.roots[0].value + 1].mesh.value, 0u);
    EXPECT_EQ(canonical.nodes[canonical.roots[1].value + 1].mesh.value, 1u);
    EXPECT_EQ(canonical.nodes[canonical.roots[2].value + 1].mesh.value, 0u);
    EXPECT_EQ(loaded.scene->scene->GetRenderableCount(), 3u);
    EXPECT_EQ(canonical.preferredCameraNode, static_cast<int32_t>(canonical.roots[1].value + 2));
    EXPECT_EQ(canonical.imageBasedLight, 1);
}

TEST_F(SceneAssemblyTest, StaticModelKeepsItsCoordinatesInVertexStreamsAndPublishesBuildDiagnostics)
{
    zenasset::SceneWriteOptions options;
    options.metadata.diagnostics.push_back(
        {zenasset::DiagnosticSeverity::Warning, zenasset::StatusCode::InvalidData, "first.gltf", "clip", "Dropped a clip"});
    ASSERT_NO_FATAL_FAILURE(StoreModel(0, AssemblyTriangle("first", false), options));
    assembly.instances.push_back({"one", "first"});
    ASSERT_NO_FATAL_FAILURE(WriteAssembly());
    const asset::SceneLoadResult loaded = asset::LoadScene(path.string());
    ASSERT_TRUE(loaded.scene) << loaded.error;
    ASSERT_EQ(loaded.diagnostics.size(), 1u);
    EXPECT_EQ(loaded.diagnostics[0].severity, asset::SceneDiagnosticSeverity::Warning);
    EXPECT_EQ(loaded.diagnostics[0].object, "first/clip");
    EXPECT_EQ(loaded.diagnostics[0].message, "Dropped a clip");
    // One set fits uv0, so neither the composed asset nor a static runtime scene needs a second stream.
    ASSERT_TRUE(loaded.asset);
    EXPECT_TRUE(loaded.asset->vertexTexCoords.empty());
    ASSERT_EQ(loaded.asset->vertices.size(), 3u);
    EXPECT_EQ(loaded.asset->vertices[0].uv0, Vec2(.4f));
    const sg::SceneAssetData& data = loaded.scene->scene->GetAssetData();
    EXPECT_TRUE(data.deformations.empty());
    EXPECT_TRUE(data.bindVertices.empty());
    EXPECT_TRUE(data.bindVertexTexCoords.empty());
    EXPECT_TRUE(data.vertexTexCoords.empty());
    ASSERT_EQ(loaded.scene->vertices.size(), 3u);
    EXPECT_EQ(loaded.scene->vertices[0].uv0, Vec2(.4f));
}

TEST_F(SceneAssemblyTest, UninstantiatedSkinnedNodeKeepsAStaticTransferWithoutCoordinateRows)
{
    zenasset::Scene model = AssemblyTriangle("first", false);
    // The skinned definition is outside the selected scene, so no instance deforms.
    model.nodes[2].mesh = zenasset::MeshId{0};
    model.nodes[2].skin = zenasset::SkinId{0};
    model.skins.push_back({"skin-first", 1, {1}, {zenasset::Mat4(1)}});
    model.meshes[0].primitives[0].influences.resize(3, {{0, 1}});
    ASSERT_NO_FATAL_FAILURE(StoreModel(0, model));
    assembly.instances.push_back({"one", "first"});
    ASSERT_NO_FATAL_FAILURE(WriteAssembly());
    asset::SceneImportRequest request{path.string()};
    request.retainCanonicalAsset        = false;
    const asset::SceneLoadResult loaded = asset::LoadScene(request);
    ASSERT_TRUE(loaded.scene) << loaded.error;
    const sg::SceneAssetData& data = loaded.scene->scene->GetAssetData();
    EXPECT_TRUE(data.deformations.empty());
    EXPECT_TRUE(data.bindVertices.empty());
    EXPECT_TRUE(data.vertexTexCoords.empty());
    ASSERT_EQ(loaded.scene->vertices.size(), 3u);
    EXPECT_EQ(loaded.scene->vertices[0].uv0, Vec2(.4f));
}

TEST_F(SceneAssemblyTest, RuntimeOnlyLoadsPreserveGeometryAndLeaseNativeImages)
{
    ASSERT_NO_FATAL_FAILURE(WriteModels());
    const asset::SceneLoadResult reference = asset::LoadScene(path.string());
    ASSERT_TRUE(reference.scene) << reference.error;
    asset::SceneImportRequest request{path.string()};
    request.retainCanonicalAsset   = false;
    asset::SceneLoadResult runtime = asset::LoadScene(request);
    ASSERT_TRUE(runtime.scene) << runtime.error;
    EXPECT_FALSE(runtime.asset);
    EXPECT_EQ(runtime.scene->vertices.size(), reference.scene->vertices.size());
    EXPECT_EQ(runtime.scene->indices.size(), reference.scene->indices.size());
    EXPECT_TRUE(std::equal(runtime.scene->indices.begin(), runtime.scene->indices.end(), reference.scene->indices.begin()));
    for (size_t vertex = 0; vertex < runtime.scene->vertices.size(); ++vertex)
    {
        EXPECT_EQ(runtime.scene->vertices[vertex].pos, reference.scene->vertices[vertex].pos);
        EXPECT_EQ(runtime.scene->vertices[vertex].uv0, reference.scene->vertices[vertex].uv0);
        EXPECT_EQ(runtime.scene->vertices[vertex].uv1, reference.scene->vertices[vertex].uv1);
    }
    const HeapVector<sg::Texture*>& textures = runtime.scene->scene->GetComponents<sg::Texture>();
    ASSERT_FALSE(textures.empty());
    const SharedPtr<const asset::ImagePixels, MultiThreadCounter> descriptor = textures[0]->pixels;
    ASSERT_TRUE(descriptor);
    EXPECT_TRUE(descriptor->payloadSource);
    EXPECT_TRUE(descriptor->bytes.empty());
    runtime.scene.Reset();
    SharedPtr<const asset::ImagePixels, MultiThreadCounter> lease;
    ASSERT_TRUE(asset::ResolveImagePixels(descriptor, lease));
    EXPECT_EQ(lease->bytes.size(), 4u);
    EXPECT_FALSE(lease->payloadSource);
    lease.Reset();
    EXPECT_TRUE(descriptor->bytes.empty());
    request.path                        = (directory / "first.zenasset").string();
    const asset::SceneLoadResult single = asset::LoadScene(request);
    ASSERT_TRUE(single.scene) << single.error;
    EXPECT_FALSE(single.asset);
    EXPECT_FALSE(single.scene->vertices.empty());
}

TEST_F(SceneAssemblyTest, PreservesAuthoredLightsAndUsesExplicitInstanceSelections)
{
    ASSERT_NO_FATAL_FAILURE(WriteModels());
    const asset::SceneLoadResult loaded = asset::LoadScene(path.string());
    ASSERT_TRUE(loaded.scene) << loaded.error;
    ASSERT_TRUE(loaded.asset);
    uint32_t authored  = 0;
    uint32_t submitted = 0;
    for (const asset::NodeAsset& node : loaded.asset->nodes)
    {
        authored  += node.light.IsValid() ? 1u : 0u;
        submitted += node.light.IsValid() && node.submitLight ? 1u : 0u;
    }
    EXPECT_EQ(authored, 3u);
    EXPECT_EQ(submitted, 2u);
    EXPECT_EQ(loaded.scene->scene->GetComponents<sg::Light>().size(), 2u);
    ASSERT_GE(loaded.asset->preferredCameraNode, 0);
    EXPECT_EQ(loaded.scene->scene->GetAssetData().preferredCameraNode, loaded.asset->preferredCameraNode);
    ASSERT_NE(loaded.scene->scene->GetSelectedCamera(), nullptr);
    EXPECT_EQ(loaded.scene->scene->GetSelectedCamera()->worldMatrix[3].x, 12);
    const asset::NodeAsset& cameraNode = loaded.asset->nodes[loaded.asset->preferredCameraNode];
    EXPECT_TRUE(cameraNode.camera.IsValid());
    bool found = false;
    for (const UniquePtr<sg::Node>& node : loaded.scene->scene->GetNodes())
    {
        if (node->GetIndex() == static_cast<uint32_t>(loaded.asset->preferredCameraNode))
        {
            EXPECT_EQ(node->GetData().modelMatrix[3].x, 12);
            found = true;
        }
    }
    EXPECT_TRUE(found);
    ASSERT_GE(loaded.asset->imageBasedLight, 0);
    EXPECT_EQ(loaded.asset->imageBasedLights[loaded.asset->imageBasedLight].intensity, 4);
    EXPECT_EQ(loaded.scene->scene->GetAssetData().imageBasedLight, loaded.asset->imageBasedLight);
    assembly.camera      = {};
    assembly.environment = {};
    ASSERT_NO_FATAL_FAILURE(WriteAssembly());
    const asset::SceneLoadResult unselected = asset::LoadScene(path.string());
    ASSERT_TRUE(unselected.scene) << unselected.error;
    EXPECT_EQ(unselected.asset->preferredCameraNode, -1);
    EXPECT_EQ(unselected.asset->imageBasedLight, -1);
    EXPECT_TRUE(unselected.scene->scene->GetAssetData().explicitCameraSelection);
    EXPECT_EQ(unselected.scene->scene->GetSelectedCamera(), nullptr);
}

TEST_F(SceneAssemblyTest, CameraSelectionRequiresAnInstantiatedNodeAndSkipsUnselectedDefinitions)
{
    ASSERT_NO_FATAL_FAILURE(WriteModels());
    zenasset::Scene inactive = AssemblyTriangle("second", false);
    inactive.nodes[1].camera = {};
    inactive.nodes[2].camera = zenasset::CameraId{0};
    ASSERT_NO_FATAL_FAILURE(StoreModel(1, inactive));
    ASSERT_NO_FATAL_FAILURE(WriteAssembly());
    const asset::SceneLoadResult missing = asset::LoadScene(path.string());
    EXPECT_EQ(missing.status, asset::SceneImportStatus::InvalidData);
    EXPECT_FALSE(missing.asset);
    EXPECT_FALSE(missing.scene);

    zenasset::Scene active = AssemblyTriangle("second", false);
    std::swap(active.nodes[1], active.nodes[2]);
    active.nodes[0].children = {zenasset::NodeId{2}};
    active.nodes[1].camera   = zenasset::CameraId{0};
    ASSERT_NO_FATAL_FAILURE(StoreModel(1, active));
    ASSERT_NO_FATAL_FAILURE(WriteAssembly());
    const asset::SceneLoadResult selected = asset::LoadScene(path.string());
    ASSERT_TRUE(selected.scene) << selected.error;
    ASSERT_NE(selected.scene->scene->GetSelectedCamera(), nullptr);
    EXPECT_EQ(selected.scene->scene->GetSelectedCamera()->worldMatrix[3].x, 12);
    ASSERT_GE(selected.asset->preferredCameraNode, 0);
    const asset::NodeAsset& node = selected.asset->nodes[selected.asset->preferredCameraNode];
    EXPECT_NE(node.name.find("joint-light-camera-second"), std::string::npos);
}

TEST_F(SceneAssemblyTest, LegacyWriterRejectsAssemblyPoliciesWithoutPublishingPartialOutput)
{
    ASSERT_NO_FATAL_FAILURE(WriteModels());
    const asset::SceneLoadResult assemblyScene = asset::LoadScene(path.string());
    ASSERT_TRUE(assemblyScene.asset) << assemblyScene.error;
    const std::filesystem::path    flattened = directory / "flattened.zenasset";
    const asset::SceneImportReport rejected  = asset::WriteSceneAssetFile(flattened.string(), *assemblyScene.asset);
    EXPECT_EQ(rejected.status, asset::SceneImportStatus::UnsupportedFeature);
    EXPECT_FALSE(std::filesystem::exists(flattened));

    const asset::SceneLoadResult single = asset::LoadScene((directory / "first.zenasset").string());
    ASSERT_TRUE(single.asset) << single.error;
    EXPECT_FALSE(single.asset->explicitCameraSelection);
    EXPECT_EQ(single.asset->preferredCameraNode, -1);
    asset::SceneAsset lightPolicy = *single.asset;
    ASSERT_TRUE(lightPolicy.nodes[1].light.IsValid());
    lightPolicy.nodes[1].submitLight             = false;
    const std::filesystem::path    lights        = directory / "light-policy.zenasset";
    const asset::SceneImportReport rejectedLight = asset::WriteSceneAssetFile(lights.string(), lightPolicy);
    EXPECT_EQ(rejectedLight.status, asset::SceneImportStatus::UnsupportedFeature);
    EXPECT_FALSE(std::filesystem::exists(lights));
}

TEST_F(SceneAssemblyTest, RelocatesWithoutSourcesAndRejectsChangedDependenciesTransactionally)
{
    ASSERT_NO_FATAL_FAILURE(WriteModels());
    const std::filesystem::path relocated = directory / "relocated";
    std::error_code             error;
    std::filesystem::create_directories(relocated, error);
    ASSERT_FALSE(error);
    for (const char* name : {"scene.zenscene", "first.zenasset", "second.zenasset"})
    {
        std::filesystem::rename(directory / name, relocated / name, error);
        ASSERT_FALSE(error) << error.message();
    }
    path                                  = relocated / "scene.zenscene";
    const asset::SceneLoadResult previous = asset::LoadScene(path.string());
    ASSERT_TRUE(previous.scene) << previous.error;
    zenasset::Scene changed          = AssemblyTriangle("second", false);
    changed.vertices[1].pos.x        = .5f;
    const zenasset::Status rewritten = zenasset::WriteScene((relocated / "second.zenasset").string(), changed);
    ASSERT_TRUE(rewritten.Succeeded()) << rewritten.message;
    const asset::SceneLoadResult failed = asset::LoadScene(path.string());
    EXPECT_NE(failed.status, asset::SceneImportStatus::Success);
    EXPECT_FALSE(failed.scene);
    EXPECT_FALSE(failed.asset);
    EXPECT_EQ(previous.scene->scene->GetRenderableCount(), 3u);
    EXPECT_EQ(previous.asset->vertices[4].pos.x, 1);
    asset::SceneImportRequest cancelled{path.string()};
    cancelled.control = SharedPtr<asset::SceneImportControl, MultiThreadCounter>(new asset::SceneImportControl);
    cancelled.control->Cancel();
    const asset::SceneLoadResult stopped = asset::LoadScene(cancelled);
    EXPECT_EQ(stopped.status, asset::SceneImportStatus::Cancelled);
    EXPECT_FALSE(stopped.scene);
    EXPECT_FALSE(stopped.asset);
}

TEST_F(SceneAssemblyTest, RemapsSkinsMorphsAndTypedAnimationTargetsForRepeatedInstances)
{
    ASSERT_NO_FATAL_FAILURE(WriteModels(true));
    const asset::SceneLoadResult loaded = asset::LoadScene(path.string());
    ASSERT_TRUE(loaded.scene) << loaded.error;
    ASSERT_TRUE(loaded.asset);
    const asset::SceneAsset& canonical = *loaded.asset;
    EXPECT_EQ(canonical.vertices.size(), 6u);
    ASSERT_EQ(canonical.skins.size(), 3u);
    ASSERT_EQ(canonical.animations.size(), 3u);
    ASSERT_EQ(canonical.morphPrimitives.size(), 2u);
    for (const asset::NodeAsset& node : canonical.nodes)
    {
        if (node.mesh.IsValid())
        {
            ASSERT_TRUE(node.skin.IsValid());
            ASSERT_EQ(node.children.size(), 1u);
            const asset::SkinAsset& skin = canonical.skins[node.skin.value];
            ASSERT_EQ(skin.joints.size(), 1u);
            EXPECT_EQ(skin.joints[0], node.children[0].value);
            EXPECT_EQ(skin.skeleton, static_cast<int32_t>(node.children[0].value));
            EXPECT_EQ(canonical.nodes[node.children[0].value].morphWeightsSource.value,
                      static_cast<uint32_t>(&node - canonical.nodes.data()));
        }
    }
    for (const asset::AnimationAsset& clip : canonical.animations)
    {
        ASSERT_EQ(clip.channels.size(), 5u);
        const asset::NodeAsset& meshNode = canonical.nodes[clip.channels[0].node];
        EXPECT_TRUE(meshNode.mesh.IsValid());
        EXPECT_EQ(clip.channels[0].target.index, clip.channels[0].node);
        const asset::NodeAsset& child = canonical.nodes[meshNode.children[0].value];
        EXPECT_EQ(clip.channels[1].target.index, meshNode.materials[0].value);
        EXPECT_EQ(clip.channels[2].target.index, child.camera.value);
        EXPECT_EQ(clip.channels[3].target.index, child.light.value);
        EXPECT_LT(clip.channels[4].target.index, canonical.imageBasedLights.size());
    }
    for (const asset::MorphPrimitiveAsset& morph : canonical.morphPrimitives)
    {
        ASSERT_EQ(morph.targets.size(), 1u);
        ASSERT_EQ(morph.targets[0].texCoords.size(), 3u);
        const uint32_t absent = morph.mesh == 0 ? 1 : 0;
        EXPECT_TRUE(morph.targets[0].texCoords[absent].empty());
    }
}
