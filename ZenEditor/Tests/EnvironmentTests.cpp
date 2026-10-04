#include "Editor/Model/EditorEnvironment.h"
#include "Editor/Model/EditorText.h"
#include "AssetLib/TextureLoader.h"
#include "SyntheticEnvironment.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <fstream>

namespace zen::editor
{
namespace
{
TEST(EditorEnvironment, CatalogFiltersExtensionsRecursivelyAndHandlesMissingDirectories)
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "ZenEditor-environment-catalog";

    std::filesystem::remove_all(directory);

    std::filesystem::create_directories(directory / "nested");

    ASSERT_TRUE(WriteSyntheticPanorama(directory / "studio_small_09.hdr", 64));

    // Discovery filters extensions only; the renderer validates contents when selected.
    for (const char* name : {"Sky-Dome.HDR", "nested/papermill_copy.ktx", "cubemap.dds", "notes.txt"})
    {
        std::ofstream(directory / name, std::ios::binary) << "data";
    }

    const HeapVector<EnvironmentTextureItem> items = DiscoverEnvironmentTextures(PathToUtf8(directory));

    ASSERT_EQ(items.size(), 4u);

    HeapVector<std::string> labels;

    for (const EnvironmentTextureItem& item : items)
    {
        EXPECT_TRUE(std::filesystem::is_regular_file(std::filesystem::u8path(item.path)));

        EXPECT_EQ(item.label.find('_'), std::string::npos);

        labels.push_back(item.label);
    }

    for (const char* label : {"Studio Small 09", "Sky Dome", "Papermill Copy", "Cubemap"})
    {
        EXPECT_NE(std::find(labels.begin(), labels.end(), label), labels.end()) << label;
    }

    EXPECT_TRUE(DiscoverEnvironmentTextures(PathToUtf8(directory / "missing-environment-folder")).empty());

    EXPECT_EQ(EnvironmentTextureLabel(""), "Scene / engine default");

    std::filesystem::remove_all(directory);
}

TEST(EditorEnvironment, PanoramasDecodeToFiniteLinearCubemaps)
{
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "ZenEditor-environment-panorama.hdr";

    for (uint32_t width : {64u, 256u})
    {
        ASSERT_TRUE(WriteSyntheticPanorama(file, width));

        const std::string path = PathToUtf8(file);

        uint32_t size          = 0;

        HeapVector<Vec4> pixels;

        std::string error;

        ASSERT_TRUE(asset::TextureLoader::LoadHDRCubemap(path, size, pixels, error)) << path << ": " << error;

        EXPECT_EQ(size, width / 4);

        EXPECT_EQ(pixels.size(), size_t(size) * size * 6);

        float peak = 0;

        for (const Vec4& pixel : pixels)
        {
            ASSERT_TRUE(std::isfinite(pixel.x) && std::isfinite(pixel.y) && std::isfinite(pixel.z));

            ASSERT_TRUE(pixel.x >= 0 && pixel.y >= 0 && pixel.z >= 0 && pixel.w == 1);

            peak = std::max(peak, std::max(pixel.x, std::max(pixel.y, pixel.z)));
        }

        EXPECT_GT(peak, 1.0f);
    }

    std::filesystem::remove(file);
}

TEST(EditorEnvironment, ConversionPreservesHDRAndRejectsNonPanoramasWithoutStaleOutput)
{
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "ZenEditor-environment-conversion.hdr";

    {
        std::ofstream stream(file, std::ios::binary);

        stream << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 4\n";

        const unsigned char pixel[] = {128, 64, 32, 131}; // linear RGB = 4, 2, 1

        for (uint32_t index = 0; index < 8; ++index)
        {
            stream.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));
        }
    }

    uint32_t size = 0;

    HeapVector<Vec4> pixels;

    std::string error;

    ASSERT_TRUE(asset::TextureLoader::LoadHDRCubemap(PathToUtf8(file), size, pixels, error)) << error;

    ASSERT_EQ(pixels.size(), 6u);

    for (const Vec4& pixel : pixels)
    {
        EXPECT_EQ(pixel, Vec4(4, 2, 1, 1));
    }

    {
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);

        stream << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 4\n";

        const unsigned char sky[]    = {128, 0, 0, 131};

        const unsigned char ground[] = {0, 128, 0, 131};

        for (uint32_t index = 0; index < 8; ++index)
        {
            stream.write(reinterpret_cast<const char*>(index < 4 ? sky : ground), 4);
        }
    }

    ASSERT_TRUE(asset::TextureLoader::LoadHDRCubemap(PathToUtf8(file), size, pixels, error)) << error;

    // World +Y samples source -Y in both skybox.frag and filtercube.vert.
    EXPECT_EQ(pixels[3], Vec4(4, 0, 0, 1));

    EXPECT_EQ(pixels[2], Vec4(0, 4, 0, 1));

    {
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);

        stream << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 4\n";
    }

    EXPECT_FALSE(asset::TextureLoader::LoadHDRCubemap(PathToUtf8(file), size, pixels, error));

    EXPECT_FALSE(error.empty());

    EXPECT_EQ(size, 0u);

    EXPECT_TRUE(pixels.empty());

    EXPECT_FALSE(asset::TextureLoader::LoadHDRCubemap(std::string(ZEN_TEXTURE_PATH) + "wood.png", size, pixels, error));

    std::filesystem::remove(file);
}
} // namespace
} // namespace zen::editor
