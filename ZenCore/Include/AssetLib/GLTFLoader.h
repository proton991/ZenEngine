#pragma once
#include <vector>
#include <string>
#include <tiny_gltf.h>
#include "Common/Math.h"
#include "Common/Types.h"
#include "Common/UniquePtr.h"


namespace zen::sg
{
class Scene;
class Node;
} // namespace zen::sg
namespace zen::gltf
{
struct Vertex
{
    Vec3 pos;
    Vec3 normal;
    Vec2 uv0;
    Vec2 uv1;
    Vec4 joint0;
    Vec4 weight0;
    Vec4 color;
};

using Index = uint32_t;

struct TextureInfo
{
    TextureInfo() = default;
    TextureInfo(int samplerIndex_,
                uint32_t weight_,
                uint32_t height_,
                Format format_,
                // moved
                std::vector<uint8_t> data_) :
        samplerIndex(samplerIndex_),
        width(weight_),
        height(height_),
        format(format_),
        data(std::move(data_))
    {}

    int samplerIndex{-1};
    uint32_t width{0};
    uint32_t height{0};
    Format format{Format::UNDEFINED};
    // byte data no mipmaps
    std::vector<uint8_t> data;
};

struct SamplerInfo
{
    int minFilter{-1}; // optional. -1 = no filter defined. ["NEAREST", "LINEAR",
                       // "NEAREST_MIPMAP_NEAREST", "LINEAR_MIPMAP_NEAREST",
                       // "NEAREST_MIPMAP_LINEAR", "LINEAR_MIPMAP_LINEAR"]
    int magFilter{-1}; // optional. -1 = no filter defined. ["NEAREST", "LINEAR"]
    int wrapS{TINYGLTF_TEXTURE_WRAP_REPEAT}; // ["CLAMP_TO_EDGE", "MIRRORED_REPEAT",
                                             // "REPEAT"], default "REPEAT"
    int wrapT{TINYGLTF_TEXTURE_WRAP_REPEAT}; // ["CLAMP_TO_EDGE", "MIRRORED_REPEAT",
                                             // "REPEAT"], default "REPEAT"
};

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


class ModelLoader
{
public:
    ModelLoader();

    void LoadFromFile(const std::string& path, gltf::Model* pOutModel, float scale = 1.0f);

    const auto& GetVertices() const { return m_vertices; }
    const auto& GetIndices() const { return m_indices; }

private:
    void LoadSamplers(tinygltf::Model& gltfModel);
    void LoadTextures(tinygltf::Model& gltfModel);
    void LoadMaterials(tinygltf::Model& gltfModel);
    void LoadNode(gltf::Model* pOutModel,
                  gltf::Node* parent,
                  const tinygltf::Node& node,
                  uint32_t nodeIndex,
                  const tinygltf::Model& model,
                  float globalScale);
    void GetModelProps(const tinygltf::Node& node,
                       const tinygltf::Model& model,
                       // sum
                       size_t& vertexCount,
                       size_t& indexCount,
                       size_t& nodeCount);
    void SetModelBoundingBox(gltf::Model* pModel);

    // counters
    size_t m_vertexPos{0};
    size_t m_indexPos{0};
    // vertices and indices
    std::vector<gltf::Vertex> m_vertices;
    std::vector<gltf::Index> m_indices;
    // textures
    std::vector<gltf::TextureInfo> m_textureInfos;
    std::vector<gltf::SamplerInfo> m_samplerInfos;
    // materials
    std::vector<gltf::Material> m_materials;

    struct DefaultTextures
    {
        gltf::TextureInfo baseColor;
        gltf::TextureInfo metallicRoughness;
        gltf::TextureInfo normal;
    } m_defaultTextures;
};


class GltfLoader
{
public:
    GltfLoader() = default;

    void LoadFromFile(const std::string& path, sg::Scene* scene);

    const auto& GetVertices() const { return m_vertices; }
    const auto& GetIndices() const { return m_indices; }

private:
    void LoadGltfSamplers(sg::Scene* scene);

    void LoadGltfTextures(sg::Scene* scene);

    void LoadGltfMaterials(sg::Scene* scene);

    void LoadGltfMeshes(sg::Scene* scene);

    void ParseGltfNodes(sg::Scene* scene);

    void ParseGltfNode(
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
    std::vector<gltf::Vertex> m_vertices;
    std::vector<gltf::Index> m_indices;
};
} // namespace zen::gltf