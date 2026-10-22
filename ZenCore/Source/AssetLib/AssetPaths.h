#pragma once
#include <filesystem>
#include <string>

namespace zen::asset
{
// UTF-8 text of a path with forward slashes: the form stored in assets and caches.
inline std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// Absolute, lexically normalized UTF-8 path; empty when it cannot be resolved.
inline std::string AbsoluteUtf8Path(const std::filesystem::path& path)
{
    std::error_code             error;
    const std::filesystem::path absolute = std::filesystem::absolute(path, error).lexically_normal();
    return error ? std::string() : PathToUtf8(absolute);
}
} // namespace zen::asset
