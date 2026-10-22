#include "AssetLib/AssetHash.h"
#include "AssetLib/SceneImporter.h"
#include <cstdio>
#include <filesystem>

namespace zen::asset
{
std::string DigestText(AssetDigest digest)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string           text(16, '0');
    for (size_t i = 0; i < 16; ++i)
    {
        text[i] = digits[(digest >> ((15 - i) * 4)) & 15];
    }
    return text;
}

bool HashAssetFile(const std::string& path, AssetDigest& digest, SceneImportControl* control)
{
    std::error_code             error;
    const std::filesystem::path file   = std::filesystem::u8path(path);
    FILE*                       stream = nullptr;
    if (std::filesystem::is_regular_file(file, error) && (control == nullptr || !control->IsCancelled()))
    {
#if defined(_WIN32)
        stream = _wfopen(file.c_str(), L"rb");
#else
        stream = std::fopen(file.c_str(), "rb");
#endif
    }
    bool valid = stream != nullptr;
    if (valid)
    {
        AssetHash           hash;
        HeapVector<uint8_t> buffer;
        buffer.resize_for_overwrite(size_t(4) << 20);
        size_t transferred = buffer.size();
        while (valid && transferred == buffer.size())
        {
            transferred = std::fread(buffer.data(), 1, buffer.size(), stream);
            hash.Update(buffer.data(), transferred);
            valid = std::ferror(stream) == 0 && (control == nullptr || !control->IsCancelled());
        }
        valid &= std::fclose(stream) == 0;
        if (valid)
        {
            digest = hash.Finish();
        }
    }
    return valid;
}

bool StampAssetFile(const std::string& path, AssetFileStamp& stamp)
{
    std::error_code                       sizeError;
    std::error_code                       timeError;
    const std::filesystem::path           file     = std::filesystem::u8path(path);
    const bool                            regular  = std::filesystem::is_regular_file(file, sizeError);
    const uintmax_t                       size     = regular ? std::filesystem::file_size(file, sizeError) : 0;
    const std::filesystem::file_time_type modified = std::filesystem::last_write_time(file, timeError);
    const bool                            valid    = regular && !sizeError && !timeError;
    if (valid)
    {
        stamp.size     = static_cast<uint64_t>(size);
        stamp.modified = static_cast<int64_t>(modified.time_since_epoch().count());
    }
    return valid;
}
} // namespace zen::asset
