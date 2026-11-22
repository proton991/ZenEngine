#include "Math/Math.h"
#include "Utils/Errors.h"
#include "Platform/FileSystem.h"
#include "SceneGraph/Scene.h"
#include "ZenFoundation/Templates/HeapVector.h"
#include "AssetLib/TextureLoader.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

using namespace zen;

TEST(SceneRegression, RotatedAndReflectedBoundsEncloseEveryCorner)
{
    sg::AABB bounds(Vec3(-2.0f, -1.0f, -3.0f), Vec3(2.0f, 1.0f, 3.0f));

    const Mat4 transform =
        glm::rotate(Mat4(1.0f), glm::radians(45.0f), Vec3(0, 0, 1)) * glm::scale(Mat4(1.0f), Vec3(-1.0f, 1.0f, 1.0f));

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
    platform::FileLoadError error   = platform::FileLoadError::eOpenFailed;

    const HeapVector<uint8_t> bytes = platform::FileSystem::LoadSpvFile("VoxelGI/voxel_vis.vert.spv", &error);

    EXPECT_EQ(error, platform::FileLoadError::eNone);

    const HeapVector<uint32_t> words = platform::FileSystem::LoadSpvFile<uint32_t>("VoxelGI/voxel_vis.vert.spv");

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
        m_name =
            "filesystem-error-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".spv";

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

    std::string           m_name;
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
