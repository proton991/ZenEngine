#pragma once
#include "Component.h"

namespace zen::sg
{
enum class TextureFilter : uint32_t
{
    Nearest = 0,
    Linear  = 1,
    MaxEnum = 0x7FFFFFFF
};

enum class SamplerAddressMode : uint32_t
{
    Repeat            = 0,
    MirroredRepeat    = 1,
    ClampToEdge       = 2,
    ClampToBorder     = 3,
    MirrorClampToEdge = 4,
    MaxEnum           = 0x7FFFFFFF
};

class Sampler : public Component
{
public:
    Sampler(std::string name) : Component(std::move(name)) {}

    TypeId GetTypeId() const override
    {
        return typeid(Sampler);
    }

    TextureFilter minFilter{TextureFilter::MaxEnum};
    TextureFilter magFilter{TextureFilter::MaxEnum};

    SamplerAddressMode wrapS{SamplerAddressMode::MaxEnum};
    SamplerAddressMode wrapT{SamplerAddressMode::MaxEnum};
};
} // namespace zen::sg