#pragma once
#include "AssetLib/ImageAsset.h"
#include "AssetLib/MaterialAsset.h"
#include "AssetLib/SceneData.h"
#include "Math/AABB.h"

namespace zen::asset
{
// Canonical coordinates are right-handed, Y-up, meters and seconds. Asset IDs
// index their owning SceneAsset arrays; no parser or runtime object is retained.
struct NodeAsset
{
    std::string        name;
    NodeId             parent;
    HeapVector<NodeId> children;
    Vec3               translation{0.0f};
    Quat               rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3               scale{1.0f};
    // Local transform is T * R * S * baseMatrix, retaining affine/sheared nodes.
    Mat4   baseMatrix{1.0f};
    bool   matrixTransform{false};
    MeshId mesh;
    // Optional per-instance bindings, indexed by the mesh primitive's material slot.
    HeapVector<MaterialId> materials;
    SkinId                 skin;
    CameraId               camera;
    LightId                light;
    HeapVector<float>      morphWeights;
    NodeId                 morphWeightsSource;
    bool                   visible{true};
    bool                   selectable{true};
    bool                   hoverable{true};
    // An instancing parent can carry mesh/weight state without drawing itself.
    bool renderMesh{true};
};

struct MeshPrimitiveAsset
{
    std::string  name;
    uint32_t     firstVertex{0};
    uint32_t     vertexCount{0};
    uint32_t     firstIndex{0};
    uint32_t     indexCount{0};
    MaterialId   material;
    uint32_t     materialSlot{UINT32_MAX};
    MeshTopology topology{MeshTopology::Triangles};
    AABB         bounds;
    bool         flatNormals{false};
    // Final vertex -> authored vertex, used by morph/custom attributes.
    HeapVector<uint32_t>                         sourceVertices;
    HeapVector<HeapVector<VertexJointInfluence>> influences;
};

struct MeshAsset
{
    std::string                    name;
    HeapVector<MeshPrimitiveAsset> primitives;
    HeapVector<float>              morphWeights;
};

struct CameraAsset : CameraParameters
{
    std::string name;
};

enum class LightKind
{
    Directional,
    Point,
    Spot
};

struct LightAsset
{
    std::string name;
    LightKind   kind{LightKind::Point};
    Vec3        color{1.0f};
    float       intensity{1.0f};
    float       range{0.0f};
    float       innerConeAngle{0.0f};
    float       outerConeAngle{glm::quarter_pi<float>()};
};

// A file revision consumed while importing, identified by its size and last-write
// time when it was read. exists == false records a missing preferred file whose
// later appearance changes resource resolution. Cache publication fills
// contentHash (XXH64 of the bytes) only if that revision is still on disk; cache
// lookups then compare content, so touched-but-unchanged files still hit.
struct SceneDependency
{
    std::string path;
    bool        exists{true};
    uint64_t    size{0};
    int64_t     modified{0};
    uint64_t    contentHash{0};
};

struct SceneAsset
{
    std::string name;
    std::string sourcePath;
    // External files actually consumed while importing (embedded images need no path).
    HeapVector<SceneDependency> dependencies;
    // Optional source metadata for inspection/round-tripping, never geometry ownership.
    std::string                               sourceDocument;
    HeapVector<NodeAsset>                     nodes;
    HeapVector<NodeId>                        roots;
    HeapVector<MeshAsset>                     meshes;
    HeapVector<Vertex>                        vertices;
    HeapVector<uint32_t>                      indices;
    HeapVector<VertexTextureCoordinates>      vertexTexCoords;
    HeapVector<uint64_t>                      vertexTexCoordSets;
    HeapVector<MaterialAsset>                 materials;
    HeapVector<ImageAsset>                    images;
    HeapVector<TextureAsset>                  textures;
    HeapVector<SamplerAsset>                  samplers;
    HeapVector<SkinAsset>                     skins;
    HeapVector<AnimationAsset>                animations;
    HeapVector<MorphPrimitiveAsset>           morphPrimitives;
    HeapVector<CameraAsset>                   cameras;
    HeapVector<LightAsset>                    lights;
    HeapVector<std::string>                   materialVariants;
    HeapVector<MaterialVariantPrimitiveAsset> variantPrimitives;
    HeapVector<VertexAttributeAsset>          extraVertexAttributes;
    HeapVector<ImageBasedLightAsset>          imageBasedLights;
    int32_t                                   imageBasedLight{-1};
};

// Checks canonical references, payloads and bounded runtime geometry expansion.
// A failure names the first invalid area in *failure when it is supplied.
bool ValidateSceneAsset(const SceneAsset& asset,
                        uint64_t          maxRuntimeGeometryBytes = 16ull * 1024 * 1024 * 1024,
                        std::string*      failure                 = nullptr);
// Checks one clip against the asset it animates. Importers drop invalid clips with a
// diagnostic instead of rejecting the whole scene.
bool ValidateAnimationAsset(const SceneAsset& asset, const AnimationAsset& animation, std::string& failure);
} // namespace zen::asset
