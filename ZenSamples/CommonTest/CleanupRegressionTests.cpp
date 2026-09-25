#include "Platform/FileSystem.h"
#include "SceneGraph/Scene.h"
#include "Templates/HeapVector.h"
#include "Utils/ThreadPool.h"
#include "AssetLib/TextureLoader.h"
#include "AssetLib/FastGLTFLoader.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

using namespace zen;

TEST(HeapVectorRegression, AppendPreservesAliasedSourceDuringGrowth)
{
    HeapVector<std::string> values{"first", "second"};
    values.push_back(values);
    ASSERT_EQ(values.size(), 4u);
    EXPECT_EQ(values[2], "first");
    EXPECT_EQ(values[3], "second");
    values.push_back(VectorView<std::string>(values.data() + 1, 2));
    ASSERT_EQ(values.size(), 6u);
    EXPECT_EQ(values[4], "second");
    EXPECT_EQ(values[5], "first");
}

TEST(HeapVectorRegression, EraseRangeMovesOwnedElementsOnce)
{
    HeapVector<UniquePtr<int>> values;
    for (int i = 0; i < 6; ++i)
    {
        values.emplace_back(MakeUnique<int>(i));
    }
    HeapVector<UniquePtr<int>>::iterator next =
        values.erase(values.begin() + 1, values.begin() + 4);
    ASSERT_EQ(values.size(), 3u);
    EXPECT_EQ(**next, 4);
    EXPECT_EQ(*values[0], 0);
    EXPECT_EQ(*values[2], 5);
    values.remove(1);
    EXPECT_EQ(*values.back(), 5);
    next = values.erase(values.begin(), values.end());
    EXPECT_EQ(next, values.end());
    EXPECT_TRUE(values.empty());
}

TEST(HeapVectorRegression, EmptyEraseAndFilledConstruction)
{
    HeapVector<int> empty;
    EXPECT_EQ(empty.erase(empty.begin(), empty.end()), empty.end());
    const HeapVector<int> filled(4, -1);
    EXPECT_EQ(filled.size(), 4u);
    EXPECT_EQ(filled.front(), -1);
    EXPECT_EQ(filled.back(), -1);
}

TEST(SceneRegression, RotatedAndReflectedBoundsEncloseEveryCorner)
{
    sg::AABB bounds(Vec3(-2.0f, -1.0f, -3.0f), Vec3(2.0f, 1.0f, 3.0f));
    const Mat4 transform = glm::rotate(Mat4(1.0f), glm::radians(45.0f), Vec3(0, 0, 1)) *
        glm::scale(Mat4(1.0f), Vec3(-1.0f, 1.0f, 1.0f));
    bounds.Transform(transform);
    const float extent = 3.0f / std::sqrt(2.0f);
    EXPECT_NEAR(bounds.GetMin().x, -extent, 0.0001f);
    EXPECT_NEAR(bounds.GetMax().x, extent, 0.0001f);
    EXPECT_NEAR(bounds.GetMin().y, -extent, 0.0001f);
    EXPECT_NEAR(bounds.GetMax().y, extent, 0.0001f);
}

TEST(SceneRegression, RepeatedBoundsUpdatePreservesLocalMeshBounds)
{
    sg::Scene scene;
    sg::Node node(0, "node");
    sg::Mesh mesh("mesh");
    mesh.SetAABB(Vec3(-4, -3, -2), Vec3(-2, -1, -1));
    sg::Transform transform(node);
    transform.SetTranslation(Vec3(10, 0, 0));
    node.AddComponent(&mesh);
    node.AddComponent(&transform);
    scene.AddRenderableNode(&node);
    scene.UpdateAABB();
    const sg::AABB first = scene.GetAABB();
    scene.UpdateAABB();
    EXPECT_EQ(scene.GetAABB(), first);
    EXPECT_EQ(mesh.GetAABB().GetMin(), Vec3(-4, -3, -2));
    EXPECT_EQ(scene.GetLocalAABB().GetMax(), Vec3(-2, -1, -1));
    EXPECT_EQ(first.GetMax(), Vec3(8, -1, -1));
}

TEST(SceneRegression, MeshEqualityChecksRightHandSubmesh)
{
    sg::Mesh left("mesh");
    sg::Mesh right("mesh");
    sg::SubMesh leftPart("part", 0, 3, 3);
    sg::SubMesh rightPart("part", 3, 6, 3);
    left.AddSubMesh(&leftPart);
    right.AddSubMesh(&rightPart);
    EXPECT_NE(left, right);
    rightPart.SetFirstIndex(0);
    rightPart.SetIndexCount(3);
    EXPECT_EQ(left, right);
    rightPart.SetIndexCount(0);
    EXPECT_FALSE(rightPart.HasIndices());
}

TEST(SceneRegression, MaterialEqualityHandlesMissingTextures)
{
    sg::Material left("material");
    sg::Material right("material");
    EXPECT_EQ(left, right);
    sg::Texture texture("texture");
    left.m_pBaseColorTexture = &texture;
    EXPECT_NE(left, right);
    right.m_pBaseColorTexture = &texture;
    EXPECT_EQ(left, right);
}

TEST(SceneRegression, TransformUpdatesDoNotAccumulateOrDoubleCountAncestors)
{
    sg::Node root(0, "root");
    sg::Node parent(1, "parent");
    sg::Node child(2, "child");
    sg::Transform rootTransform(root);
    sg::Transform parentTransform(parent);
    sg::Transform childTransform(child);
    root.AddComponent(&rootTransform);
    parent.AddComponent(&parentTransform);
    child.AddComponent(&childTransform);
    parent.SetParent(&root);
    child.SetParent(&parent);
    rootTransform.SetTranslation(Vec3(10, 0, 0));
    parentTransform.SetTranslation(Vec3(2, 0, 0));
    childTransform.SetTranslation(Vec3(1, 0, 0));
    EXPECT_EQ(Vec3(childTransform.GetWorldMatrix()[3]), Vec3(13, 0, 0));
    rootTransform.SetTranslation(Vec3(20, 0, 0));
    EXPECT_EQ(Vec3(childTransform.GetWorldMatrix()[3]), Vec3(23, 0, 0));
    childTransform.SetTranslation(Vec3(3, 0, 0));
    EXPECT_EQ(Vec3(childTransform.GetWorldMatrix()[3]), Vec3(25, 0, 0));
    childTransform.SetLocalMatrix(glm::translate(Mat4(1), Vec3(4, 0, 0)));
    EXPECT_EQ(Vec3(childTransform.GetWorldMatrix()[3]), Vec3(29, 0, 0));
}

TEST(TextureLoaderRegression, BothOverloadsAgreeAndMissingImagesStayEmpty)
{
    const asset::TextureInfo value = asset::TextureLoader::LoadTexture2DFromFile("wood.png");
    asset::TextureInfo output;
    asset::TextureLoader::LoadTexture2DFromFile("wood.png", &output);
    ASSERT_GT(value.width, 0u);
    EXPECT_EQ(output.width, value.width);
    EXPECT_EQ(output.height, value.height);
    EXPECT_EQ(output.format, value.format);
    EXPECT_EQ(output.data, value.data);
    asset::TextureLoader::LoadTexture2DFromFile("cleanup-nonexistent-image.png", &output);
    EXPECT_EQ(output.width, 0u);
    EXPECT_TRUE(output.data.empty());
}

TEST(FileSystemRegression, SpirvWordAndByteLoadsAgree)
{
    platform::FileLoadError error = platform::FileLoadError::eOpenFailed;
    const HeapVector<uint8_t> bytes =
        platform::FileSystem::LoadSpvFile("VoxelGI/voxel_vis.vert.spv", &error);
    EXPECT_EQ(error, platform::FileLoadError::eNone);
    const HeapVector<uint32_t> words =
        platform::FileSystem::LoadSpvFile<uint32_t>("VoxelGI/voxel_vis.vert.spv");
    ASSERT_FALSE(words.empty());
    EXPECT_EQ(bytes.size(), words.size() * sizeof(uint32_t));
    EXPECT_EQ(std::memcmp(bytes.data(), words.data(), bytes.size()), 0);
    EXPECT_EQ(words[0], 0x07230203u);
}

class FileSystemErrorTest : public testing::Test
{
protected:
    void SetUp() override
    {
        m_name = "filesystem-error-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".spv";
        m_path = std::filesystem::path(SPV_SHADER_PATH) / m_name;
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove(m_path, error);
        EXPECT_FALSE(error);
    }

    bool Write(std::string_view data)
    {
        std::ofstream file(m_path, std::ios::binary | std::ios::trunc);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        file.close();
        return !file.fail();
    }

    std::string m_name;
    std::filesystem::path m_path;
};

TEST_F(FileSystemErrorTest, MissingFilesReturnEmptyDataAndOpenError)
{
    platform::FileLoadError error   = platform::FileLoadError::eNone;
    const HeapVector<uint8_t> spirv = platform::FileSystem::LoadSpvFile(m_name, &error);
    EXPECT_TRUE(spirv.empty());
    EXPECT_EQ(error, platform::FileLoadError::eOpenFailed);
    const std::string text = platform::FileSystem::LoadTextFile(m_path.string(), &error);
    EXPECT_TRUE(text.empty());
    EXPECT_EQ(error, platform::FileLoadError::eOpenFailed);
    EXPECT_TRUE(platform::FileSystem::LoadSpvFile(m_name).empty());
    EXPECT_TRUE(platform::FileSystem::LoadTextFile(m_path.string()).empty());
}

TEST_F(FileSystemErrorTest, InvalidSpirvSizesReturnEmptyDataAndSizeError)
{
    platform::FileLoadError error = platform::FileLoadError::eNone;
    ASSERT_TRUE(Write("abc"));
    EXPECT_TRUE(platform::FileSystem::LoadSpvFile(m_name, &error).empty());
    EXPECT_EQ(error, platform::FileLoadError::eInvalidSize);
    ASSERT_TRUE(Write(""));
    EXPECT_TRUE(platform::FileSystem::LoadSpvFile(m_name, &error).empty());
    EXPECT_EQ(error, platform::FileLoadError::eInvalidSize);
    ASSERT_TRUE(Write("abcd"));
    EXPECT_TRUE(platform::FileSystem::LoadSpvFile<uint64_t>(m_name, &error).empty());
    EXPECT_EQ(error, platform::FileLoadError::eInvalidSize);
}

TEST_F(FileSystemErrorTest, TextReadsAndEmptyFilesClearPreviousErrors)
{
    platform::FileLoadError error = platform::FileLoadError::eOpenFailed;
    const std::string text(5000, 'a');
    ASSERT_TRUE(Write(text));
    EXPECT_EQ(platform::FileSystem::LoadTextFile(m_path.string(), &error), text);
    EXPECT_EQ(error, platform::FileLoadError::eNone);
    ASSERT_TRUE(Write(""));
    EXPECT_TRUE(platform::FileSystem::LoadTextFile(m_path.string(), &error).empty());
    EXPECT_EQ(error, platform::FileLoadError::eNone);
}

TEST(FastGLTFLoaderRegression, TextureBatchesTransferOwnershipAndResolveImageFormats)
{
    const std::filesystem::path path =
        std::filesystem::path(__FILE__).parent_path() / "Assets/cleanup_texture.gltf";
    sg::Scene scene;
    asset::FastGLTFLoader loader;
    loader.LoadFromFile(path.string(), &scene);
    const std::vector<sg::Texture*> textures = scene.GetComponents<sg::Texture>();
    ASSERT_EQ(textures.size(), 7u);
    for (uint32_t index = 0; index < 2; ++index)
    {
        ASSERT_NE(textures[index], nullptr);
        EXPECT_EQ(textures[index]->index, index);
        EXPECT_EQ(textures[index]->format,
                  index == 1 ? asset::Format::R8G8B8A8_SRGB : asset::Format::R8G8B8A8_UNORM);
        EXPECT_EQ(textures[index]->width, 1u);
        EXPECT_EQ(textures[index]->height, 1u);
        EXPECT_EQ(textures[index]->bytesData.size(), 4u);
    }
}

TEST(FastGLTFLoaderRegression, KeepsTransformsForNonRenderableAncestors)
{
    const std::filesystem::path path =
        std::filesystem::path(__FILE__).parent_path() / "Assets/cleanup_texture.gltf";
    sg::Scene scene;
    asset::FastGLTFLoader loader;
    loader.LoadFromFile(path.string(), &scene);
    const std::vector<sg::Transform*> transforms = scene.GetComponents<sg::Transform>();
    ASSERT_EQ(transforms.size(), 3u);
    EXPECT_EQ(Vec3(transforms.back()->GetWorldMatrix()[3]), Vec3(13, 0, 0));
    transforms.front()->SetTranslation(Vec3(20, 0, 0));
    EXPECT_EQ(Vec3(transforms.back()->GetWorldMatrix()[3]), Vec3(23, 0, 0));
}

static int AddValues(int left, int right)
{
    return left + right;
}

TEST(ThreadPoolRegression, ResizeToZeroJoinsWorkersAndCanRestart)
{
    ThreadPool<void, uint32_t> pool(2);
    std::future<int> first = pool.Push([](uint32_t) { return 7; });
    ASSERT_EQ(first.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(first.get(), 7);
    pool.Resize(0);
    EXPECT_EQ(pool.GetSize(), 0u);
    pool.Resize(1);
    std::future<int> bound = pool.Push(AddValues, 3, 4);
    ASSERT_EQ(bound.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(bound.get(), 7);
    pool.Stop(true);
    pool.Stop(true);
    EXPECT_EQ(pool.GetSize(), 0u);
    EXPECT_THROW(pool.Push(AddValues, 1, 2), std::runtime_error);
}

TEST(ThreadPoolRegression, GracefulStopDrainsTasksAndPropagatesExceptions)
{
    ThreadPool<void, uint32_t> pool(2);
    std::atomic<uint32_t> count{0};
    for (uint32_t i = 0; i < 20; ++i)
    {
        pool.Push([&count](uint32_t) { ++count; });
    }
    std::future<void> failed = pool.Push([](uint32_t) { throw std::runtime_error("task failed"); });
    pool.Stop(true);
    EXPECT_EQ(count.load(), 20u);
    EXPECT_THROW(failed.get(), std::runtime_error);
}

TEST(FastGLTFLoaderRegression, MissingTexturesPreserveMaterialFactorsIncludingEmission)
{
    const std::filesystem::path path =
        std::filesystem::path(__FILE__).parent_path() / "Assets/voxel_gi_material.gltf";
    sg::Scene scene;
    asset::FastGLTFLoader loader;
    loader.LoadFromFile(path.string(), &scene);
    const sg::Scene::DefaultTextures defaults = sg::Scene::GetDefaultTextures();
    for (const sg::Texture* texture :
         {defaults.pBaseColor, defaults.pMetallicRoughness, defaults.pEmissive})
    {
        ASSERT_NE(texture, nullptr);
        ASSERT_EQ(texture->bytesData.size(), 4u);
        for (uint8_t channel : texture->bytesData)
        {
            EXPECT_EQ(channel, 255u);
        }
    }
    const std::vector<sg::Material*> materials = scene.GetComponents<sg::Material>();
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
    const std::filesystem::path path =
        std::filesystem::path(__FILE__).parent_path() / "Assets/normal_material.gltf";
    sg::Scene scene;
    asset::FastGLTFLoader loader;
    loader.LoadFromFile(path.string(), &scene);

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
    EXPECT_EQ(sizeof(sg::MaterialData), 96u);
}

TEST(FastGLTFLoaderRegression, PreservesRGBAndRGBAColorsAcrossComponentTypesAndStrides)
{
    const std::filesystem::path path =
        std::filesystem::path(__FILE__).parent_path() / "Assets/vertex_colors.gltf";
    sg::Scene scene;
    asset::FastGLTFLoader loader;
    loader.LoadFromFile(path.string(), &scene);
    // RGB and RGBA, each with float/normalized byte/normalized short, tightly packed
    // (with glTF's four-byte vertex alignment) and padded with an accessor offset.
    constexpr uint32_t variants = 12;
    const Vec4 expected[]       = {Vec4(0, 1.0f / 3, 2.0f / 3, 1), Vec4(1, 2.0f / 3, 1.0f / 3, 0),
                                   Vec4(2.0f / 3, 1, 0, 1.0f / 3)};
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
    const std::filesystem::path path =
        std::filesystem::path(__FILE__).parent_path() / "Assets/vertex_attributes.gltf";
    sg::Scene scene;
    asset::FastGLTFLoader loader;
    loader.LoadFromFile(path.string(), &scene);
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
    const std::filesystem::path path =
        std::filesystem::path(__FILE__).parent_path() / "Assets/flat_normals_default.gltf";
    sg::Scene scene;
    asset::FastGLTFLoader loader;
    loader.LoadFromFile(path.string(), &scene);
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
        const std::filesystem::path path =
            std::filesystem::path(__FILE__).parent_path() / "Assets" / fixture;
        sg::Scene scene;
        asset::FastGLTFLoader loader;
        loader.LoadFromFile(path.string(), &scene);
        const std::vector<sg::Material*> materials = scene.GetComponents<sg::Material>();
        const std::vector<sg::SubMesh*> meshes     = scene.GetComponents<sg::SubMesh>();
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
    const std::filesystem::path path =
        std::filesystem::path(__FILE__).parent_path() / "Assets/texture_roles.gltf";
    sg::Scene scene;
    asset::FastGLTFLoader loader;
    loader.LoadFromFile(path.string(), &scene);
    const std::vector<sg::Texture*> textures   = scene.GetComponents<sg::Texture>();
    const std::vector<sg::Material*> materials = scene.GetComponents<sg::Material>();
    ASSERT_EQ(textures.size(), 8u); // Two authored textures, one linear copy, five defaults.
    ASSERT_EQ(materials.size(), 3u);
    for (uint32_t index = 0; index < textures.size(); ++index)
    {
        EXPECT_EQ(textures[index]->index, index);
    }
    EXPECT_EQ(textures[0]->format, asset::Format::R8G8B8A8_SRGB);
    EXPECT_EQ(textures[1]->format, asset::Format::R8G8B8A8_UNORM);
    EXPECT_EQ(textures[2]->format, asset::Format::R8G8B8A8_UNORM);
    EXPECT_EQ(textures[0]->bytesData, textures[1]->bytesData);
    EXPECT_EQ(textures[0]->bytesData, textures[2]->bytesData);
    for (uint32_t materialIndex = 0; materialIndex < 2; ++materialIndex)
    {
        const sg::MaterialData& material = materials[materialIndex]->data;
        const int linearIndex            = materialIndex == 0 ? 2 : 1;
        EXPECT_EQ(material.bcTexIndex, 0);
        EXPECT_EQ(material.mrTexIndex, linearIndex);
        EXPECT_EQ(material.normalTexIndex, linearIndex);
        EXPECT_EQ(material.occlusionTexIndex, linearIndex);
    }
    EXPECT_EQ(materials[0]->data.emissiveTexIndex, 0);
    EXPECT_EQ(materials.back()->data.bcTexIndex, 3);
}
