#include "Utils/Errors.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "AssetLib/GLTFModelCatalog.h"

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

        m_root = std::filesystem::temp_directory_path() / ("ZenGLTFModelCatalogTests_" + std::to_string(identifier));

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

TEST(GLTFModelCatalog, FindsRecursiveVariantsAndSortsRelativeLabels)
{
    CatalogFixture fixture;

    fixture.Write("Zebra/Zebra.gltf");

    fixture.Write("Alpha/glTF/Alpha.gltf");

    fixture.Write("Alpha/glTF-Binary/Alpha.GLB");

    fixture.Write("Alpha/glTF-Embedded/Alpha.glTF");

    fixture.Write("Alpha/glTF/Alpha.bin");

    fixture.Write("Alpha/readme.txt");

    std::filesystem::create_directory(fixture.GetRoot() / "directory.gltf");

    const HeapVector<asset::GLTFModelCatalogEntry> catalog = asset::DiscoverGLTFModels(CatalogTestUtf8(fixture.GetRoot()));

    ASSERT_EQ(catalog.size(), 4u);

    EXPECT_EQ(catalog[0].label, "Alpha/glTF-Binary/Alpha.GLB");

    EXPECT_EQ(catalog[1].label, "Alpha/glTF-Embedded/Alpha.glTF");

    EXPECT_EQ(catalog[2].label, "Alpha/glTF/Alpha.gltf");

    EXPECT_EQ(catalog[3].label, "Zebra/Zebra.gltf");

    for (const asset::GLTFModelCatalogEntry& entry : catalog)
    {
        const std::filesystem::path path = std::filesystem::u8path(entry.path);

        EXPECT_TRUE(path.is_absolute());

        EXPECT_TRUE(std::filesystem::is_regular_file(path));

        EXPECT_EQ(entry.path, CatalogTestUtf8((fixture.GetRoot() / std::filesystem::u8path(entry.label)).lexically_normal()));

        EXPECT_EQ(entry.path.find('\\'), std::string::npos);
    }
}

TEST(GLTFModelCatalog, UnicodeRootsAndNamesReturnNormalizedUtf8LoadPaths)
{
    CatalogFixture fixture;

    const std::filesystem::path root     = fixture.GetRoot() / std::filesystem::path(u8"根目录❤");

    const std::filesystem::path relative = std::filesystem::path(u8"模型/示例❤.GLB");

    fixture.Write(std::filesystem::path(u8"根目录❤") / relative);

    std::filesystem::create_directory(root / "unused");

    const std::filesystem::path supplied                   = root / "unused" / "..";

    const HeapVector<asset::GLTFModelCatalogEntry> catalog = asset::DiscoverGLTFModels(CatalogTestUtf8(supplied));

    ASSERT_EQ(catalog.size(), 1u);

    EXPECT_EQ(catalog[0].label, CatalogTestUtf8(relative));

    EXPECT_EQ(catalog[0].path, CatalogTestUtf8((root / relative).lexically_normal()));

    EXPECT_TRUE(std::filesystem::is_regular_file(std::filesystem::u8path(catalog[0].path)));
}

TEST(GLTFModelCatalog, MissingEmptyAndFileRootsReturnEmptyCatalogs)
{
    CatalogFixture fixture;

    fixture.Write("file.gltf");

    EXPECT_TRUE(asset::DiscoverGLTFModels("").empty());

    EXPECT_TRUE(asset::DiscoverGLTFModels(CatalogTestUtf8(fixture.GetRoot() / "missing")).empty());

    EXPECT_TRUE(asset::DiscoverGLTFModels(CatalogTestUtf8(fixture.GetRoot() / "file.gltf")).empty());
}

TEST(GLTFModelCatalog, DirectorySymlinksDoNotDiscoverModelsOutsideTheRoot)
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

    EXPECT_TRUE(asset::DiscoverGLTFModels(CatalogTestUtf8(root)).empty());
}
