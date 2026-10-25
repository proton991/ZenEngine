#pragma once
#include "Math/Math.h"
#include "AssetLib/AnimationTarget.h"
#include "AssetLib/Types.h"
#include "Utils/SharedPtr.h"
#include "Templates/HeapVector.h"
#include "Templates/SmallVector.h"
#include <string>

namespace zen::asset
{
using VertexTextureCoordinates = SmallVector<Vec2, 2>;

enum class MeshTopology
{
    Points,
    Lines,
    Triangles
};

struct CameraParameters
{
    bool  orthographic{false};
    bool  infiniteFar{false};
    bool  fixedAspect{false};
    float aspect{1.0f};
    float verticalFov{glm::radians(70.0f)};
    float nearPlane{0.001f};
    float farPlane{100.0f};
    float xmag{1.0f};
    float ymag{1.0f};
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
    Property
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
    uint32_t        sampler{0};
    uint32_t        node{0};
    AnimationPath   path{AnimationPath::Translation};
    AnimationTarget target;
    // Optional diagnostic metadata; playback never reads it.
    std::string sourceTarget;
};

struct AnimationBakeSettings
{
    double   sampleRate{60};
    float    translationTolerance{1e-5f};
    float    rotationTolerance{1e-4f};
    float    scaleTolerance{1e-5f};
    float    valueTolerance{1e-4f};
    uint32_t maxSubdivisions{10};
    uint64_t maxValues{64u * 1024u * 1024u};
};

struct AnimationAsset
{
    std::string                  name;
    HeapVector<AnimationSampler> samplers;
    HeapVector<AnimationChannel> channels;
    bool                         baked{false};
    AnimationBakeSettings        bakeSettings;
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
    uint32_t mesh{0};
    // UINT32_MAX applies a shared control-point payload to every mesh primitive.
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

enum class VertexAttributeDomain
{
    SourceVertex,
    PrimitiveVertex
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
    // SourceVertex follows the primitive's sourceVertices mapping; PrimitiveVertex
    // is already expanded to final corners (needed for FBX per-corner color sets).
    VertexAttributeDomain domain{VertexAttributeDomain::SourceVertex};
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
    Format                       specularFormat{Format::R32G32B32A32_SFLOAT};
    // Compressed authored cube faces share immutable storage across runtime instances.
    // Only the array matching specularFormat is populated; raw callers retain their existing fields.
    SharedPtr<const HeapVector<HeapVector<uint8_t>>, MultiThreadCounter> specularBlockMipFaces;
};

} // namespace zen::asset
