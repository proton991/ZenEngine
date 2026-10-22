#pragma once
#include <cstdint>

namespace zen::asset
{
// IDs are local to one asset. Names and IDs are not persistent editor identities.
template <class Tag> struct AssetId
{
    uint32_t value{UINT32_MAX};

    bool IsValid() const
    {
        return value != UINT32_MAX;
    }

    bool operator==(const AssetId&) const = default;
};

using NodeId     = AssetId<struct NodeTag>;
using MeshId     = AssetId<struct MeshTag>;
using MaterialId = AssetId<struct MaterialTag>;
using ImageId    = AssetId<struct ImageTag>;
using TextureId  = AssetId<struct TextureTag>;
using SamplerId  = AssetId<struct SamplerTag>;
using SkinId     = AssetId<struct SkinTag>;
using CameraId   = AssetId<struct CameraTag>;
using LightId    = AssetId<struct LightTag>;
} // namespace zen::asset
