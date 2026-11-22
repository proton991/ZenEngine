#include "AssetTools/SceneTools.h"
#include "Utils/Errors.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "AssetLib/SceneModelCatalog.h"
#include "AssetLib/SceneLoader.h"

using namespace zen;

namespace
{
std::string CatalogTestUtf8(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.generic_u8string();

    const std::string result(reinterpret_cast<const char*>(utf8.data()), utf8.size());

    return result;
}

class CatalogFixture
{
public:
    CatalogFixture()
    {
        const int64_t identifier = std::chrono::steady_clock::now().time_since_epoch().count();

        m_root = std::filesystem::temp_directory_path() / ("ZenSceneModelCatalogTests_" + std::to_string(identifier));

        if (!std::filesystem::create_directory(m_root))
        {
            VERIFY_EXPR_MSG(false, "Could not create glTF catalog fixture directory");
        }
    }

    ~CatalogFixture()
    {
        std::error_code error;

        std::filesystem::remove_all(m_root, error);
    }

    void Write(const std::filesystem::path& relative)
    {
        const std::filesystem::path path = m_root / relative;

        std::filesystem::create_directories(path.parent_path());

        std::ofstream file(path, std::ios::binary);

        file << "Discovery does not parse model contents.";

        if (!file)
        {
            VERIFY_EXPR_MSG(false, "Could not write glTF catalog fixture file");
        }
    }

    const std::filesystem::path& GetRoot() const
    {
        return m_root;
    }

private:
    std::filesystem::path m_root;
};
} // namespace

TEST(SceneModelCatalog, FindsRecursiveVariantsAndSortsRelativeLabels)
{
    CatalogFixture fixture;

    fixture.Write("Zebra/Zebra.gltf");
    fixture.Write("Interior/Room.FBX");

    fixture.Write("Alpha/glTF/Alpha.gltf");

    fixture.Write("Alpha/glTF-Binary/Alpha.GLB");

    fixture.Write("Alpha/glTF-Embedded/Alpha.glTF");

    fixture.Write("Alpha/glTF/Alpha.bin");

    fixture.Write("Alpha/readme.txt");

    std::filesystem::create_directory(fixture.GetRoot() / "directory.gltf");

    const HeapVector<asset::SceneModelCatalogEntry> catalog =
        asset::DiscoverSourceSceneModels(CatalogTestUtf8(fixture.GetRoot()));

    ASSERT_EQ(catalog.size(), 5u);

    EXPECT_EQ(catalog[0].label, "Alpha/glTF-Binary/Alpha.GLB");

    EXPECT_EQ(catalog[1].label, "Alpha/glTF-Embedded/Alpha.glTF");

    EXPECT_EQ(catalog[2].label, "Alpha/glTF/Alpha.gltf");

    EXPECT_EQ(catalog[3].label, "Interior/Room.FBX");
    EXPECT_EQ(catalog[4].label, "Zebra/Zebra.gltf");

    for (const asset::SceneModelCatalogEntry& entry : catalog)
    {
        const std::filesystem::path path = std::filesystem::u8path(entry.path);

        EXPECT_TRUE(path.is_absolute());

        EXPECT_TRUE(std::filesystem::is_regular_file(path));

        EXPECT_EQ(entry.path, CatalogTestUtf8((fixture.GetRoot() / std::filesystem::u8path(entry.label)).lexically_normal()));

        EXPECT_EQ(entry.path.find('\\'), std::string::npos);
    }
}

TEST(SceneModelCatalog, DiscoveryAndFileFiltersCoverEveryRegisteredLoadFormat)
{
    CatalogFixture    fixture;
    size_t            expected = 0;
    const std::string filters  = ";" + asset::GetSourceSceneFileFilterExtensions() + ";";
    for (const asset::SceneFormatDescriptor& format : asset::GetSourceSceneLoadFormats())
    {
        for (std::string_view extension : format.extensions)
        {
            const std::string name = "file" + std::string(extension);
            fixture.Write(name);
            EXPECT_NE(filters.find(";" + std::string(extension.substr(1)) + ";"), std::string::npos);
            EXPECT_NE(asset::FindSourceSceneLoadFormat(name), nullptr);
            ++expected;
        }
    }
    fixture.Write("unsupported.unknown");
    EXPECT_EQ(asset::FindSourceSceneLoadFormat("unsupported.unknown"), nullptr);
    EXPECT_EQ(asset::DiscoverSourceSceneModels(CatalogTestUtf8(fixture.GetRoot())).size(), expected);
}

TEST(SceneModelCatalog, UnicodeRootsAndNamesReturnNormalizedUtf8LoadPaths)
{
    CatalogFixture fixture;

    const std::filesystem::path root     = fixture.GetRoot() / std::filesystem::path(u8"根目录❤");

    const std::filesystem::path relative = std::filesystem::path(u8"模型/示例❤.GLB");

    fixture.Write(std::filesystem::path(u8"根目录❤") / relative);

    std::filesystem::create_directory(root / "unused");

    const std::filesystem::path supplied                    = root / "unused" / "..";

    const HeapVector<asset::SceneModelCatalogEntry> catalog = asset::DiscoverSourceSceneModels(CatalogTestUtf8(supplied));

    ASSERT_EQ(catalog.size(), 1u);

    EXPECT_EQ(catalog[0].label, CatalogTestUtf8(relative));

    EXPECT_EQ(catalog[0].path, CatalogTestUtf8((root / relative).lexically_normal()));

    EXPECT_TRUE(std::filesystem::is_regular_file(std::filesystem::u8path(catalog[0].path)));
}

TEST(SceneModelCatalog, MissingEmptyAndFileRootsReturnEmptyCatalogs)
{
    CatalogFixture fixture;

    fixture.Write("file.gltf");

    EXPECT_TRUE(asset::DiscoverSourceSceneModels("").empty());

    EXPECT_TRUE(asset::DiscoverSourceSceneModels(CatalogTestUtf8(fixture.GetRoot() / "missing")).empty());

    EXPECT_TRUE(asset::DiscoverSourceSceneModels(CatalogTestUtf8(fixture.GetRoot() / "file.gltf")).empty());
}

TEST(SceneModelCatalog, DirectorySymlinksDoNotDiscoverModelsOutsideTheRoot)
{
    CatalogFixture fixture;

    fixture.Write("Elsewhere/model.gltf");

    const std::filesystem::path root = fixture.GetRoot() / "Models";

    std::filesystem::create_directory(root);

    std::error_code error;

    std::filesystem::create_directory_symlink(fixture.GetRoot() / "Elsewhere", root / "Shortcut", error);

    if (error)
    {
        GTEST_SKIP() << "Directory symlink creation is unavailable: " << error.message();
    }

    EXPECT_TRUE(asset::DiscoverSourceSceneModels(CatalogTestUtf8(root)).empty());
}
