#pragma once
#include "Component.h"
#include "AssetLib/Types.h"
#include <vector>

namespace zen::sg
{
class Texture : public Component
{
public:
    Texture() = default;

    Texture(std::string name) : Component(std::move(name)) {}

    Texture(std::string name,
            // props
            uint32_t index,
            uint32_t width,
            uint32_t height,
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
        index        = index_;
        samplerIndex = info.samplerIndex;
        width        = info.width;
        height       = info.height;
        format       = info.format;
        bytesData    = std::move(info.data);
    }

    TypeId GetTypeId() const override
    {
        return typeid(Texture);
    }

    uint32_t index{0};
    int samplerIndex{-1};
    uint32_t width{0};
    uint32_t height{0};
    asset::Format format{asset::Format::UNDEFINED};
    // byte data no mipmaps
    std::vector<uint8_t> bytesData;
};

inline bool operator==(const Texture& lhs, const Texture& rhs)
{
    // Compare the name
    //    if (lhs.GetName() != rhs.GetName())
    //        return false;

    // Compare the index
    if (lhs.index != rhs.index)
        return false;

    // Compare samplerIndex
    if (lhs.samplerIndex != rhs.samplerIndex)
        return false;

    // Compare width and height
    if (lhs.width != rhs.width || lhs.height != rhs.height)
        return false;

    // Compare format
    if (lhs.format != rhs.format)
        return false;

    // Compare the byte data (check if sizes and contents match)
    if (lhs.bytesData.size() != rhs.bytesData.size())
        return false;

    for (size_t i = 0; i < lhs.bytesData.size(); ++i)
    {
        if (lhs.bytesData[i] != rhs.bytesData[i])
            return false;
    }

    return true;
}

inline bool operator!=(const Texture& lhs, const Texture& rhs)
{
    return !(lhs == rhs);
}
} // namespace zen::sg