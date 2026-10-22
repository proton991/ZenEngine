#include "AssetLib/SceneModelCatalog.h"
#include "AssetLib/SceneLoader.h"
#include "AssetLib/AssetPaths.h"
#include <algorithm>
#include <filesystem>

namespace zen::asset
{
HeapVector<SceneModelCatalogEntry> DiscoverSceneModels(const std::string& basePath)
{
    HeapVector<SceneModelCatalogEntry> result;

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

                if (entry.is_regular_file(entryError) && !entryError
                    && FindSceneLoadFormat(PathToUtf8(entry.path())) != nullptr)
                {
                    const std::filesystem::path path = entry.path().lexically_normal();

                    result.push_back({PathToUtf8(path.lexically_relative(root)), PathToUtf8(path)});
                }

                iterator.increment(error);
            }
        }
    }

    if (result.size() > 1)
    {
        std::sort(result.begin(), result.end(), [](const SceneModelCatalogEntry& left, const SceneModelCatalogEntry& right) {
            return left.label == right.label ? left.path < right.path : left.label < right.label;
        });
    }

    return result;
}
} // namespace zen::asset
