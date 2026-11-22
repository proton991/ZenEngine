#include "AssetLib/ProcessedSceneAdapter.h"
#include "AssetLib/SceneLoader.h"
#include "AssetLib/SceneRuntimeBuilder.h"
#include "AssetLib/SceneAssetIO.h"
#include "SceneGraph/Scene.h"
#include "ZenAsset/Format/Scene.h"
#include "ZenAsset/Format/Validation.h"
#include "ZenAsset/Processor/Write.h"
#include <gtest/gtest.h>
#include <limits>
#include <filesystem>

using namespace zen;

namespace
{
zenasset::Scene RichProcessedScene()
{
    zenasset::Scene scene;
    scene.name           = "independent processed scene";
    scene.sourcePath     = "models/example.gltf";
    scene.sourceDocument = "{\"extras\":{\"retained\":true}}";
    scene.dependencies.push_back({"textures/preferred.png", false, 0, 0, 0});
    scene.dependencies.push_back({"textures/fallback.png", true, 5, 1234, 5678});
    scene.vertices.resize(3);
    scene.vertices[0].pos = zenasset::Vec4(0, 0, 0, 1);
    scene.vertices[1].pos = zenasset::Vec4(1, 0, 0, 1);
    scene.vertices[2].pos = zenasset::Vec4(0, 1, 0, 1);
    for (zenasset::Vertex& vertex : scene.vertices)
    {
        vertex.normal  = zenasset::Vec4(0, 0, 1, 0);
        vertex.tangent = zenasset::Vec4(1, 0, 0, -1);
        vertex.uv0     = zenasset::Vec2(0.2f, 0.3f);
        vertex.uv1     = zenasset::Vec2(0.4f, 0.5f);
        vertex.weight0 = zenasset::Vec4(1, 0, 0, 0);
        vertex.color   = zenasset::Vec4(0.6f, 0.7f, 0.8f, 0.9f);
    }
    scene.indices            = {0, 1, 2};
    scene.vertexTexCoordSets = {0, 1, 7};
    scene.vertexTexCoords.resize(3, {zenasset::Vec2(0.2f), zenasset::Vec2(0.4f), zenasset::Vec2(0.6f)});
    scene.meshes.resize(1);
    scene.meshes[0].name         = "shared mesh";
    scene.meshes[0].morphWeights = {0.1f};
    zenasset::MeshPrimitiveAsset primitive;
    primitive.name           = "triangle";
    primitive.vertexCount    = 3;
    primitive.indexCount     = 3;
    primitive.material       = zenasset::MaterialId{0};
    primitive.materialSlot   = 0;
    primitive.bounds         = zenasset::Bounds(zenasset::Vec3(0), zenasset::Vec3(1, 1, 0));
    primitive.sourceVertices = {2, 0, 1};
    primitive.influences.resize(3, {{0, 0.25f}, {0, 0.75f}});
    primitive.flatNormals = true;
    scene.meshes[0].primitives.push_back(primitive);
    scene.nodes.resize(2);
    scene.nodes[0].name               = "mesh instance";
    scene.nodes[0].mesh               = zenasset::MeshId{0};
    scene.nodes[0].skin               = zenasset::SkinId{0};
    scene.nodes[0].camera             = zenasset::CameraId{0};
    scene.nodes[0].children           = {zenasset::NodeId{1}};
    scene.nodes[0].materials          = {zenasset::MaterialId{0}};
    scene.nodes[0].morphWeights       = {0.3f};
    scene.nodes[0].translation        = zenasset::Vec3(2, 3, 4);
    scene.nodes[0].rotation           = zenasset::Quat(0.5f, 0.5f, 0.5f, 0.5f);
    scene.nodes[0].scale              = zenasset::Vec3(2, 3, 4);
    scene.nodes[0].baseMatrix[1][0]   = 0.125f;
    scene.nodes[0].matrixTransform    = true;
    scene.nodes[0].selectable         = false;
    scene.nodes[0].hoverable          = false;
    scene.nodes[1].name               = "joint and light";
    scene.nodes[1].parent             = zenasset::NodeId{0};
    scene.nodes[1].morphWeightsSource = zenasset::NodeId{0};
    scene.nodes[1].light              = zenasset::LightId{0};
    scene.nodes[1].renderMesh         = false;
    scene.nodes[1].visible            = false;
    scene.roots                       = {zenasset::NodeId{0}};
    scene.skins.push_back({"skin", 1, {1}, {zenasset::Mat4(1)}});
    scene.skins[0].inverseBindMatrices[0][3][0] = -2;
    scene.morphPrimitives.resize(1);
    scene.morphPrimitives[0].primitive = UINT32_MAX;
    scene.morphPrimitives[0].weights   = {0.2f};
    scene.morphPrimitives[0].targets.resize(1);
    zenasset::MorphTargetAsset& morph = scene.morphPrimitives[0].targets[0];
    morph.positions.resize(3, zenasset::Vec3(0.1f, 0.2f, 0.3f));
    morph.normals.resize(3, zenasset::Vec3(0.01f));
    morph.tangents.resize(3, zenasset::Vec3(0.02f));
    morph.colors.resize(3, zenasset::Vec4(0.03f));
    morph.texCoords.resize(3, std::vector<zenasset::Vec2>(3, zenasset::Vec2(0.04f)));
    morph.extraColors.push_back({9, std::vector<zenasset::Vec4>(3, zenasset::Vec4(0.05f))});

    std::shared_ptr<zenasset::ImagePixels> pixels = std::make_shared<zenasset::ImagePixels>();
    pixels->width                                 = 2;
    pixels->height                                = 2;
    pixels->bytes.resize(16, 127);
    pixels->mips = {pixels->bytes, {1, 2, 3, 4}};
    scene.images.push_back({"shared color/data image", pixels});
    scene.samplers.resize(1);
    scene.samplers[0].name      = "authored sampler";
    scene.samplers[0].wrapS     = zenasset::SamplerAddressMode::MirroredRepeat;
    scene.samplers[0].wrapT     = zenasset::SamplerAddressMode::ClampToEdge;
    scene.samplers[0].minFilter = zenasset::TextureFilter::Nearest;
    scene.samplers[0].magFilter = zenasset::TextureFilter::Linear;
    scene.textures.resize(2);
    scene.textures[0].name         = "color view";
    scene.textures[0].image        = zenasset::ImageId{0};
    scene.textures[0].sampler      = zenasset::SamplerId{0};
    scene.textures[0].format       = zenasset::PixelFormat::RGBA8Srgb;
    scene.textures[0].linearSource = zenasset::TextureId{1};
    scene.textures[1].name         = "data view";
    scene.textures[1].image        = zenasset::ImageId{0};
    scene.textures[1].flatNormal   = true;
    scene.materials.resize(1);
    zenasset::MaterialAsset& material             = scene.materials[0];
    material.name                                 = "extended PBR";
    material.alphaMode                            = zenasset::AlphaMode::Mask;
    material.doubleSided                          = true;
    material.unlit                                = true;
    material.pbrWorkflows.metallicRoughness       = false;
    material.pbrWorkflows.specularGlossiness      = true;
    material.m_pBaseColorTexture                  = zenasset::TextureId{0};
    material.m_pMetallicRoughnessTexture          = zenasset::TextureId{1};
    material.m_pNormalTexture                     = zenasset::TextureId{1};
    material.m_pOcclusionTexture                  = zenasset::TextureId{1};
    material.m_pEmissiveTexture                   = zenasset::TextureId{0};
    material.texCoordSets.baseColor               = 2;
    material.texCoordSets.specularGlossiness      = 1;
    material.extension.pDiffuseTexture            = zenasset::TextureId{0};
    material.extension.pSpecularGlossinessTexture = zenasset::TextureId{1};
    material.extension.diffuseFactor              = zenasset::Vec4(0.25f);
    material.extension.specularFactor             = zenasset::Vec3(0.35f);
    material.features.diffuseTransmission         = 0.45f;
    material.features.retroreflection             = 0.55f;
    material.features.multiscatterColor           = zenasset::Vec3(0.65f);
    material.features.iridescenceThicknessMax     = 720;
    for (size_t slot = 0; slot < zenasset::MaterialFeatures::textureMembers.size(); ++slot)
    {
        zenasset::MaterialTextureBinding& binding = material.features.*zenasset::MaterialFeatures::textureMembers[slot];
        binding.texture                           = zenasset::TextureId{uint32_t(slot % 2)};
        binding.texCoord                          = 2;
        binding.scale                             = 0.1f + float(slot);
        binding.transform.uvOffset                = zenasset::Vec2(float(slot), -float(slot));
        binding.transform.uvScale                 = zenasset::Vec2(2, 3);
        binding.transform.uvRotation              = 0.25f;
        binding.transform.row0                    = zenasset::Vec4(2, 3, 4, 5);
        binding.transform.row1                    = zenasset::Vec4(6, 7, 8, 9);
    }
    material.textureTransforms[4].uvRotation = 0.75f;
    scene.materialVariants                   = {"default", "alternate"};
    scene.variantPrimitives.push_back({0, 0, {-1, 0}});
    scene.extraVertexAttributes.push_back({0,
                                           0,
                                           "_EXACT_INTEGER",
                                           -1,
                                           1,
                                           5125,
                                           false,
                                           zenasset::VertexAttributeDomain::PrimitiveVertex,
                                           {double(UINT32_MAX), 2, 3}});
    scene.extraVertexAttributes.push_back(
        {0, 0, "_MORPH_CUSTOM", 0, 1, 5126, true, zenasset::VertexAttributeDomain::SourceVertex, {0.1, 0.2, 0.3}});
    scene.cameras.resize(1);
    scene.cameras[0].name        = "camera";
    scene.cameras[0].fixedAspect = true;
    scene.cameras[0].infiniteFar = true;
    scene.cameras[0].aspect      = 1.5f;
    scene.lights.push_back({"spot", zenasset::LightKind::Spot, zenasset::Vec3(0.2f, 0.4f, 0.6f), 12, 20, 0.2f, 0.6f});
    scene.imageBasedLights.resize(1);
    zenasset::ImageBasedLightAsset& environment           = scene.imageBasedLights[0];
    environment.name                                      = "HDR";
    std::shared_ptr<zenasset::EnvironmentCubePixels> cube = std::make_shared<zenasset::EnvironmentCubePixels>();
    cube->size = cube->mipLevels = 1;
    environment.intensity        = 2;
    environment.irradianceCoefficients.resize(9, zenasset::Vec3(0.5f));
    cube->rgba32FloatMipFaces.resize(6);
    for (size_t face = 0; face < 6; ++face)
    {
        cube->rgba32FloatMipFaces[face] = {zenasset::Vec4(float(face) + 10)};
    }
    environment.specular  = cube;
    scene.imageBasedLight = 0;
    scene.animations.resize(1);
    scene.animations[0].name                    = "typed UV and morph animation";
    scene.animations[0].baked                   = true;
    scene.animations[0].bakeSettings.sampleRate = 24;
    scene.animations[0].bakeSettings.maxValues  = 120;
    scene.animations[0].samplers.push_back({zenasset::AnimationInterpolation::Step, 1, {0, 1}, {0.2f, 0.8f}});
    scene.animations[0].samplers.push_back(
        {zenasset::AnimationInterpolation::CubicSpline, 2, {0, 1}, {0, 0, 0.1f, 0.2f, 0, 0, 0, 0, 0.3f, 0.4f, 0, 0}});
    scene.animations[0].channels.push_back(
        {0,
         0,
         zenasset::AnimationPath::Property,
         {zenasset::AnimatedObject::Node, 0, zenasset::AnimatedProperty::Weights, 1, 0, UINT32_MAX},
         "/nodes/0/weights/0"});
    scene.animations[0].channels.push_back(
        {1,
         0,
         zenasset::AnimationPath::Property,
         {zenasset::AnimatedObject::Material, 0, zenasset::AnimatedProperty::TextureOffset, 2, UINT32_MAX, 5},
         "/materials/0/specularTexture/offset"});
    return scene;
}
} // namespace

TEST(ProcessedSceneAdapter, PreservesHierarchyDeformationMaterialsAnimationAndEnvironment)
{
    const zenasset::Scene          source = RichProcessedScene();
    const asset::SceneImportResult result = asset::AdaptProcessedScene(source);
    ASSERT_TRUE(result.asset) << result.error;
    EXPECT_EQ(result.status, asset::SceneImportStatus::Success);
    const asset::SceneAsset& actual = *result.asset;
    EXPECT_EQ(actual.name, source.name);
    EXPECT_EQ(actual.sourceDocument, source.sourceDocument);
    ASSERT_EQ(actual.dependencies.size(), 2u);
    EXPECT_FALSE(actual.dependencies[0].exists);
    EXPECT_EQ(actual.dependencies[1].modified, 1234);
    EXPECT_EQ(actual.dependencies[1].contentHash, 5678u);
    ASSERT_EQ(actual.nodes.size(), 2u);
    EXPECT_EQ(actual.nodes[0].children[0], asset::NodeId{1});
    EXPECT_EQ(actual.nodes[1].parent, asset::NodeId{0});
    EXPECT_EQ(actual.nodes[0].materials[0], asset::MaterialId{0});
    EXPECT_EQ(actual.nodes[0].translation, Vec3(2, 3, 4));
    EXPECT_EQ(actual.nodes[0].rotation, Quat(0.5f, 0.5f, 0.5f, 0.5f));
    EXPECT_EQ(actual.nodes[0].baseMatrix[1][0], 0.125f);
    EXPECT_TRUE(actual.nodes[0].matrixTransform);
    EXPECT_FALSE(actual.nodes[0].selectable);
    EXPECT_FALSE(actual.nodes[0].hoverable);
    EXPECT_FALSE(actual.nodes[1].visible);
    EXPECT_FALSE(actual.nodes[1].renderMesh);
    EXPECT_EQ(actual.nodes[1].morphWeightsSource, asset::NodeId{0});
    EXPECT_EQ(actual.vertices[0].tangent.w, -1);
    EXPECT_EQ(actual.vertexTexCoordSets[2], 7u);
    EXPECT_EQ(actual.vertexTexCoords[2][2], Vec2(0.6f));
    EXPECT_EQ(actual.meshes[0].primitives[0].sourceVertices[0], 2u);
    EXPECT_EQ(actual.meshes[0].primitives[0].influences[1][1].weight, 0.75f);
    EXPECT_TRUE(actual.meshes[0].primitives[0].flatNormals);
    EXPECT_EQ(actual.skins[0].inverseBindMatrices[0][3][0], -2);
    EXPECT_EQ(actual.morphPrimitives[0].primitive, UINT32_MAX);
    EXPECT_EQ(actual.morphPrimitives[0].targets[0].texCoords[2][1], Vec2(0.04f));
    EXPECT_EQ(actual.morphPrimitives[0].targets[0].extraColors[0].set, 9u);
    EXPECT_EQ(actual.extraVertexAttributes[0].values[0], double(UINT32_MAX));
    EXPECT_EQ(actual.extraVertexAttributes[1].domain, asset::VertexAttributeDomain::SourceVertex);
    EXPECT_EQ(actual.extraVertexAttributes[1].morphTarget, 0);
    EXPECT_TRUE(actual.extraVertexAttributes[1].normalized);
    EXPECT_EQ(actual.variantPrimitives[0].materials[0], -1);
    EXPECT_EQ(actual.materialVariants[1], "alternate");
    EXPECT_EQ(actual.materials[0].features.diffuseTransmission, 0.45f);
    EXPECT_EQ(actual.materials[0].features.retroreflection, 0.55f);
    EXPECT_EQ(actual.materials[0].features.multiscatterColor, Vec3(0.65f));
    EXPECT_EQ(actual.materials[0].features.iridescenceThicknessMax, 720);
    EXPECT_EQ(actual.materials[0].textureTransforms[4].uvRotation, 0.75f);
    EXPECT_TRUE(actual.materials[0].unlit);
    EXPECT_FALSE(actual.materials[0].pbrWorkflows.metallicRoughness);
    EXPECT_TRUE(actual.materials[0].pbrWorkflows.specularGlossiness);
    for (size_t slot = 0; slot < asset::MaterialFeatures::textureMembers.size(); ++slot)
    {
        const asset::MaterialTextureBinding& binding =
            actual.materials[0].features.*asset::MaterialFeatures::textureMembers[slot];
        EXPECT_EQ(binding.texture, asset::TextureId{uint32_t(slot % 2)});
        EXPECT_EQ(binding.texCoord, 2u);
        EXPECT_EQ(binding.scale, 0.1f + float(slot));
        EXPECT_EQ(binding.transform.row0, Vec4(2, 3, 4, 5));
        EXPECT_EQ(binding.transform.row1, Vec4(6, 7, 8, 9));
        EXPECT_EQ(binding.transform.uvOffset, Vec2(float(slot), -float(slot)));
        EXPECT_EQ(binding.transform.uvScale, Vec2(2, 3));
        EXPECT_EQ(binding.transform.uvRotation, 0.25f);
    }
    EXPECT_EQ(actual.samplers[0].wrapS, asset::SamplerAddressMode::MirroredRepeat);
    EXPECT_EQ(actual.samplers[0].minFilter, asset::TextureFilter::Nearest);
    EXPECT_EQ(actual.samplers[0].mipFilter, asset::TextureFilter::Linear);
    EXPECT_EQ(actual.textures[0].format, asset::Format::R8G8B8A8_SRGB);
    EXPECT_EQ(actual.textures[1].format, asset::Format::R8G8B8A8_UNORM);
    EXPECT_EQ(actual.textures[0].linearSource, asset::TextureId{1});
    EXPECT_TRUE(actual.textures[1].flatNormal);
    EXPECT_EQ(actual.cameras[0].aspect, 1.5f);
    EXPECT_EQ(actual.lights[0].kind, asset::LightKind::Spot);
    EXPECT_EQ(actual.lights[0].intensity, 12);
    EXPECT_EQ(actual.imageBasedLight, 0);
    EXPECT_EQ(actual.imageBasedLights[0].specularMipFaces[5][0], Vec4(15));
    EXPECT_EQ(actual.animations[0].bakeSettings.sampleRate, 24);
    EXPECT_EQ(actual.animations[0].bakeSettings.maxValues, 120u);
    EXPECT_EQ(actual.animations[0].channels[0].target.element, 0u);
    EXPECT_EQ(actual.animations[0].channels[1].target.textureSlot, 5u);
    EXPECT_EQ(actual.animations[0].channels[1].target.property, asset::AnimatedProperty::TextureOffset);
    EXPECT_EQ(actual.animations[0].samplers[1].interpolation, asset::AnimationInterpolation::CubicSpline);
    EXPECT_EQ(actual.animations[0].samplers[1].values[8], 0.3f);
}

TEST(ProcessedSceneAdapter, OwnsPixelsAndMipDataAfterTheSourceIsDestroyed)
{
    for (const bool legacyDuplicate : {false, true})
    {
        asset::SceneImportResult result;
        {
            zenasset::Scene                        source = RichProcessedScene();
            std::shared_ptr<zenasset::ImagePixels> pixels = std::make_shared<zenasset::ImagePixels>(*source.images[0].pixels);
            if (!legacyDuplicate)
            {
                pixels->bytes.clear();
            }
            source.images[0].pixels = pixels;
            result                  = asset::AdaptProcessedScene(source);
            ASSERT_TRUE(result.asset) << result.error;
            EXPECT_TRUE(result.asset->images[0].pixels->bytes.empty());
            EXPECT_NE(asset::GetImageBaseBytes(*result.asset->images[0].pixels).data(),
                      zenasset::GetImageBaseBytes(*pixels).data());
        }
        EXPECT_EQ(asset::GetImageBaseBytes(*result.asset->images[0].pixels)[15], 127);
        EXPECT_EQ(result.asset->images[0].pixels->mips[1][3], 4);
        EXPECT_EQ(result.asset->animations[0].channels[1].sourceTarget, "/materials/0/specularTexture/offset");
        const asset::SceneLoadResult runtime = asset::BuildSceneRuntime(result.asset);
        ASSERT_TRUE(runtime.scene) << runtime.error;
        const HeapVector<sg::Texture*> textures = runtime.scene->scene->GetComponents<sg::Texture>();
        ASSERT_FALSE(textures.empty());
        ASSERT_EQ(textures[0]->GetBytes().size(), 16u);
        EXPECT_EQ(textures[0]->GetBytes()[15], 127);
    }
}

TEST(ProcessedSceneAdapter, RejectsUnsupportedEncodedImagesAndViewsWithoutPublishing)
{
    zenasset::Scene                        source  = RichProcessedScene();
    std::shared_ptr<zenasset::ImagePixels> encoded = std::make_shared<zenasset::ImagePixels>(*source.images[0].pixels);
    encoded->format                                = zenasset::PixelFormat::BC6HUfloat;
    source.images[0].pixels                        = encoded;
    asset::SceneImportResult result                = asset::AdaptProcessedScene(source);
    EXPECT_EQ(result.status, asset::SceneImportStatus::UnsupportedFeature);
    EXPECT_FALSE(result.asset);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_FALSE(result.diagnostics[0].message.empty());
    encoded->format           = zenasset::PixelFormat::RGBA8Unorm;
    source.textures[0].format = zenasset::PixelFormat::BC5Unorm;
    result                    = asset::AdaptProcessedScene(source);
    EXPECT_EQ(result.status, asset::SceneImportStatus::InvalidData);
    EXPECT_FALSE(result.asset);
}

TEST(ProcessedSceneAdapter, ArrayPagesPreserveOwnersLayersAndLogicalMipSpans)
{
    for (zenasset::PixelFormat format : {zenasset::PixelFormat::RGBA8Unorm, zenasset::PixelFormat::BC7Unorm,
                                         zenasset::PixelFormat::BC4Unorm, zenasset::PixelFormat::BC5Unorm})
    {
        zenasset::Scene                        source = RichProcessedScene();
        std::shared_ptr<zenasset::ImagePixels> pixels = std::make_shared<zenasset::ImagePixels>();
        pixels->width = pixels->height = 2;
        pixels->arrayLayers            = 2;
        pixels->format                 = format;
        for (uint32_t level = 0; level < 2; ++level)
        {
            uint64_t bytes = 0;
            ASSERT_TRUE(zenasset::GetImageLevelByteSize(format, 2 >> level, 2 >> level, bytes));
            pixels->mips.emplace_back(static_cast<size_t>(bytes) * 2, 127);
            std::fill(pixels->mips.back().begin() + bytes, pixels->mips.back().end(), 128);
            if (format == zenasset::PixelFormat::BC7Unorm)
            {
                pixels->mips.back()[0] = pixels->mips.back()[bytes] = 0x40;
            }
        }
        pixels->bytes           = pixels->mips[0];
        source.images[0].pixels = pixels;
        for (uint32_t index = 0; index < source.textures.size(); ++index)
        {
            source.textures[index].format         = format;
            source.textures[index].arrayLayer     = index % 2;
            source.textures[index].interpretation = format == zenasset::PixelFormat::BC5Unorm
                                                      ? zenasset::TextureInterpretation::NormalXYPositiveZ
                                                      : zenasset::TextureInterpretation::Default;
        }
        const asset::SceneImportResult adapted = asset::AdaptProcessedScene(source);
        ASSERT_TRUE(adapted.asset) << adapted.error;
        EXPECT_EQ(adapted.asset->images[0].pixels->arrayLayers, 2u);
        EXPECT_NE(asset::GetImageBaseBytes(*adapted.asset->images[0].pixels).data(), pixels->bytes.data());
        const asset::SceneLoadResult runtime = asset::BuildSceneRuntime(adapted.asset);
        ASSERT_TRUE(runtime.scene) << runtime.error;
        const HeapVector<sg::Texture*> textures = runtime.scene->scene->GetComponents<sg::Texture>();
        ASSERT_GE(textures.size(), 2u);
        EXPECT_EQ(textures[0]->pixels.Get(), textures[1]->pixels.Get());
        for (uint32_t index = 0; index < 2; ++index)
        {
            EXPECT_EQ(textures[index]->GetArrayLayer(), index);
            EXPECT_EQ(textures[index]->GetArrayLayers(), 2u);
            for (uint32_t level = 0; level < 2; ++level)
            {
                const VectorView<const uint8_t> bytes = textures[index]->GetLayerMipBytes(level);
                ASSERT_EQ(bytes.size(), pixels->mips[level].size() / 2);
                EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), pixels->mips[level].begin() + bytes.size() * index));
            }
            EXPECT_TRUE(textures[index]->GetLayerMipBytes(2).empty());
        }
        source.textures[0].arrayLayer = 2;
        EXPECT_EQ(asset::AdaptProcessedScene(source).status, asset::SceneImportStatus::InvalidData);
        source.textures[0].arrayLayer = 0;
        pixels->mips.clear();
        EXPECT_EQ(asset::AdaptProcessedScene(source).status, asset::SceneImportStatus::InvalidData);
        pixels->arrayLayers = 0;
        EXPECT_EQ(asset::AdaptProcessedScene(source).status, asset::SceneImportStatus::InvalidData);
    }
}

TEST(ProcessedSceneAdapter, LegacyWriterRejectsArrayPagesWithoutReplacingDestination)
{
    zenasset::Scene                source   = RichProcessedScene();
    const asset::SceneImportResult original = asset::AdaptProcessedScene(source);
    ASSERT_TRUE(original.asset);
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "zenasset_legacy_array_pages.zenasset";
    ASSERT_EQ(asset::WriteSceneAssetFile(path.string(), *original.asset).status, asset::SceneImportStatus::Success);
    std::shared_ptr<zenasset::ImagePixels> pixels = std::make_shared<zenasset::ImagePixels>(*source.images[0].pixels);
    pixels->arrayLayers                           = 2;
    for (std::vector<uint8_t>& mip : pixels->mips)
    {
        const HeapVector<uint8_t> layer(mip.data(), mip.size());
        mip.insert(mip.end(), layer.begin(), layer.end());
    }
    pixels->bytes           = pixels->mips[0];
    source.images[0].pixels = pixels;
    for (uint32_t layer = 0; layer < 2; ++layer)
    {
        source.textures[0].arrayLayer          = layer;
        const asset::SceneImportResult adapted = asset::AdaptProcessedScene(source);
        ASSERT_TRUE(adapted.asset) << adapted.error;
        EXPECT_EQ(asset::WriteSceneAssetFile(path.string(), *adapted.asset).status,
                  asset::SceneImportStatus::UnsupportedFeature);
        const asset::SceneAssetFileResult retained = asset::ReadSceneAssetFile(path.string());
        ASSERT_TRUE(retained.asset) << retained.error;
        EXPECT_EQ(retained.asset->images[0].pixels->arrayLayers, 1u);
        EXPECT_EQ(retained.asset->textures[0].arrayLayer, 0u);
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST(ProcessedSceneAdapter, BC7MipsPreserveOwningStorageAndCompatibleColorViews)
{
    zenasset::Scene                        source = RichProcessedScene();
    std::shared_ptr<zenasset::ImagePixels> pixels = std::make_shared<zenasset::ImagePixels>();
    pixels->width                                 = 5;
    pixels->height                                = 7;
    pixels->format                                = zenasset::PixelFormat::BC7Unorm;
    pixels->bytes.resize(64, 0x40);
    pixels->mips                          = {pixels->bytes, std::vector<uint8_t>(16, 1), std::vector<uint8_t>(16, 2)};
    source.images[0].pixels               = pixels;
    source.textures[0].format             = zenasset::PixelFormat::BC7Srgb;
    source.textures[1].format             = zenasset::PixelFormat::BC7Unorm;
    const asset::SceneImportResult result = asset::AdaptProcessedScene(source);
    ASSERT_TRUE(result.asset) << result.error;
    EXPECT_EQ(result.asset->images[0].pixels->format, asset::Format::BC7_UNORM_BLOCK);
    EXPECT_EQ(result.asset->textures[0].format, asset::Format::BC7_SRGB_BLOCK);
    EXPECT_EQ(result.asset->images[0].pixels->mips.size(), 3u);
    EXPECT_EQ(result.asset->images[0].pixels->mips[2].size(), 16u);
    EXPECT_NE(asset::GetImageBaseBytes(*result.asset->images[0].pixels).data(), pixels->bytes.data());
    for (uint32_t scenario = 0; scenario < 3; ++scenario)
    {
        std::shared_ptr<zenasset::ImagePixels> invalid = std::make_shared<zenasset::ImagePixels>(*pixels);
        source.images[0].pixels                        = invalid;
        if (scenario == 0)
        {
            invalid->mips[2].pop_back();
        }
        else if (scenario == 1)
        {
            invalid->mips[0][0] ^= 1;
        }
        else
        {
            source.textures[0].format = zenasset::PixelFormat::RGBA8Srgb;
        }
        const asset::SceneImportResult rejected = asset::AdaptProcessedScene(source);
        EXPECT_EQ(rejected.status, asset::SceneImportStatus::InvalidData);
        EXPECT_FALSE(rejected.asset);
    }
}

TEST(ProcessedSceneAdapter, BC4AndBC5PreserveMipsInterpretationAndRuntimeMaterialBindings)
{
    for (const bool normal : {false, true})
    {
        zenasset::Scene                        source     = RichProcessedScene();
        std::shared_ptr<zenasset::ImagePixels> pixels     = std::make_shared<zenasset::ImagePixels>();
        const uint32_t                         blockBytes = normal ? 16u : 8u;
        pixels->width                                     = 5;
        pixels->height                                    = 7;
        pixels->format = normal ? zenasset::PixelFormat::BC5Unorm : zenasset::PixelFormat::BC4Unorm;
        pixels->bytes.resize(4 * blockBytes, 127);
        pixels->mips            = {pixels->bytes, std::vector<uint8_t>(blockBytes, 128), std::vector<uint8_t>(blockBytes, 129)};
        source.images[0].pixels = pixels;
        for (zenasset::TextureAsset& texture : source.textures)
        {
            texture.format     = pixels->format;
            texture.flatNormal = false;
            texture.interpretation =
                normal ? zenasset::TextureInterpretation::NormalXYPositiveZ : zenasset::TextureInterpretation::Default;
        }
        const asset::SceneImportResult adapted = asset::AdaptProcessedScene(source);
        ASSERT_TRUE(adapted.asset) << adapted.error;
        EXPECT_EQ(adapted.asset->textures[0].format, normal ? asset::Format::BC5_UNORM_BLOCK : asset::Format::BC4_UNORM_BLOCK);
        EXPECT_EQ(adapted.asset->textures[0].interpretation,
                  normal ? asset::TextureInterpretation::NormalXYPositiveZ : asset::TextureInterpretation::Default);
        EXPECT_NE(asset::GetImageBaseBytes(*adapted.asset->images[0].pixels).data(), pixels->bytes.data());
        EXPECT_EQ(adapted.asset->images[0].pixels->mips[2].size(), blockBytes);
        const asset::SceneLoadResult runtime = asset::BuildSceneRuntime(adapted.asset);
        ASSERT_TRUE(runtime.scene) << runtime.error;
        const HeapVector<sg::Texture*> textures = runtime.scene->scene->GetComponents<sg::Texture>();
        ASSERT_GE(textures.size(), 2u);
        EXPECT_EQ(textures[0]->interpretation, adapted.asset->textures[0].interpretation);
        EXPECT_EQ(textures[0]->GetMipBytes()[2].size(), blockBytes);
        const HeapVector<sg::Material*> materials = runtime.scene->scene->GetComponents<sg::Material>();
        ASSERT_FALSE(materials.empty());
        EXPECT_EQ(materials[0]->data.normalEncoding, normal ? glm::uvec4(1, 1, 0, 0) : glm::uvec4(0));
        EXPECT_EQ(materials[0]->data.featureTextures[6].transform.row0.w, 1.0f);
        source.textures[0].interpretation =
            normal ? zenasset::TextureInterpretation::Default : zenasset::TextureInterpretation::NormalXYPositiveZ;
        EXPECT_FALSE(asset::AdaptProcessedScene(source).asset);
        source.textures[0].interpretation = static_cast<zenasset::TextureInterpretation>(99);
        EXPECT_FALSE(asset::AdaptProcessedScene(source).asset);
    }
}

TEST(ProcessedSceneAdapter, LegacyWriterRejectsNormalInterpretationWithoutReplacingDestination)
{
    zenasset::Scene                source   = RichProcessedScene();
    const asset::SceneImportResult original = asset::AdaptProcessedScene(source);
    ASSERT_TRUE(original.asset);
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "zenasset_legacy_normal_interpretation.zenasset";
    ASSERT_EQ(asset::WriteSceneAssetFile(path.string(), *original.asset).status, asset::SceneImportStatus::Success);
    std::shared_ptr<zenasset::ImagePixels> pixels = std::make_shared<zenasset::ImagePixels>();
    pixels->width = pixels->height = 1;
    pixels->format                 = zenasset::PixelFormat::BC5Unorm;
    pixels->bytes.resize(16, 127);
    source.images[0].pixels = pixels;
    for (zenasset::TextureAsset& texture : source.textures)
    {
        texture.format         = zenasset::PixelFormat::BC5Unorm;
        texture.interpretation = zenasset::TextureInterpretation::NormalXYPositiveZ;
    }
    const asset::SceneImportResult adapted = asset::AdaptProcessedScene(source);
    ASSERT_TRUE(adapted.asset) << adapted.error;
    EXPECT_EQ(asset::WriteSceneAssetFile(path.string(), *adapted.asset).status, asset::SceneImportStatus::UnsupportedFeature);
    const asset::SceneAssetFileResult retained = asset::ReadSceneAssetFile(path.string());
    ASSERT_TRUE(retained.asset) << retained.error;
    EXPECT_EQ(retained.asset->textures[0].format, asset::Format::R8G8B8A8_SRGB);
    EXPECT_EQ(retained.asset->textures[0].interpretation, asset::TextureInterpretation::Default);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST(ProcessedSceneAdapter, BC6HEnvironmentPreservesAuthoredFacesAndSharesRuntimeBlocks)
{
    zenasset::Scene                                  source = RichProcessedScene();
    std::shared_ptr<zenasset::EnvironmentCubePixels> cube   = std::make_shared<zenasset::EnvironmentCubePixels>();
    cube->format                                            = zenasset::PixelFormat::BC6HUfloat;
    cube->size                                              = 5;
    cube->mipLevels                                         = 3;
    for (uint32_t face = 0; face < 18; ++face)
    {
        cube->blockMipFaces.emplace_back(face < 6 ? 64 : 16, 0);
        cube->blockMipFaces.back()[1] = static_cast<uint8_t>(face + 1);
    }
    source.imageBasedLights[0].specular    = cube;
    const asset::SceneImportResult adapted = asset::AdaptProcessedScene(source);
    ASSERT_TRUE(adapted.asset) << adapted.error;
    const asset::ImageBasedLightAsset& light = adapted.asset->imageBasedLights[0];
    EXPECT_EQ(light.specularFormat, asset::Format::BC6H_UFLOAT_BLOCK);
    EXPECT_EQ(light.size, 5u);
    EXPECT_EQ(light.mipLevels, 3u);
    EXPECT_TRUE(light.specularMipFaces.empty());
    ASSERT_TRUE(light.specularBlockMipFaces);
    EXPECT_NE((*light.specularBlockMipFaces)[0].data(), cube->blockMipFaces[0].data());
    EXPECT_EQ((*light.specularBlockMipFaces)[17][1], 18u);
    EXPECT_EQ(light.irradianceCoefficients[8], Vec3(.5f));
    const asset::SceneLoadResult runtime = asset::BuildSceneRuntime(adapted.asset);
    ASSERT_TRUE(runtime.scene) << runtime.error;
    EXPECT_EQ(runtime.scene->scene->GetAssetData().imageBasedLights[0].specularBlockMipFaces.Get(),
              light.specularBlockMipFaces.Get());
    const std::filesystem::path    path = std::filesystem::temp_directory_path() / "zenasset_legacy_hdr_environment.zenasset";
    const asset::SceneImportResult original = asset::AdaptProcessedScene(RichProcessedScene());
    ASSERT_TRUE(original.asset);
    ASSERT_EQ(asset::WriteSceneAssetFile(path.string(), *original.asset).status, asset::SceneImportStatus::Success);
    EXPECT_EQ(asset::WriteSceneAssetFile(path.string(), *adapted.asset).status, asset::SceneImportStatus::UnsupportedFeature);
    const asset::SceneAssetFileResult retained = asset::ReadSceneAssetFile(path.string());
    ASSERT_TRUE(retained.asset) << retained.error;
    EXPECT_EQ(retained.asset->imageBasedLights[0].specularFormat, asset::Format::R32G32B32A32_SFLOAT);
    EXPECT_EQ(retained.asset->imageBasedLights[0].specularMipFaces[5][0], Vec4(15));
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    for (uint32_t scenario = 0; scenario < 7; ++scenario)
    {
        std::shared_ptr<zenasset::EnvironmentCubePixels> invalid = std::make_shared<zenasset::EnvironmentCubePixels>(*cube);
        source.imageBasedLights[0].specular                      = invalid;
        if (scenario == 0)
        {
            invalid->blockMipFaces[0].pop_back();
        }
        else if (scenario == 1)
        {
            invalid->blockMipFaces[17][0] = 31;
        }
        else if (scenario == 2)
        {
            invalid->blockMipFaces[0][16] = 19;
        }
        else if (scenario == 3)
        {
            invalid->rgba32FloatMipFaces.resize(18);
        }
        else if (scenario == 4)
        {
            invalid->blockMipFaces.pop_back();
        }
        else if (scenario == 5)
        {
            invalid->format = zenasset::PixelFormat::BC6HSfloat;
        }
        else
        {
            source.imageBasedLights[0].specular.reset();
        }
        const asset::SceneImportResult rejected = asset::AdaptProcessedScene(source);
        EXPECT_EQ(rejected.status, asset::SceneImportStatus::InvalidData);
        EXPECT_FALSE(rejected.asset);
    }
    source.imageBasedLights[0].specular = cube;
    cube->mipLevels                     = 2;
    cube->blockMipFaces.resize(12);
    const asset::SceneImportResult partial = asset::AdaptProcessedScene(source);
    ASSERT_TRUE(partial.asset) << partial.error;
    EXPECT_EQ(partial.asset->imageBasedLights[0].mipLevels, 2u);
}

TEST(ProcessedSceneAdapter, RejectsInvalidReferencesEnumsValuesAndImageOwnership)
{
    zenasset::Scene source          = RichProcessedScene();
    source.nodes[0].mesh            = zenasset::MeshId{27};
    asset::SceneImportResult result = asset::AdaptProcessedScene(source);
    EXPECT_EQ(result.status, asset::SceneImportStatus::InvalidData);
    EXPECT_FALSE(result.asset);
    source.nodes[0].mesh          = zenasset::MeshId{0};
    source.materials[0].alphaMode = static_cast<zenasset::AlphaMode>(99);
    result                        = asset::AdaptProcessedScene(source);
    EXPECT_EQ(result.status, asset::SceneImportStatus::InvalidData);
    EXPECT_FALSE(result.asset);
    source.materials[0].alphaMode = zenasset::AlphaMode::Opaque;
    source.vertices[0].pos.x      = std::numeric_limits<float>::quiet_NaN();
    result                        = asset::AdaptProcessedScene(source);
    EXPECT_EQ(result.status, asset::SceneImportStatus::InvalidData);
    EXPECT_FALSE(result.asset);
    source.vertices[0].pos.x = 0;
    source.images[0].pixels.reset();
    result = asset::AdaptProcessedScene(source);
    EXPECT_EQ(result.status, asset::SceneImportStatus::InvalidData);
    EXPECT_FALSE(result.asset);
}

namespace
{
zenasset::Scene SpatialProcessedScene()
{
    zenasset::Scene                        source = RichProcessedScene();
    std::shared_ptr<zenasset::ImagePixels> pixels = std::make_shared<zenasset::ImagePixels>();
    pixels->width = pixels->height = 16;
    pixels->bytes.resize(16 * 16 * 4, 127);
    pixels->mips.push_back(pixels->bytes);
    source.images[0].pixels      = pixels;
    source.samplers[0].minFilter = source.samplers[0].magFilter = zenasset::TextureFilter::Linear;
    source.samplers[0].wrapS = source.samplers[0].wrapT = zenasset::SamplerAddressMode::ClampToEdge;
    source.textures[0].atlasRegion                      = {4, 4, 4, 8};
    source.textures[1].atlasRegion                      = {8, 4, 4, 8};
    source.textures[1].sampler                          = zenasset::SamplerId{0};
    return source;
}
} // namespace

TEST(ProcessedSceneAdapter, SpatialRegionsPreserveSharedOwnersAndIndependentRuntimeViews)
{
    asset::SceneImportResult adapted;
    {
        const zenasset::Scene source = SpatialProcessedScene();
        ASSERT_TRUE(zenasset::ValidateSceneAsset(source));
        adapted = asset::AdaptProcessedScene(source);
        ASSERT_TRUE(adapted.asset) << adapted.error;
        EXPECT_NE(asset::GetImageBaseBytes(*adapted.asset->images[0].pixels).data(), source.images[0].pixels->bytes.data());
        EXPECT_EQ(adapted.asset->materials[0].textureTransforms[0].row0.x, source.materials[0].textureTransforms[0].row0.x);
        EXPECT_EQ(adapted.asset->materials[0].textureTransforms[0].row1.z, source.materials[0].textureTransforms[0].row1.z);
    }
    EXPECT_EQ(adapted.asset->textures[0].atlasRegion, (asset::ImageRegion{4, 4, 4, 8}));
    EXPECT_EQ(adapted.asset->textures[1].atlasRegion, (asset::ImageRegion{8, 4, 4, 8}));
    EXPECT_EQ(adapted.asset->textures[0].linearSource, asset::TextureId{1});
    const asset::SceneLoadResult first  = asset::BuildSceneRuntime(adapted.asset);
    const asset::SceneLoadResult second = asset::BuildSceneRuntime(adapted.asset);
    ASSERT_TRUE(first.scene) << first.error;
    ASSERT_TRUE(second.scene) << second.error;
    const HeapVector<sg::Texture*> a = first.scene->scene->GetComponents<sg::Texture>();
    const HeapVector<sg::Texture*> b = second.scene->scene->GetComponents<sg::Texture>();
    ASSERT_GE(a.size(), 2u);
    ASSERT_EQ(a.size(), b.size());
    for (uint32_t index = 0; index < 2; ++index)
    {
        EXPECT_EQ(a[index]->GetAtlasRegion(), adapted.asset->textures[index].atlasRegion);
        EXPECT_EQ(a[index]->pixels.Get(), a[0]->pixels.Get());
        EXPECT_EQ(a[index]->pixels.Get(), b[index]->pixels.Get());
        EXPECT_EQ(a[index]->width, 16u);
        EXPECT_EQ(a[index]->height, 16u);
        EXPECT_EQ(a[index]->GetLayerMipBytes(0).size(), 16u * 16u * 4u);
    }
    a[0]->atlasRegion = a[1]->atlasRegion;
    EXPECT_EQ(b[0]->GetAtlasRegion(), adapted.asset->textures[0].atlasRegion);
    EXPECT_NE(a[0]->GetAtlasRegion(), b[0]->GetAtlasRegion());
}

TEST(ProcessedSceneAdapter, SpatialRegionValidationRejectsMalformedBoundsAndSamplingPolicy)
{
    for (const zenasset::ImageRegion region : {zenasset::ImageRegion{0, 0, 0, 8},
                                               {UINT32_MAX, 4, 4, 8},
                                               {4, 4, UINT32_MAX, 8},
                                               {3, 4, 4, 8},
                                               {4, 4, 3, 8},
                                               {12, 4, 4, 8}})
    {
        zenasset::Scene                source   = SpatialProcessedScene();
        const asset::SceneImportResult original = asset::AdaptProcessedScene(source);
        ASSERT_TRUE(original.asset);
        source.textures[0].atlasRegion          = region;
        const asset::SceneImportResult rejected = asset::AdaptProcessedScene(source);
        EXPECT_EQ(rejected.status, asset::SceneImportStatus::InvalidData);
        EXPECT_FALSE(rejected.asset);
        asset::SceneAsset changed       = *original.asset;
        changed.textures[0].atlasRegion = {region.x, region.y, region.width, region.height};
        EXPECT_FALSE(asset::ValidateSceneAsset(changed));
    }
    for (uint32_t scenario = 0; scenario < 6; ++scenario)
    {
        zenasset::Scene                        source = SpatialProcessedScene();
        std::shared_ptr<zenasset::ImagePixels> pixels = std::make_shared<zenasset::ImagePixels>(*source.images[0].pixels);
        source.images[0].pixels                       = pixels;
        if (scenario == 0)
        {
            source.textures[0].sampler = {};
        }
        else if (scenario == 1)
        {
            source.samplers[0].wrapT = zenasset::SamplerAddressMode::Repeat;
        }
        else if (scenario == 2)
        {
            source.samplers[0].minFilter = zenasset::TextureFilter::Nearest;
        }
        else if (scenario == 3)
        {
            pixels->mips.clear();
        }
        else if (scenario == 4)
        {
            pixels->mips.emplace_back(8 * 8 * 4, 127);
        }
        else
        {
            pixels->arrayLayers = 2;
            pixels->bytes.resize(2 * 16 * 16 * 4, 127);
            pixels->mips[0] = pixels->bytes;
        }
        const asset::SceneImportResult rejected = asset::AdaptProcessedScene(source);
        EXPECT_EQ(rejected.status, asset::SceneImportStatus::InvalidData) << scenario;
        EXPECT_FALSE(rejected.asset);
    }
}

TEST(ProcessedSceneAdapter, SpatialPackagesPreserveRegionsThroughReadAndRuntimeConstruction)
{
    const zenasset::Scene       source = SpatialProcessedScene();
    const std::filesystem::path path   = std::filesystem::temp_directory_path() / "zenasset_spatial_runtime.zenasset";
    for (zenasset::TransportCodec transport : {zenasset::TransportCodec::None, zenasset::TransportCodec::Zstandard})
    {
        zenasset::SceneWriteOptions write;
        write.compression.transport                     = transport;
        write.metadata.processing.textures.spatialPages = true;
        ASSERT_TRUE(zenasset::WriteScene(path.string(), source, write).Succeeded());
        const asset::SceneAssetFileResult read = asset::ReadSceneAssetFile(path.string());
        ASSERT_TRUE(read.asset) << read.error;
        EXPECT_EQ(read.metadata.processing, write.metadata.processing);
        EXPECT_EQ(read.asset->textures[0].atlasRegion, (asset::ImageRegion{4, 4, 4, 8}));
        EXPECT_EQ(read.asset->textures[1].atlasRegion, (asset::ImageRegion{8, 4, 4, 8}));
        const asset::SceneLoadResult loaded = asset::LoadScene(path.string());
        ASSERT_TRUE(loaded.scene) << loaded.error;
        const HeapVector<sg::Texture*> textures = loaded.scene->scene->GetComponents<sg::Texture>();
        ASSERT_GE(textures.size(), 2u);
        EXPECT_EQ(textures[0]->GetAtlasRegion(), read.asset->textures[0].atlasRegion);
        EXPECT_EQ(textures[1]->GetAtlasRegion(), read.asset->textures[1].atlasRegion);
        EXPECT_EQ(textures[0]->pixels.Get(), textures[1]->pixels.Get());
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST(ProcessedSceneAdapter, LegacyWriterRejectsSpatialRegionsWithoutReplacingDestination)
{
    zenasset::Scene source = SpatialProcessedScene();
    for (zenasset::TextureAsset& texture : source.textures)
    {
        texture.atlasRegion = {};
    }
    const asset::SceneImportResult original = asset::AdaptProcessedScene(source);
    ASSERT_TRUE(original.asset) << original.error;
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "zenasset_legacy_spatial_regions.zenasset";
    ASSERT_EQ(asset::WriteSceneAssetFile(path.string(), *original.asset).status, asset::SceneImportStatus::Success);
    const asset::SceneImportResult spatial = asset::AdaptProcessedScene(SpatialProcessedScene());
    ASSERT_TRUE(spatial.asset) << spatial.error;
    EXPECT_EQ(asset::WriteSceneAssetFile(path.string(), *spatial.asset).status, asset::SceneImportStatus::UnsupportedFeature);
    const asset::SceneAssetFileResult retained = asset::ReadSceneAssetFile(path.string());
    ASSERT_TRUE(retained.asset) << retained.error;
    EXPECT_EQ(retained.asset->textures[0].atlasRegion, asset::ImageRegion{});
    EXPECT_EQ(retained.asset->textures[1].atlasRegion, asset::ImageRegion{});
    EXPECT_EQ(retained.asset->images[0].pixels->bytes.size(), 16u * 16u * 4u);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST(ProcessedSceneAdapter, HonorsCancellationAndRuntimeGeometryBudgetBeforePublication)
{
    const zenasset::Scene     source = RichProcessedScene();
    asset::SceneImportControl control;
    control.Cancel();
    const asset::SceneImportResult cancelled = asset::AdaptProcessedScene(source, &control);
    EXPECT_EQ(cancelled.status, asset::SceneImportStatus::Cancelled);
    EXPECT_FALSE(cancelled.asset);
    const asset::SceneImportResult limited = asset::AdaptProcessedScene(source, nullptr, 0);
    EXPECT_EQ(limited.status, asset::SceneImportStatus::InvalidData);
    EXPECT_FALSE(limited.asset);
    EXPECT_NE(limited.error.find("budget"), std::string::npos);
}

TEST(ProcessedSceneAdapter, RuntimeDiscoveryAndLoadingDoNotDispatchSourceImporters)
{
    ASSERT_EQ(asset::GetSceneLoadFormats().size(), 2u);
    EXPECT_EQ(asset::GetSceneLoadFormats()[0].id, "zenasset");
    EXPECT_EQ(asset::GetSceneLoadFormats()[1].id, "zenscene");
    EXPECT_EQ(asset::GetSceneFileFilterExtensions(), "zenasset;zenscene");
    EXPECT_EQ(asset::FindSceneLoadFormat("source.GLTF"), nullptr);
    EXPECT_NE(asset::FindSceneLoadFormat("prepared.ZENASSET"), nullptr);
    EXPECT_NE(asset::FindSceneLoadFormat("prepared.ZENSCENE"), nullptr);
    const asset::SceneLoadResult result = asset::LoadScene("source.gltf");
    EXPECT_EQ(result.status, asset::SceneImportStatus::UnsupportedFormat);
    EXPECT_FALSE(result.scene);
    EXPECT_FALSE(result.asset);
}
