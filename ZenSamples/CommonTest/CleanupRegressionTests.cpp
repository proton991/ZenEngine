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
        EXPECT_EQ(textures[index]->format, asset::Format::R8G8B8A8_SRGB);
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
