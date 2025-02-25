#pragma once
#include <vector>
#include <string>
#include <tiny_gltf.h>
#include "Common/Math.h"
#include "Common/UniquePtr.h"
#include "Types.h"

namespace zen::sg
{
class Scene;
class Node;
} // namespace zen::sg
namespace zen::asset
{
struct BoundingBox
{
    BoundingBox() = default;
    bool valid{false};
    Vec3 min;
    Vec3 max;
    BoundingBox(const Vec3& min, const Vec3& max) : min(min), max(max) {}
    void Transform(const Mat4& matrix);
};

enum class AlphaMode
{
    Opaque,
    Mask,
    Blend
};

struct Material
{

    AlphaMode alphaMode{AlphaMode::Opaque};
    float alphaCutoff{1.0f};
    float metallicFactor{1.0f};
    float roughnessFactor{1.0f};
    Vec4 baseColorFactor{1.0f};
    Vec4 emissiveFactor{0.0f};
    TextureInfo* baseColorTexture{nullptr};
    TextureInfo* metallicRoughnessTexture{nullptr};
    TextureInfo* normalTexture{nullptr};
    TextureInfo* occlusionTexture{nullptr};
    TextureInfo* emissiveTexture{nullptr};
    bool doubleSided{false};
    struct TexCoordSets
    {
        uint8_t baseColor{0};
        uint8_t metallicRoughness{0};
        uint8_t specularGlossiness{0};
        uint8_t normal{0};
        uint8_t occlusion{0};
        uint8_t emissive{0};
    } texCoordSets;
    struct Extension
    {
        TextureInfo* specularGlossinessTexture;
        TextureInfo* diffuseTexture;
        Vec4 diffuseFactor{1.0f};
        Vec3 specularFactor{0.0f};
    } extension;
    struct PbrWorkflows
    {
        bool metallicRoughness{true};
        bool specularGlossiness{false};
    } pbrWorkflows;
    uint32_t index{0};
    bool unlit{false};
    float emissiveStrength{1.0f};
};

struct Primitive
{
    Primitive(uint32_t firstIndex, uint32_t indexCount, uint32_t vertexCount, Material* mat) :
        firstIndex(firstIndex), indexCount(indexCount), vertexCount(vertexCount), material(mat)
    {}

    void SetBoundingBox(const Vec3& min, const Vec3& max)
    {
        bb.min = min;
        bb.max = max;
    }
    uint32_t firstIndex{0};
    uint32_t indexCount{0};
    uint32_t vertexCount{0};
    BoundingBox bb;
    Material* material{nullptr};
    bool hasIndices{false};
};

struct Node
{
    Node() = default;
    ~Node();

    Mat4 GetMatrix() const;

    Node* parent{nullptr};
    uint32_t index{0};
    int32_t skinIndex{-1};
    std::string name;
    // node matrix
    Mat4 matrix{1.0f};
    // combined transform matrix
    Mat4 localMatrix{1.0f};
    // axis aligned bounding box
    BoundingBox bb;
    // contained primitives
    std::vector<Primitive*> primitives;
    // children
    std::vector<Node*> children;
    // transformations
    Vec3 translation{0.0f};
    Vec3 scale{1.0f};
    Quat rotation{};
};

struct Model
{
    Model() = default;
    ~Model();
    float GetSize() const;

    BoundingBox bb{Vec3(FLT_MAX), Vec3(FLT_MIN)};
    // nodes
    std::vector<Node*> nodes;
    std::vector<Node*> flatNodes;
};

class GLTFLoader
{
public:
    GLTFLoader() = default;

    void LoadFromFile(const std::string& path, sg::Scene* scene);

    const auto& GetVertices() const
    {
        return m_vertices;
    }
    const auto& GetIndices() const
    {
        return m_indices;
    }

private:
    void LoadGltfSamplers(sg::Scene* scene);

    void LoadGltfTextures(sg::Scene* scene);

    void LoadGltfMaterials(sg::Scene* scene);

    void LoadGltfMeshes(sg::Scene* scene);

    void LoadGltfRenderableNodes(sg::Scene* scene);

    void LoadGltfRenderableNodes(
        // current node index
        uint32_t nodeIndex,
        // parent node
        sg::Node* parent,
        // store results
        std::vector<UniquePtr<sg::Node>>& sgNodes,
        // access materials
        sg::Scene* scene);

    tinygltf::Model m_gltfModel;
    tinygltf::TinyGLTF m_gltfContext;
    // counters
    size_t m_vertexPos{0};
    size_t m_indexPos{0};
    // vertices and indices
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
};
} // namespace zen::asset