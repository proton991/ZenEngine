#pragma once
#include "Component.h"
#include "AssetLib/Types.h"
#include <vector>
#include <algorithm>
#include "Templates/HeapVector.h"
#include "AssetLib/ImageAsset.h"

namespace zen::sg
{
class Texture : public Component
{
public:
    Texture() = default;

    Texture(std::string name) : Component(std::move(name)) {}

    Texture(std::string name,
            // props
            uint32_t      index,
            uint32_t      width,
            uint32_t      height,
            asset::Format format,
            // moved
            std::vector<uint8_t> data,
            // optional
            int samplerIndex = -1) :
        Component(std::move(name)),
        index(index),
        samplerIndex(samplerIndex),
        width(width),
        height(height),
        format(format),
        bytesData(std::move(data))
    {}

    void Init(uint32_t index_, const asset::TextureInfo& info)
    {
        index             = index_;
        linearSourceIndex = UINT32_MAX;
        samplerIndex      = info.samplerIndex;
        width             = info.width;
        height            = info.height;
        format            = info.format;
        interpretation    = asset::TextureInterpretation::Default;
        arrayLayer        = 0;
        atlasRegion       = {};
        bytesData         = std::move(info.data);
        pixels.Reset();
        mipBytes.clear();
    }

    TypeId GetTypeId() const override
    {
        return typeid(Texture);
    }

    VectorView<const uint8_t> GetBytes() const
    {
        return !bytesData.empty() || !pixels ? VectorView<const uint8_t>(bytesData.data(), bytesData.size())
                                             : VectorView<const uint8_t>(pixels->bytes.data(), pixels->bytes.size());
    }

    const HeapVector<HeapVector<uint8_t>>& GetMipBytes() const
    {
        return !bytesData.empty() || !mipBytes.empty() || !pixels ? mipBytes : pixels->mips;
    }

    bool UsesSharedPixels() const
    {
        return pixels && bytesData.empty() && mipBytes.empty();
    }

    uint32_t GetArrayLayers() const
    {
        return UsesSharedPixels() ? pixels->arrayLayers : 1;
    }

    uint32_t GetArrayLayer() const
    {
        return UsesSharedPixels() ? arrayLayer : 0;
    }

    asset::ImageRegion GetAtlasRegion() const
    {
        return UsesSharedPixels() ? atlasRegion : asset::ImageRegion{};
    }

    // Borrow only this logical view's layer. GetBytes/GetMipBytes retain whole
    // page storage for uploads; previews must not decode a neighboring layer.
    VectorView<const uint8_t> GetLayerMipBytes(uint32_t level) const
    {
        VectorView<const uint8_t>              bytes;
        const HeapVector<HeapVector<uint8_t>>& levels = GetMipBytes();
        if (level < levels.size())
        {
            bytes = VectorView<const uint8_t>(levels[level].data(), levels[level].size());
        }
        else if (level == 0 && levels.empty())
        {
            bytes = GetBytes();
        }
        const uint32_t            layers = GetArrayLayers();
        const uint32_t            layer  = GetArrayLayer();
        VectorView<const uint8_t> result;
        if (layers > 0 && layer < layers && !bytes.empty() && bytes.size() % layers == 0 && (layers == 1 || !levels.empty()))
        {
            const size_t size = bytes.size() / layers;
            result            = VectorView<const uint8_t>(bytes.data() + size * layer, size);
        }
        return result;
    }

    // Imported scenes share immutable pixels across texture views and instances.
    SharedPtr<const asset::ImagePixels, MultiThreadCounter> pixels;

    uint32_t index{0};
    // Importer-generated linear view of another texture in this scene; authored textures have no source.
    uint32_t                     linearSourceIndex{UINT32_MAX};
    int                          samplerIndex{-1};
    uint32_t                     width{0};
    uint32_t                     height{0};
    asset::Format                format{asset::Format::UNDEFINED};
    asset::TextureInterpretation interpretation{asset::TextureInterpretation::Default};
    uint32_t                     arrayLayer{0};
    asset::ImageRegion           atlasRegion;
    // byte data no mipmaps
    std::vector<uint8_t> bytesData;
    // Authored image levels, including level zero; empty means generate from bytesData.
    HeapVector<HeapVector<uint8_t>> mipBytes;
    // The scene's stand-in for a missing normal map. Eight bits cannot store a flat
    // normal exactly (127/255 decodes to -0.0039, tilting it 0.32 degrees), so
    // materials bound to it publish no normal map and shade with the vertex normal.
    bool flatNormal{false};
};

inline bool operator==(const Texture& lhs, const Texture& rhs)
{
    bool equal = lhs.index == rhs.index && lhs.linearSourceIndex == rhs.linearSourceIndex
              && lhs.samplerIndex == rhs.samplerIndex && lhs.width == rhs.width && lhs.height == rhs.height
              && lhs.format == rhs.format && lhs.interpretation == rhs.interpretation
              && lhs.GetArrayLayers() == rhs.GetArrayLayers() && lhs.GetArrayLayer() == rhs.GetArrayLayer()
              && lhs.GetAtlasRegion() == rhs.GetAtlasRegion()
              && std::equal(lhs.GetBytes().begin(), lhs.GetBytes().end(), rhs.GetBytes().begin(), rhs.GetBytes().end())
              && lhs.GetMipBytes().size() == rhs.GetMipBytes().size() && lhs.flatNormal == rhs.flatNormal;

    for (size_t level = 0; equal && level < lhs.GetMipBytes().size(); ++level)
    {
        const HeapVector<uint8_t>& left  = lhs.GetMipBytes()[level];

        const HeapVector<uint8_t>& right = rhs.GetMipBytes()[level];

        equal                            = left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
    }

    return equal;
}

inline bool operator!=(const Texture& lhs, const Texture& rhs)
{
    return !(lhs == rhs);
}
} // namespace zen::sg
