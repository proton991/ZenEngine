#pragma once

#include "Component.h"
#include "AssetLib/SceneData.h"
#include "AssetLib/SceneAsset.h"
#include "Math/Math.h"
#include "Templates/HeapVector.h"
#include "Templates/SmallVector.h"
#include "AssetLib/Types.h"

namespace zen::sg
{
// Common meshes use one or two UV sets. Additional sets still grow dynamically.
using VertexTextureCoordinates = asset::VertexTextureCoordinates;

// Camera instances belong to scene nodes; interactive Camera objects consume this data.
class SceneCamera : public Component, public asset::CameraParameters
{
public:
    explicit SceneCamera(std::string name) : Component(std::move(name)) {}

    TypeId GetTypeId() const override
    {
        return typeid(SceneCamera);
    }

    Mat4     worldMatrix{1.0f};
    uint32_t sourceIndex{UINT32_MAX};
    float    unitScale{1.0f};
};

using SkinAsset                     = asset::SkinAsset;
using AnimationPath                 = asset::AnimationPath;
using AnimationInterpolation        = asset::AnimationInterpolation;
using AnimationSampler              = asset::AnimationSampler;
using AnimationChannel              = asset::AnimationChannel;
using AnimationAsset                = asset::AnimationAsset;
using MorphColorAttributeAsset      = asset::MorphColorAttributeAsset;
using MorphTargetAsset              = asset::MorphTargetAsset;
using MorphPrimitiveAsset           = asset::MorphPrimitiveAsset;
using VertexJointInfluence          = asset::VertexJointInfluence;
using DeformationPrimitiveAsset     = asset::DeformationPrimitiveAsset;
using MaterialVariantPrimitiveAsset = asset::MaterialVariantPrimitiveAsset;
using VertexAttributeAsset          = asset::VertexAttributeAsset;
using ImageBasedLightAsset          = asset::ImageBasedLightAsset;

// Facts about the canonical source that runtime animation targets still need after
// the CPU asset is released: inactive targets (nodes outside the selected scene,
// unused cameras and lights) remain valid, and matrix nodes reject TRS animation.
struct SceneSourceInfo
{
    // Optional source metadata retained for inspection and round-tripping.
    std::string               document;
    HeapVector<asset::MeshId> nodeMeshes;
    HeapVector<uint8_t>       nodeMatrixTransforms;
    uint32_t                  materials{0};
    uint32_t                  cameras{0};
    uint32_t                  lights{0};
    uint32_t                  imageBasedLights{0};
};

struct SceneAssetData
{
    SharedPtr<const SceneSourceInfo, MultiThreadCounter> source;
    // Renderer-world units per canonical meter, accumulated by normalization.
    float unitScale{1.0f};

    HeapVector<SkinAsset>                 skins;
    HeapVector<AnimationAsset>            animations;
    HeapVector<MorphPrimitiveAsset>       morphPrimitives;
    HeapVector<DeformationPrimitiveAsset> deformations;
    HeapVector<VertexTextureCoordinates>  vertexTexCoords;
    // Original TEXCOORD_n semantic IDs for compact coordinate slots in vertexTexCoords.
    HeapVector<uint64_t>                      vertexTexCoordSets;
    HeapVector<asset::Vertex>                 bindVertices;
    HeapVector<VertexTextureCoordinates>      bindVertexTexCoords;
    HeapVector<std::string>                   materialVariants;
    HeapVector<MaterialVariantPrimitiveAsset> variantPrimitives;
    HeapVector<VertexAttributeAsset>          extraVertexAttributes;
    HeapVector<ImageBasedLightAsset>          imageBasedLights;
    int32_t                                   imageBasedLight{-1};
};
} // namespace zen::sg
