#include "Editor/Model/EditorEnvironment.h"
#include "Editor/Model/EditorText.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace zen::editor
{
std::string EnvironmentTextureLabel(const std::string& path)
{
    std::string label = "Scene / engine default";

    if (!path.empty())
    {
        label           = PathToUtf8(std::filesystem::u8path(path).stem());

        bool capitalize = true;

        for (char& character : label)
        {
            if (character == '_' || character == '-')
            {
                character = ' ';
            }

            if (capitalize)
            {
                character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
            }

            capitalize = character == ' ';
        }
    }

    return label;
}

HeapVector<EnvironmentTextureItem> DiscoverEnvironmentTextures(const std::string& directory)
{
    HeapVector<EnvironmentTextureItem> result;

    std::error_code error;

    std::filesystem::recursive_directory_iterator entry(std::filesystem::u8path(directory),
                                                        std::filesystem::directory_options::skip_permission_denied, error);

    const std::filesystem::recursive_directory_iterator end;

    while (!error && entry != end)
    {
        if (entry->is_regular_file(error))
        {
            std::string extension = entry->path().extension().string();

            for (char& character : extension)
            {
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            }

            if (extension == ".hdr" || extension == ".ktx" || extension == ".dds")
            {
                const std::string path = PathToUtf8(std::filesystem::absolute(entry->path(), error));

                if (!error)
                {
                    result.push_back({EnvironmentTextureLabel(path), path});
                }
            }
        }

        entry.increment(error);
    }

    std::sort(result.begin(), result.end(),
              [](const EnvironmentTextureItem& left, const EnvironmentTextureItem& right) { return left.path < right.path; });

    return result;
}
} // namespace zen::editor
