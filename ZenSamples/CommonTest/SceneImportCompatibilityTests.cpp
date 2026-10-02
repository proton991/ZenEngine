#include <gtest/gtest.h>
#include <cstddef>
#include <filesystem>
#include "AssetLib/FastGLTFLoader.h"
#include "SceneGraph/Scene.h"

using namespace zen;

namespace
{
std::string CompatibilityFixture(const char* name)
{
    const std::string result = (std::filesystem::path(__FILE__).parent_path() / "Assets" / name).string();

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

    asset::FastGLTFLoader loader;

    loader.LoadFromFile(CompatibilityFixture("all_primitive_modes.gltf"), &scene);

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

    asset::FastGLTFLoader loader;

    loader.LoadFromFile(CompatibilityFixture("all_primitive_modes.gltf"), &scene);

    const size_t vertexCount = loader.GetVertices().size();

    const size_t indexCount  = loader.GetIndices().size();

    const size_t nodeCount   = scene.GetNodes().size();

    EXPECT_THROW(loader.LoadFromFile(CompatibilityFixture("invalid_compressed_scene.gltf"), &scene), std::exception);

    EXPECT_EQ(loader.GetVertices().size(), vertexCount);

    EXPECT_EQ(loader.GetIndices().size(), indexCount);

    EXPECT_EQ(scene.GetNodes().size(), nodeCount);

    EXPECT_EQ(scene.GetName(), "all_primitive_modes");
}

TEST(SceneImportCompatibility, PublishesAdvancedMaterialFactorsAndUVBindingsToGPUData)
{
    sg::Scene scene;

    sg::Material material("MaterialPublication");

    AttachDefaultTextures(material, scene);

    material.SetData();

    EXPECT_EQ(sizeof(sg::TextureTransformData), 32u);

    EXPECT_EQ(sizeof(sg::MaterialTextureData), 48u);

    EXPECT_EQ(offsetof(sg::MaterialData, specularColorIor), 272u);

    EXPECT_EQ(offsetof(sg::MaterialData, volumeScatterColorRetroreflection), 416u);

    EXPECT_EQ(offsetof(sg::MaterialData, featureTextures), 432u);

    EXPECT_EQ(sizeof(sg::MaterialData), 1248u);

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
