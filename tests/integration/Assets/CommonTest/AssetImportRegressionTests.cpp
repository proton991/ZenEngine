#include "Utils/Errors.h"
#include "Platform/FileSystem.h"
#include "SceneGraph/Scene.h"
#include "ZenFoundation/Templates/HeapVector.h"
#include "ZenFoundation/Utils/ThreadPool.h"
#include "AssetLib/TextureLoader.h"
#include "SceneTestLoader.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

using namespace zen;

TEST(FastGLTFLoaderRegression, TextureBatchesTransferOwnershipAndResolveImageFormats)
{
    const std::filesystem::path path = std::filesystem::path(ZEN_ASSET_TEST_CORPUS) / "cleanup_texture.gltf";

    sg::Scene scene;

    test::SceneLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(path.string(), &scene)) << loader.GetError();

    const zen::HeapVector<sg::Texture*> textures = scene.GetComponents<sg::Texture>();

    ASSERT_EQ(textures.size(), 7u);

    for (uint32_t index = 0; index < 2; ++index)
    {
        ASSERT_NE(textures[index], nullptr);

        EXPECT_EQ(textures[index]->index, index);

        EXPECT_EQ(textures[index]->format, index == 1 ? asset::Format::R8G8B8A8_SRGB : asset::Format::R8G8B8A8_UNORM);

        EXPECT_EQ(textures[index]->width, 1u);

        EXPECT_EQ(textures[index]->height, 1u);

        EXPECT_EQ(textures[index]->GetBytes().size(), 4u);
    }
}

TEST(FastGLTFLoaderRegression, KeepsTransformsForNonRenderableAncestors)
{
    const std::filesystem::path path = std::filesystem::path(ZEN_ASSET_TEST_CORPUS) / "cleanup_texture.gltf";

    sg::Scene scene;

    test::SceneLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(path.string(), &scene)) << loader.GetError();

    const HeapVector<sg::Transform*> transforms = scene.GetComponents<sg::Transform>();

    ASSERT_EQ(transforms.size(), 3u);

    EXPECT_EQ(Vec3(transforms.back()->GetWorldMatrix()[3]), Vec3(13, 0, 0));

    transforms.front()->SetTranslation(Vec3(20, 0, 0));

    EXPECT_EQ(Vec3(transforms.back()->GetWorldMatrix()[3]), Vec3(23, 0, 0));
}

TEST(FastGLTFLoaderRegression, MissingTexturesPreserveMaterialFactorsIncludingEmission)
{
    const std::filesystem::path path = std::filesystem::path(ZEN_ASSET_TEST_CORPUS) / "voxel_gi_material.gltf";

    sg::Scene scene;

    test::SceneLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(path.string(), &scene)) << loader.GetError();

    const sg::Scene::DefaultTextures defaults = scene.GetDefaultTextures();

    for (const sg::Texture* texture : {defaults.pBaseColor, defaults.pMetallicRoughness, defaults.pEmissive})
    {
        ASSERT_NE(texture, nullptr);

        ASSERT_EQ(texture->GetBytes().size(), 4u);

        for (uint8_t channel : texture->GetBytes())
        {
            EXPECT_EQ(channel, 255u);
        }
    }

    const zen::HeapVector<sg::Material*> materials = scene.GetComponents<sg::Material>();

    ASSERT_FALSE(materials.empty());

    EXPECT_EQ(materials[0]->data.baseColorFactor, Vec4(0.2f, 0.3f, 0.4f, 0.5f));

    EXPECT_EQ(materials[0]->data.emissiveFactor, Vec4(8, 4, 2, 0));

    ASSERT_EQ(loader.GetVertices().size(), 3u);

    for (const asset::Vertex& vertex : loader.GetVertices())
    {
        EXPECT_EQ(vertex.color, Vec4(1.0f));
    }
}

TEST(FastGLTFLoaderRegression, PreservesNormalMapScaleUVSetAndMirroredTangents)
{
    const std::filesystem::path path = std::filesystem::path(ZEN_ASSET_TEST_CORPUS) / "normal_material.gltf";

    sg::Scene scene;

    test::SceneLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(path.string(), &scene)) << loader.GetError();

    ASSERT_EQ(loader.GetVertices().size(), 3u);

    for (const asset::Vertex& vertex : loader.GetVertices())
    {
        EXPECT_EQ(vertex.tangent, Vec4(1, 0, 0, -1));
    }

    ASSERT_FALSE(scene.GetComponents<sg::Material>().empty());

    const sg::Material* material = scene.GetComponents<sg::Material>().front();

    ASSERT_NE(material->m_pNormalTexture, nullptr);

    EXPECT_EQ(material->m_pNormalTexture->format, asset::Format::R8G8B8A8_UNORM);

    EXPECT_FLOAT_EQ(material->normalScale, 0.35f);

    EXPECT_FLOAT_EQ(material->data.surfaceProperties.z, 0.35f);

    EXPECT_EQ(material->data.normalTexSet, 1);

    EXPECT_EQ(sizeof(sg::MaterialData), 1632u);
}

TEST(FastGLTFLoaderRegression, PreservesRGBAndRGBAColorsAcrossComponentTypesAndStrides)
{
    const std::filesystem::path path = std::filesystem::path(ZEN_ASSET_TEST_CORPUS) / "vertex_colors.gltf";

    sg::Scene scene;

    test::SceneLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(path.string(), &scene)) << loader.GetError();

    // RGB and RGBA, each with float/normalized byte/normalized short, tightly packed
    // (with glTF's four-byte vertex alignment) and padded with an accessor offset.
    constexpr uint32_t variants = 12;

    const Vec4 expected[] = {Vec4(0, 1.0f / 3, 2.0f / 3, 1), Vec4(1, 2.0f / 3, 1.0f / 3, 0), Vec4(2.0f / 3, 1, 0, 1.0f / 3)};

    ASSERT_EQ(loader.GetVertices().size(), variants * 3u);

    for (uint32_t variant = 0; variant < variants; ++variant)
    {
        SCOPED_TRACE(variant);

        for (uint32_t vertex = 0; vertex < 3; ++vertex)
        {
            SCOPED_TRACE(vertex);

            const Vec4 color = loader.GetVertices()[variant * 3 + vertex].color;

            for (uint32_t channel = 0; channel < 4; ++channel)
            {
                const float value = variant < 6 && channel == 3 ? 1.0f : expected[vertex][channel];

                EXPECT_FLOAT_EQ(color[channel], value);
            }
        }
    }
}

TEST(FastGLTFLoaderRegression, DecodesInterleavedNormalizedAndSparseVertexAttributes)
{
    const std::filesystem::path path = std::filesystem::path(ZEN_ASSET_TEST_CORPUS) / "vertex_attributes.gltf";

    sg::Scene scene;

    test::SceneLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(path.string(), &scene)) << loader.GetError();

    ASSERT_EQ(loader.GetVertices().size(), 9u);

    ASSERT_EQ(loader.GetIndices().size(), 9u);

    const Vec3 positions[] = {Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0)};

    const Vec2 uv[]        = {Vec2(0, 1), Vec2(1, 0), Vec2(1.0f / 3, 2.0f / 3)};

    const Vec4 weights[]   = {Vec4(1, 0, 0, 0), Vec4(0, 1, 0, 0), Vec4(1.0f / 3, 2.0f / 3, 0, 0)};

    for (uint32_t variant = 0; variant < 3; ++variant)
    {
        SCOPED_TRACE(variant);

        for (uint32_t index = 0; index < 3; ++index)
        {
            SCOPED_TRACE(index);

            const asset::Vertex& vertex = loader.GetVertices()[variant * 3 + index];

            EXPECT_EQ(vertex.pos, Vec4(positions[index], 1));

            EXPECT_EQ(vertex.normal, Vec4(0, 0, 1, 0));

            EXPECT_EQ(vertex.tangent, Vec4(1, 0, 0, -1));

            EXPECT_EQ(vertex.joint0, Vec4(0, 128, 200, 255));

            for (uint32_t channel = 0; channel < 2; ++channel)
            {
                EXPECT_FLOAT_EQ(vertex.uv0[channel], uv[index][channel]);

                EXPECT_FLOAT_EQ(vertex.uv1[channel], uv[index][1 - channel]);
            }

            for (uint32_t channel = 0; channel < 4; ++channel)
            {
                EXPECT_FLOAT_EQ(vertex.weight0[channel], weights[index][channel]);

                EXPECT_FLOAT_EQ(vertex.color[channel], channel == index || channel == 3 ? 1 : 0);
            }

            EXPECT_EQ(loader.GetIndices()[variant * 3 + index], variant * 3 + index);
        }
    }
}

TEST(FastGLTFLoaderRegression, SplitsSharedVerticesForFlatNormalsAndIgnoresAuthoredTangents)
{
    const std::filesystem::path path = std::filesystem::path(ZEN_ASSET_TEST_CORPUS) / "flat_normals_default.gltf";

    sg::Scene scene;

    test::SceneLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(path.string(), &scene)) << loader.GetError();

    ASSERT_EQ(loader.GetVertices().size(), 9u);

    ASSERT_EQ(loader.GetIndices().size(), 9u);

    for (uint32_t index = 0; index < 9; ++index)
    {
        const asset::Vertex& vertex = loader.GetVertices()[index];

        EXPECT_EQ(vertex.normal, index < 3 ? Vec4(0, 0, 1, 0) : Vec4(0, 1, 0, 0));

        EXPECT_EQ(vertex.tangent, Vec4(0));

        EXPECT_EQ(loader.GetIndices()[index], index);
    }

    EXPECT_EQ(loader.GetVertices()[0].pos, loader.GetVertices()[3].pos);

    EXPECT_EQ(loader.GetVertices()[0].color, loader.GetVertices()[3].color);

    EXPECT_EQ(loader.GetVertices()[1].color, loader.GetVertices()[5].color);
}

TEST(FastGLTFLoaderRegression, InitializesAndIndexesDefaultMaterialWithAndWithoutAuthoredMaterials)
{
    for (const char* fixture : {"flat_normals_default.gltf", "default_material.gltf"})
    {
        SCOPED_TRACE(fixture);

        const std::filesystem::path path = std::filesystem::path(ZEN_ASSET_TEST_CORPUS) / fixture;

        sg::Scene scene;

        test::SceneLoader loader;

        ASSERT_TRUE(loader.LoadFromFile(path.string(), &scene)) << loader.GetError();

        const zen::HeapVector<sg::Material*> materials = scene.GetComponents<sg::Material>();

        const zen::HeapVector<sg::SubMesh*> meshes     = scene.GetComponents<sg::SubMesh>();

        ASSERT_FALSE(materials.empty());

        ASSERT_FALSE(meshes.empty());

        const sg::Material* fallback = materials.back();

        EXPECT_EQ(fallback->index, materials.size() - 1);

        EXPECT_EQ(fallback->data.baseColorFactor, Vec4(1));

        EXPECT_EQ(fallback->data.emissiveFactor, Vec4(0));

        EXPECT_FLOAT_EQ(fallback->data.metallicFactor, 1);

        EXPECT_FLOAT_EQ(fallback->data.roughnessFactor, 1);

        EXPECT_EQ(fallback->data.bcTexIndex, fallback->m_pBaseColorTexture->index);

        EXPECT_EQ(fallback->data.mrTexIndex, fallback->m_pMetallicRoughnessTexture->index);

        for (const sg::SubMesh* mesh : meshes)
        {
            EXPECT_EQ(mesh->GetMaterial(), fallback);

            EXPECT_EQ(mesh->GetMaterialIndex(), fallback->index);
        }
    }
}

TEST(FastGLTFLoaderRegression, SeparatesLinearAndColorUsesOfSharedImagesAndTextures)
{
    const std::filesystem::path path = std::filesystem::path(ZEN_ASSET_TEST_CORPUS) / "texture_roles.gltf";

    sg::Scene scene;

    test::SceneLoader loader;

    ASSERT_TRUE(loader.LoadFromFile(path.string(), &scene)) << loader.GetError();

    const zen::HeapVector<sg::Texture*> textures   = scene.GetComponents<sg::Texture>();

    const zen::HeapVector<sg::Material*> materials = scene.GetComponents<sg::Material>();

    ASSERT_EQ(textures.size(), 7u); // One color interpretation, one linear, five defaults.

    ASSERT_EQ(materials.size(), 3u);

    for (uint32_t index = 0; index < textures.size(); ++index)
    {
        EXPECT_EQ(textures[index]->index, index);
    }

    const sg::Texture* color  = materials[0]->m_pBaseColorTexture;

    const sg::Texture* linear = materials[0]->m_pMetallicRoughnessTexture;

    ASSERT_NE(color, linear);

    EXPECT_EQ(color->format, asset::Format::R8G8B8A8_SRGB);

    EXPECT_EQ(linear->format, asset::Format::R8G8B8A8_UNORM);

    EXPECT_TRUE(
        std::equal(color->GetBytes().begin(), color->GetBytes().end(), linear->GetBytes().begin(), linear->GetBytes().end()));

    for (uint32_t materialIndex = 0; materialIndex < 2; ++materialIndex)
    {
        const sg::MaterialData& material = materials[materialIndex]->data;

        EXPECT_EQ(materials[materialIndex]->m_pBaseColorTexture, color);

        EXPECT_EQ(materials[materialIndex]->m_pMetallicRoughnessTexture, linear);

        EXPECT_EQ(materials[materialIndex]->m_pNormalTexture, linear);

        EXPECT_EQ(materials[materialIndex]->m_pOcclusionTexture, linear);

        EXPECT_EQ(material.bcTexIndex, color->index);

        EXPECT_EQ(material.mrTexIndex, linear->index);

        EXPECT_EQ(material.normalTexIndex, linear->index);

        EXPECT_EQ(material.occlusionTexIndex, linear->index);
    }

    EXPECT_EQ(materials[0]->data.emissiveTexIndex, color->index);

    EXPECT_EQ(materials.back()->m_pBaseColorTexture, scene.GetDefaultTextures().pBaseColor);

    EXPECT_EQ(materials.back()->data.bcTexIndex, scene.GetDefaultTextures().pBaseColor->index);

    // The 8-bit stand-in cannot encode a flat normal; without an authored map the material
    // publishes none, so shading keeps the vertex normal instead of tilting it 0.32 degrees.
    EXPECT_EQ(materials.back()->m_pNormalTexture, scene.GetDefaultTextures().pNormal);

    EXPECT_EQ(materials.back()->data.normalTexIndex, -1);
}
