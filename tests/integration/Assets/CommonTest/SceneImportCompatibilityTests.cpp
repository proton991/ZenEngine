#include <gtest/gtest.h>
#include <cstddef>
#include <filesystem>
#include "SceneTestLoader.h"
#include "SceneGraph/Scene.h"

using namespace zen;

namespace
{
std::string CompatibilityFixture(const char* name)
{
    const std::string result = (std::filesystem::path(ZEN_ASSET_TEST_CORPUS) / name).string();

    return result;
}

void AttachDefaultTextures(sg::Material& material, sg::Scene& scene)
{
    scene.LoadDefaultTextures(0);

    const sg::Scene::DefaultTextures textures = scene.GetDefaultTextures();

    material.m_pBaseColorTexture              = textures.pBaseColor;

    material.m_pMetallicRoughnessTexture      = textures.pMetallicRoughness;

    material.m_pNormalTexture                 = textures.pNormal;

    material.m_pOcclusionTexture              = textures.pOcclusion;

    material.m_pEmissiveTexture               = textures.pEmissive;
}
} // namespace

TEST(SceneImportCompatibility, RetainsPointsAndEveryLineConnectivityMode)
{
    sg::Scene scene;

    test::SceneLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(CompatibilityFixture("all_primitive_modes.gltf"), &scene)) << loader.GetError();

    const zen::HeapVector<sg::SubMesh*> primitives = scene.GetComponents<sg::SubMesh>();

    ASSERT_EQ(primitives.size(), 7u);

    const sg::MeshTopology topologies[] = {sg::MeshTopology::Points,   sg::MeshTopology::Lines,     sg::MeshTopology::Lines,
                                           sg::MeshTopology::Lines,    sg::MeshTopology::Triangles, sg::MeshTopology::Triangles,
                                           sg::MeshTopology::Triangles};

    const uint32_t expectedCounts[]     = {4, 4, 8, 6, 6, 6, 6};

    const uint32_t expectedLineVertices[][8] = {{0, 1, 2, 3}, {0, 1, 1, 2, 2, 3, 3, 0}, {0, 1, 1, 2, 2, 3}};

    const Vec3 sourcePositions[]             = {Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(1, 1, 0)};

    for (uint32_t primitive = 0; primitive < 7; ++primitive)
    {
        EXPECT_EQ(primitives[primitive]->topology, topologies[primitive]);

        EXPECT_EQ(primitives[primitive]->GetIndexCount(), expectedCounts[primitive]);

        if (primitive >= 1 && primitive <= 3)
        {
            for (uint32_t index = 0; index < expectedCounts[primitive]; ++index)
            {
                const uint32_t vertex = loader.GetIndices()[primitives[primitive]->GetFirstIndex() + index];

                ASSERT_LT(vertex, loader.GetVertices().size());

                EXPECT_EQ(Vec3(loader.GetVertices()[vertex].pos), sourcePositions[expectedLineVertices[primitive - 1][index]]);
            }
        }
    }
}

TEST(SceneImportCompatibility, RejectsCorruptDracoAndPreservesThePreviousScene)
{
    sg::Scene scene;

    test::SceneLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(CompatibilityFixture("all_primitive_modes.gltf"), &scene)) << loader.GetError();

    const size_t vertexCount = loader.GetVertices().size();

    const size_t indexCount  = loader.GetIndices().size();

    const size_t nodeCount   = scene.GetNodes().size();

    EXPECT_FALSE(loader.LoadFromFile(CompatibilityFixture("invalid_compressed_scene.gltf"), &scene));

    EXPECT_EQ(loader.GetVertices().size(), vertexCount);

    EXPECT_EQ(loader.GetIndices().size(), indexCount);

    EXPECT_EQ(scene.GetNodes().size(), nodeCount);

    EXPECT_EQ(scene.GetName(), "all_primitive_modes");
}

TEST(SceneImportCompatibility, PublishesNormalEncodingWithoutChangingSamplerSlots)
{
    sg::Scene    scene;
    sg::Material material("NormalEncoding");
    AttachDefaultTextures(material, scene);
    sg::Texture normal;
    normal.index                                              = 7;
    normal.format                                             = asset::Format::BC5_UNORM_BLOCK;
    normal.interpretation                                     = asset::TextureInterpretation::NormalXYPositiveZ;
    material.m_pNormalTexture                                 = &normal;
    material.features.clearcoatNormalTexture.texture          = &normal;
    material.textureTransforms[2].row0.w                      = 3;
    material.features.clearcoatNormalTexture.transform.row0.w = 5;
    material.SetData();
    EXPECT_EQ(material.data.normalEncoding, glm::uvec4(1, 1, 0, 0));
    EXPECT_EQ(material.data.normalTexIndex, 7);
    EXPECT_EQ(material.data.textureTransforms[2].row0.w, 3);
    EXPECT_EQ(material.data.featureTextures[6].transform.row0.w, 5);
    normal.flatNormal                                = true;
    material.features.clearcoatNormalTexture.texture = nullptr;
    material.SetData();
    EXPECT_EQ(material.data.normalEncoding, glm::uvec4(0));
    EXPECT_EQ(material.data.normalTexIndex, -1);
}

TEST(SceneImportCompatibility, PublishesSpatialRegionsSeparatelyFromAuthoredTransforms)
{
    sg::Scene    scene;
    sg::Material material("SpatialBindings");
    AttachDefaultTextures(material, scene);
    SharedPtr<asset::ImagePixels, MultiThreadCounter> pixels = MakeShared<asset::ImagePixels, MultiThreadCounter>();
    sg::Texture                                       texture;
    texture.pixels = pixels;
    texture.index  = 7;
    texture.width = pixels->width = 32;
    texture.height = pixels->height                  = 64;
    texture.atlasRegion                              = {4, 12, 16, 8};
    material.m_pBaseColorTexture                     = &texture;
    material.m_pNormalTexture                        = &texture;
    material.textureTransforms[0].row0               = Vec4(0.5f, 0.25f, 0.125f, 3);
    material.textureTransforms[0].row1               = Vec4(-0.25f, 0.5f, 0.375f, 9);
    material.features.clearcoatNormalTexture.texture = &texture;
    material.features.diffuseTexture.texture         = &texture;
    material.SetData();
    EXPECT_EQ(material.data.textureTransforms[0].row0, material.textureTransforms[0].row0);
    EXPECT_EQ(Vec3(material.data.textureTransforms[0].row1), Vec3(material.textureTransforms[0].row1));
    EXPECT_EQ(material.textureTransforms[0].row1.w, 9);
    EXPECT_EQ(material.data.textureTransforms[0].row1.w, 197); // width 2^5, height 2^6.
    EXPECT_EQ(material.data.textureTransforms[2].row1.w, 197);
    EXPECT_EQ(material.data.featureTextures[6].transform.row1.w, 197);
    EXPECT_EQ(material.data.textureTransforms[0].atlasRegion, glm::uvec4(4, 12, 16, 8));
    EXPECT_EQ(material.data.textureTransforms[2].atlasRegion, glm::uvec4(4, 12, 16, 8));
    EXPECT_EQ(material.data.featureTextures[6].transform.atlasRegion, glm::uvec4(4, 12, 16, 8));
    material.pbrWorkflows.specularGlossiness = true;
    material.SetData();
    EXPECT_EQ(material.data.textureTransforms[0], material.data.featureTextures[2].transform);
    EXPECT_EQ(material.data.textureTransforms[0].row1.w, 197);
    texture.flatNormal = true;
    material.SetData();
    EXPECT_EQ(material.data.normalTexIndex, -1);
    EXPECT_EQ(material.data.textureTransforms[2].row1.w, 0);
    EXPECT_EQ(material.data.textureTransforms[2].atlasRegion, glm::uvec4(0));
    texture.bytesData.resize(4, 255);
    material.SetData();
    EXPECT_EQ(material.data.textureTransforms[0].atlasRegion, glm::uvec4(0));
    EXPECT_EQ(material.data.featureTextures[6].transform.atlasRegion, glm::uvec4(0));
    EXPECT_EQ(material.data.textureTransforms[0].row1.w, 0);
    EXPECT_EQ(material.data.featureTextures[6].transform.row1.w, 0);
    EXPECT_EQ(material.textureTransforms[0].row1.w, 9);
    EXPECT_EQ(offsetof(sg::MaterialData, textureSampling), 1616u);
}

TEST(SceneImportCompatibility, DistinguishesInvalidSpatialPageExtentsFromOrdinaryTextures)
{
    sg::Texture texture;
    texture.pixels      = MakeShared<asset::ImagePixels, MultiThreadCounter>();
    texture.atlasRegion = {4, 4, 8, 8};
    texture.height      = 64;
    for (uint32_t width : {0u, 31u, UINT32_MAX})
    {
        texture.width = width;
        EXPECT_EQ(sg::PublishTextureTransform({}, &texture).row1.w, -1);
    }
    texture.width  = 64;
    texture.height = 32;
    EXPECT_EQ(sg::PublishTextureTransform({}, &texture).row1.w, 166);
    texture.bytesData.resize(4, 255);
    EXPECT_EQ(sg::PublishTextureTransform({}, &texture).row1.w, 0);
    EXPECT_EQ(sg::PublishTextureTransform({}).row1.w, 0);
}

TEST(SceneImportCompatibility, PublishesAdvancedMaterialFactorsAndUVBindingsToGPUData)
{
    sg::Scene scene;

    sg::Material material("MaterialPublication");

    AttachDefaultTextures(material, scene);

    material.SetData();

    EXPECT_EQ(sizeof(sg::TextureTransformData), 48u);

    EXPECT_EQ(sizeof(sg::MaterialTextureData), 64u);

    EXPECT_EQ(offsetof(sg::MaterialData, specularColorIor), 352u);

    EXPECT_EQ(offsetof(sg::MaterialData, volumeScatterColorRetroreflection), 496u);

    EXPECT_EQ(offsetof(sg::MaterialData, featureTextures), 512u);

    EXPECT_EQ(offsetof(sg::MaterialData, normalEncoding), 1600u);
    EXPECT_EQ(sizeof(sg::MaterialData), 1632u);

    EXPECT_FLOAT_EQ(material.data.materialProperties.w, 0.0f);

    for (const sg::MaterialTextureData& texture : material.data.featureTextures)
    {
        EXPECT_FLOAT_EQ(texture.properties.x, -1.0f);
    }

    material.alphaMode                                      = sg::AlphaMode::Blend;

    material.features.ior                                   = 1.8f;

    material.features.specularColor                         = Vec3(0.2f, 0.4f, 0.8f);

    material.features.clearcoat                             = 0.7f;

    material.features.clearcoatRoughness                    = 0.3f;

    material.features.sheenColor                            = Vec3(0.1f, 0.5f, 0.9f);

    material.features.sheenRoughness                        = 0.4f;

    material.features.transmission                          = 0.6f;

    material.features.thickness                             = 1.2f;

    material.features.iridescence                           = 0.8f;

    material.features.dispersion                            = 0.05f;

    material.features.anisotropy                            = 0.9f;

    material.features.diffuseTransmission                   = 0.25f;

    material.features.multiscatterColor                     = Vec3(0.2f, 0.5f, 0.7f);

    material.features.retroreflection                       = 0.8f;

    material.features.clearcoatNormalTexture.texture        = scene.GetDefaultTextures().pNormal;

    material.features.clearcoatNormalTexture.texCoord       = 2;

    material.features.clearcoatNormalTexture.scale          = 0.35f;

    material.features.clearcoatNormalTexture.transform.row0 = Vec4(2, 0, 0.2f, 1);

    material.features.clearcoatNormalTexture.transform.row1 = Vec4(0, 3, 0.4f, 0);

    material.SetData();

    EXPECT_FLOAT_EQ(material.data.surfaceProperties.y, float(sg::AlphaMode::Blend));

    EXPECT_EQ(material.data.specularColorIor, Vec4(0.2f, 0.4f, 0.8f, 1.8f));

    EXPECT_EQ(material.data.clearcoatSheenSpecular, Vec4(0.7f, 0.3f, 0.4f, 1.0f));

    EXPECT_EQ(material.data.sheenColorTransmission, Vec4(0.1f, 0.5f, 0.9f, 0.6f));

    EXPECT_FLOAT_EQ(material.data.volumeIridescence.x, 1.2f);

    EXPECT_FLOAT_EQ(material.data.volumeIridescence.y, 0.0f);

    EXPECT_FLOAT_EQ(material.data.volumeIridescence.z, 0.8f);

    EXPECT_FLOAT_EQ(material.data.attenuationColorDispersion.w, 0.05f);

    EXPECT_FLOAT_EQ(material.data.iridescenceAnisotropy.z, 0.9f);

    EXPECT_FLOAT_EQ(material.data.diffuseTransmissionColorFactor.w, 0.25f);

    EXPECT_EQ(material.data.volumeScatterColorRetroreflection, Vec4(0.2f, 0.5f, 0.7f, 0.8f));

    EXPECT_FLOAT_EQ(material.data.materialProperties.w, 1.0f);

    const sg::MaterialTextureData& texture =
        material.data.featureTextures[static_cast<uint32_t>(sg::MaterialFeatureTexture::ClearcoatNormal)];

    EXPECT_EQ(texture.properties, Vec4(float(scene.GetDefaultTextures().pNormal->index), 2, 0.35f, 0));

    EXPECT_EQ(texture.transform, sg::PublishTextureTransform(material.features.clearcoatNormalTexture.transform));
}
