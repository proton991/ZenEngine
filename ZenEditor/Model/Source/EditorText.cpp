#include "Editor/Model/EditorText.h"
#include <cctype>

namespace zen::editor
{
namespace
{
std::string Lower(std::string value)
{
    for (char& character : value)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }

    return value;
}
} // namespace

bool MatchesSearch(const std::string& text, const std::string& search)
{
    return search.empty() || Lower(text).find(Lower(search)) != std::string::npos;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();

    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}
} // namespace zen::editor
