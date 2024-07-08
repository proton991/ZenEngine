#pragma once
#include "Component.h"
#include "Common/Types.h"
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
            Format format,
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

    TypeId GetTypeId() const override
    {
        return typeid(Texture);
    }

    uint32_t index{0};
    int samplerIndex{-1};
    uint32_t width{0};
    uint32_t height{0};
    Format format{Format::UNDEFINED};
    // byte data no mipmaps
    std::vector<uint8_t> bytesData;
};
} // namespace zen::sg