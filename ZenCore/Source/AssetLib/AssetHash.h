#pragma once
#include "AssetLib/Kernels/AssetKernels.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace zen::asset
{
class SceneImportControl;
using AssetDigest = uint64_t;

// Streaming XXH64 fingerprints for cache payloads and source files. They detect
// stale or corrupt data quickly; caches are local engine state, not a trust boundary.
class AssetHash
{
public:
    AssetHash()
    {
        kernels::ResetHash(m_state, 0);
    }

    void Update(const void* data, size_t size)
    {
        kernels::UpdateHash(m_state, data, size);
    }

    AssetDigest Finish() const
    {
        return kernels::FinishHash(m_state);
    }

private:
    kernels::HashState m_state;
};

// Sixteen lowercase hexadecimal digits.
std::string DigestText(AssetDigest digest);
bool        HashAssetFile(const std::string& path, AssetDigest& digest, SceneImportControl* control = nullptr);

// Size and last-write time identify a file revision without reading its bytes.
struct AssetFileStamp
{
    uint64_t size{0};
    int64_t  modified{0};

    bool operator==(const AssetFileStamp&) const = default;
};

bool StampAssetFile(const std::string& path, AssetFileStamp& stamp);
} // namespace zen::asset
