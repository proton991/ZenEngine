#pragma once

#include "Component.h"
#include "Math/Math.h"
#include "Templates/HeapVector.h"
#include "AssetLib/Types.h"

namespace zen::sg
{
// Camera instances belong to scene nodes; interactive Camera objects consume this data.
class SceneCamera : public Component
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
    bool     orthographic{false};
    bool     infiniteFar{false};
    bool     fixedAspect{false};
    float    aspect{1.0f};
    float    verticalFov{glm::radians(70.0f)};
    float    nearPlane{0.001f};
    float    farPlane{100.0f};
    float    xmag{1.0f};
    float    ymag{1.0f};
};

struct SkinAsset
{
    std::string          name;
    int32_t              skeleton{-1};
    HeapVector<uint32_t> joints;
    HeapVector<Mat4>     inverseBindMatrices;
};

enum class AnimationPath
{
    Translation,
    Rotation,
    Scale,
    Weights,
    Pointer
};

enum class AnimationInterpolation
{
    Linear,
    Step,
    CubicSpline
};

struct AnimationSampler
{
    AnimationInterpolation interpolation{AnimationInterpolation::Linear};
    uint32_t               components{0};
    HeapVector<float>      times;
    // Cubic splines retain in-tangent, value, out-tangent triples.
    HeapVector<float> values;
};

struct AnimationChannel
{
    uint32_t      sampler{0};
    uint32_t      node{0};
    AnimationPath path{AnimationPath::Translation};
    std::string   pointer;
};

struct AnimationAsset
{
    std::string                  name;
    HeapVector<AnimationSampler> samplers;
    HeapVector<AnimationChannel> channels;
};

struct MorphColorAttributeAsset
{
    uint64_t         set{0};
    HeapVector<Vec4> values;
};

struct MorphTargetAsset
{
    HeapVector<Vec3> positions;
    HeapVector<Vec3> normals;
    HeapVector<Vec3> tangents;
    // Coordinate sets use SceneAssetData::vertexTexCoordSets compact slots.
    HeapVector<HeapVector<Vec2>>         texCoords;
    HeapVector<Vec4>                     colors;
    HeapVector<MorphColorAttributeAsset> extraColors;
};

struct MorphPrimitiveAsset
{
    uint32_t                     mesh{0};
    uint32_t                     primitive{0};
    HeapVector<float>            weights;
    HeapVector<MorphTargetAsset> targets;
};

struct VertexJointInfluence
{
    uint32_t joint{0};
    float    weight{0.0f};
};

struct DeformationPrimitiveAsset
{
    uint32_t                                     node{0};
    uint32_t                                     firstVertex{0};
    uint32_t                                     vertexCount{0};
    uint32_t                                     subMesh{0};
    bool                                         flatNormals{false};
    int32_t                                      morphPrimitive{-1};
    HeapVector<uint32_t>                         sourceVertices;
    HeapVector<HeapVector<VertexJointInfluence>> influences;
};

struct MaterialVariantPrimitiveAsset
{
    uint32_t mesh{0};
    uint32_t primitive{0};
    // -1 retains the primitive's default material for that variant.
    HeapVector<int32_t> materials;
};

struct VertexAttributeAsset
{
    uint32_t    mesh{0};
    uint32_t    primitive{0};
    std::string semantic;
    // -1 identifies base attributes; otherwise this is a primitive-local morph target.
    int32_t  morphTarget{-1};
    uint32_t components{0};
    uint32_t componentType{0};
    bool     normalized{false};
    // Double precision retains integer custom attributes exactly and decoded normalized values.
    HeapVector<double> values;
};

struct ImageBasedLightAsset
{
    std::string      name;
    float            intensity{1.0f};
    Quat             rotation{1.0f, 0.0f, 0.0f, 0.0f};
    HeapVector<Vec3> irradianceCoefficients;
    uint32_t         size{0};
    uint32_t         mipLevels{0};
    // Linear HDR RGBA pixels, indexed by mip * 6 + (+X, -X, +Y, -Y, +Z, -Z).
    HeapVector<HeapVector<Vec4>> specularMipFaces;
};

struct SceneAssetData
{
    // Renderer-world units per authored glTF meter, accumulated by normalization.
    float unitScale{1.0f};

    // Retain extras and optional/vendor extensions for editor round-tripping.
    std::string                           sourceDocument;
    HeapVector<SkinAsset>                 skins;
    HeapVector<AnimationAsset>            animations;
    HeapVector<MorphPrimitiveAsset>       morphPrimitives;
    HeapVector<DeformationPrimitiveAsset> deformations;
    HeapVector<HeapVector<Vec2>>          vertexTexCoords;
    // Original TEXCOORD_n semantic IDs for compact coordinate slots in vertexTexCoords.
    HeapVector<uint64_t>                      vertexTexCoordSets;
    HeapVector<asset::Vertex>                 bindVertices;
    HeapVector<HeapVector<Vec2>>              bindVertexTexCoords;
    HeapVector<std::string>                   materialVariants;
    HeapVector<MaterialVariantPrimitiveAsset> variantPrimitives;
    HeapVector<VertexAttributeAsset>          extraVertexAttributes;
    HeapVector<ImageBasedLightAsset>          imageBasedLights;
    int32_t                                   imageBasedLight{-1};
};
} // namespace zen::sg
