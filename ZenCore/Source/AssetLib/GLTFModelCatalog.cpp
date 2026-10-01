#include "AssetLib/GLTFModelCatalog.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace zen::asset
{
static std::string CatalogPathUtf8(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.generic_u8string();

    const std::string result(reinterpret_cast<const char*>(utf8.data()), utf8.size());

    return result;
}

static bool IsGLTFModelPath(const std::filesystem::path& path)
{
    std::string extension = CatalogPathUtf8(path.extension());

    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });

    const bool result = extension == ".gltf" || extension == ".glb";

    return result;
}

HeapVector<GLTFModelCatalogEntry> DiscoverGLTFModels(const std::string& basePath)
{
    HeapVector<GLTFModelCatalogEntry> result;

    if (!basePath.empty())
    {
        std::error_code error;

        const std::filesystem::path root =
            std::filesystem::absolute(std::filesystem::u8path(basePath), error).lexically_normal();

        if (!error && std::filesystem::is_directory(root, error))
        {
            std::filesystem::recursive_directory_iterator iterator(
                root, std::filesystem::directory_options::skip_permission_denied, error);

            const std::filesystem::recursive_directory_iterator end;

            while (!error && iterator != end)
            {
                const std::filesystem::directory_entry& entry = *iterator;

                std::error_code entryError;

                if (entry.is_symlink(entryError))
                {
                    iterator.disable_recursion_pending();
                }

                if (entry.is_regular_file(entryError) && !entryError &&
                    IsGLTFModelPath(entry.path()))
                {
                    const std::filesystem::path path = entry.path().lexically_normal();

                    result.push_back(
                        {CatalogPathUtf8(path.lexically_relative(root)), CatalogPathUtf8(path)});
                }

                iterator.increment(error);
            }
        }
    }

    if (result.size() > 1)
    {
        std::sort(result.begin(), result.end(),
                  [](const GLTFModelCatalogEntry& left, const GLTFModelCatalogEntry& right) {
                      return left.label == right.label ? left.path < right.path :
                                                         left.label < right.label;
                  });
    }

    return result;
}
} // namespace zen::asset
