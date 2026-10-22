#pragma once
#include "Component.h"
#include "AssetLib/ImageAsset.h"

namespace zen::sg
{
using TextureFilter      = asset::TextureFilter;
using SamplerAddressMode = asset::SamplerAddressMode;

class Sampler : public Component, public asset::SamplerParameters
{
public:
    Sampler(std::string name) : Component(std::move(name)) {}

    TypeId GetTypeId() const override
    {
        return typeid(Sampler);
    }
};
} // namespace zen::sg
