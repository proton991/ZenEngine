#pragma once
#include "AssetLib/AnimationTarget.h"
#include "AssetLib/MaterialAsset.h"
#include "Templates/VectorView.h"

namespace zen::asset
{
template <class TextureReference>
VectorView<float> GetAnimatedMaterialProperty(MaterialParametersT<TextureReference>& material, AnimatedProperty property)
{
    VectorView<float> result;
    switch (property)
    {
        case AnimatedProperty::BaseColor: result = VectorView<float>(&material.baseColorFactor[0], 4); break;
        case AnimatedProperty::Metallic: result = VectorView<float>(&material.metallicFactor, 1); break;
        case AnimatedProperty::Roughness: result = VectorView<float>(&material.roughnessFactor, 1); break;
        case AnimatedProperty::Emissive: result = VectorView<float>(&material.emissiveFactor[0], 3); break;
        case AnimatedProperty::AlphaCutoff: result = VectorView<float>(&material.alphaCutoff, 1); break;
        case AnimatedProperty::NormalScale: result = VectorView<float>(&material.normalScale, 1); break;
        case AnimatedProperty::OcclusionStrength: result = VectorView<float>(&material.occlusionStrength, 1); break;
        case AnimatedProperty::EmissiveStrength: result = VectorView<float>(&material.emissiveStrength, 1); break;
        case AnimatedProperty::Ior: result = VectorView<float>(&material.features.ior, 1); break;
        case AnimatedProperty::Dispersion: result = VectorView<float>(&material.features.dispersion, 1); break;
        case AnimatedProperty::Specular: result = VectorView<float>(&material.features.specular, 1); break;
        case AnimatedProperty::SpecularColor: result = VectorView<float>(&material.features.specularColor[0], 3); break;
        case AnimatedProperty::Diffuse: result = VectorView<float>(&material.extension.diffuseFactor[0], 4); break;
        case AnimatedProperty::SpecularGlossinessColor:
            result = VectorView<float>(&material.extension.specularFactor[0], 3);
            break;
        case AnimatedProperty::Glossiness: result = VectorView<float>(&material.features.glossiness, 1); break;
        case AnimatedProperty::Clearcoat: result = VectorView<float>(&material.features.clearcoat, 1); break;
        case AnimatedProperty::ClearcoatRoughness: result = VectorView<float>(&material.features.clearcoatRoughness, 1); break;
        case AnimatedProperty::ClearcoatNormalScale:
            result = VectorView<float>(&material.features.clearcoatNormalTexture.scale, 1);
            break;
        case AnimatedProperty::SheenColor: result = VectorView<float>(&material.features.sheenColor[0], 3); break;
        case AnimatedProperty::SheenRoughness: result = VectorView<float>(&material.features.sheenRoughness, 1); break;
        case AnimatedProperty::Transmission: result = VectorView<float>(&material.features.transmission, 1); break;
        case AnimatedProperty::Thickness: result = VectorView<float>(&material.features.thickness, 1); break;
        case AnimatedProperty::AttenuationDistance:
            result = VectorView<float>(&material.features.attenuationDistance, 1);
            break;
        case AnimatedProperty::AttenuationColor: result = VectorView<float>(&material.features.attenuationColor[0], 3); break;
        case AnimatedProperty::Iridescence: result = VectorView<float>(&material.features.iridescence, 1); break;
        case AnimatedProperty::IridescenceIor: result = VectorView<float>(&material.features.iridescenceIor, 1); break;
        case AnimatedProperty::IridescenceThicknessMinimum:
            result = VectorView<float>(&material.features.iridescenceThicknessMin, 1);
            break;
        case AnimatedProperty::IridescenceThicknessMaximum:
            result = VectorView<float>(&material.features.iridescenceThicknessMax, 1);
            break;
        case AnimatedProperty::Anisotropy: result = VectorView<float>(&material.features.anisotropy, 1); break;
        case AnimatedProperty::AnisotropyRotation: result = VectorView<float>(&material.features.anisotropyRotation, 1); break;
        case AnimatedProperty::DiffuseTransmission:
            result = VectorView<float>(&material.features.diffuseTransmission, 1);
            break;
        case AnimatedProperty::DiffuseTransmissionColor:
            result = VectorView<float>(&material.features.diffuseTransmissionColor[0], 3);
            break;
        case AnimatedProperty::MultiscatterColor: result = VectorView<float>(&material.features.multiscatterColor[0], 3); break;
        case AnimatedProperty::Retroreflection: result = VectorView<float>(&material.features.retroreflection, 1); break;
        default: break;
    }
    return result;
}
} // namespace zen::asset
