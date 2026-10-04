#pragma once
#include <filesystem>
#include <string>

namespace zen::editor
{
// Case-insensitive ASCII substring match; an empty search matches everything.
bool MatchesSearch(const std::string& text, const std::string& search);

// UTF-8 with forward slashes; never depends on the process code page.
std::string PathToUtf8(const std::filesystem::path& path);
} // namespace zen::editor
