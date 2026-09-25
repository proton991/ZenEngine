#include <future>
#include <cmath>
#include <stb_image.h>
#include "AssetLib/FastGLTFLoader.h"
#include <fastgltf/glm_element_traits.hpp>
#include "SceneGraph/Scene.h"
#include "Utils/Errors.h"
#include "Utils/ThreadPool.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"

namespace zen::asset
{
static sg::TextureFilter FromFastGltfFilter(fastgltf::Optional<fastgltf::Filter> filter)
{
    sg::TextureFilter result = sg::TextureFilter::Linear;
    switch (filter.value_or(fastgltf::Filter::Linear))
    {
        case fastgltf::Filter::Nearest:
        case fastgltf::Filter::NearestMipMapNearest:
        case fastgltf::Filter::LinearMipMapNearest:
        {
            result = sg::TextureFilter::Nearest;
            break;
        }
        case fastgltf::Filter::Linear:
        case fastgltf::Filter::NearestMipMapLinear:
        case fastgltf::Filter::LinearMipMapLinear:
        {
            result = sg::TextureFilter::Linear;
            break;
        }
        default: break;
    }
    return result;
}

static sg::SamplerAddressMode FromFastGltfWrap(fastgltf::Wrap wrap)
{
    sg::SamplerAddressMode result = sg::SamplerAddressMode::Repeat;
    switch (wrap)
    {
        case fastgltf::Wrap::Repeat:
        {
            result = sg::SamplerAddressMode::Repeat;
            break;
        }
        case fastgltf::Wrap::ClampToEdge:
        {
            result = sg::SamplerAddressMode::ClampToEdge;
            break;
        }
        case fastgltf::Wrap::MirroredRepeat:
        {
            result = sg::SamplerAddressMode::MirroredRepeat;
            break;
        }
        default: break;
    }
    return result;
}

FastGLTFLoader::FastGLTFLoader()
{
    static constexpr fastgltf::Extensions supportedExtensions{
        fastgltf::Extensions::KHR_materials_emissive_strength};
    m_gltfParser  = fastgltf::Parser(supportedExtensions);
    m_loadOptions = fastgltf::Options::DontRequireValidAssetMember |
        fastgltf::Options::DecomposeNodeMatrices | fastgltf::Options::AllowDouble |
        fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages |
        fastgltf::Options::GenerateMeshIndices;
}

void FastGLTFLoader::LoadFromFile(const std::string& path, sg::Scene* pScene)
{
    m_name = std::filesystem::path(path).stem().string();
    pScene->SetName(m_name);
    fastgltf::Expected<fastgltf::MappedGltfFile> gltfFile =
        fastgltf::MappedGltfFile::FromPath(path);
    if (!bool(gltfFile))
    {
        LOGE("Failed to open glTF file: {}", fastgltf::getErrorMessage(gltfFile.error()));
    }
    else
    {
        fastgltf::Expected<fastgltf::Asset> loadedAsset = m_gltfParser.loadGltf(
            gltfFile.get(), std::filesystem::path(path).parent_path(), m_loadOptions);
        if (loadedAsset.error() != fastgltf::Error::None)
        {
            LOGE("Failed to load glTF: {}", fastgltf::getErrorMessage(loadedAsset.error()));
        }
        else
        {
            m_gltfAsset = std::move(loadedAsset.get());
            LoadGltfSamplers(pScene);
            LoadGltfTextures(pScene);
            LoadGltfMaterials(pScene);
            LoadGltfMeshes(pScene);
            LoadGltfRenderableNodes(pScene);
            pScene->UpdateAABB();
        }
    }
}

void FastGLTFLoader::LoadGltfSamplers(sg::Scene* pScene)
{ // Load Samplers
    std::vector<UniquePtr<sg::Sampler>> samplers;
    samplers.reserve(m_gltfAsset.samplers.size());
    for (const fastgltf::Sampler& s : m_gltfAsset.samplers)
    {
        sg::Sampler* pSampler = new sg::Sampler(std::string(s.name));
        // set props
        pSampler->minFilter = FromFastGltfFilter(s.minFilter);
        pSampler->magFilter = FromFastGltfFilter(s.magFilter);
        pSampler->wrapS     = FromFastGltfWrap(s.wrapS);
        pSampler->wrapT     = FromFastGltfWrap(s.wrapT);
        samplers.emplace_back(pSampler);
    }
    pScene->SetComponents(std::move(samplers));
}

static Format GetTextureFormat(uint32_t textureIndex, const fastgltf::Asset* asset)
{
    Format format = Format::R8G8B8A8_UNORM;
    for (const fastgltf::Material& material : asset->materials)
    {
        const fastgltf::Optional<fastgltf::TextureInfo>* colorTextures[] = {
            &material.pbrData.baseColorTexture, &material.emissiveTexture};
        for (const fastgltf::Optional<fastgltf::TextureInfo>* info : colorTextures)
        {
            if (info->has_value())
            {
                if (info->value().textureIndex == textureIndex)
                {
                    format = Format::R8G8B8A8_SRGB;
                    break;
                }
            }
        }
        if (format == Format::R8G8B8A8_SRGB)
        {
            break;
        }
    }
    return format;
}

static bool UsesLinearTexture(uint32_t textureIndex, const fastgltf::Asset& asset)
{
    bool used = false;
    for (const fastgltf::Material& material : asset.materials)
    {
        used = (material.pbrData.metallicRoughnessTexture.has_value() &&
                material.pbrData.metallicRoughnessTexture->textureIndex == textureIndex) ||
            (material.normalTexture.has_value() &&
             material.normalTexture->textureIndex == textureIndex) ||
            (material.occlusionTexture.has_value() &&
             material.occlusionTexture->textureIndex == textureIndex);
        if (used)
        {
            break;
        }
    }
    return used;
}

sg::Texture* FastGLTFLoader::LoadGltfTextureVisitor(uint32_t textureIndex)
{
    const fastgltf::Texture& gltfTexture = m_gltfAsset.textures[textureIndex];
    VERIFY_EXPR(gltfTexture.imageIndex.has_value());
    const uint32_t imageIndex        = static_cast<uint32_t>(gltfTexture.imageIndex.value());
    const fastgltf::Image& gltfImage = m_gltfAsset.images[imageIndex];
    const std::string textureName    = m_name + "Texture_" + std::to_string(textureIndex);
    const int samplerIndex           = gltfTexture.samplerIndex.has_value() ?
        static_cast<int>(gltfTexture.samplerIndex.value()) :
        -1;

    // The decoder owns STB memory until the scene has copied the pixels.
    struct DecodedImage
    {
        stbi_uc* pixels{nullptr};
        int width{0};
        int height{0};
        int channels{0};
        ~DecodedImage()
        {
            stbi_image_free(pixels);
        }
    } image;

    const fastgltf::sources::URI* file    = std::get_if<fastgltf::sources::URI>(&gltfImage.data);
    const fastgltf::sources::Array* array = std::get_if<fastgltf::sources::Array>(&gltfImage.data);
    const fastgltf::sources::BufferView* view =
        std::get_if<fastgltf::sources::BufferView>(&gltfImage.data);
    if (file != nullptr)
    {
        const std::string path(file->uri.path().begin(), file->uri.path().end());
        image.pixels =
            stbi_load(path.c_str(), &image.width, &image.height, &image.channels, STBI_rgb_alpha);
    }
    else
    {
        const stbi_uc* data = nullptr;
        size_t size         = 0;
        if (array != nullptr)
        {
            data = reinterpret_cast<const stbi_uc*>(array->bytes.data());
            size = array->bytes.size();
        }
        else if (view != nullptr)
        {
            const fastgltf::BufferView& bufferView = m_gltfAsset.bufferViews[view->bufferViewIndex];
            const fastgltf::Buffer& buffer         = m_gltfAsset.buffers[bufferView.bufferIndex];
            const fastgltf::sources::Array* storage =
                std::get_if<fastgltf::sources::Array>(&buffer.data);
            if (storage != nullptr && bufferView.byteOffset <= storage->bytes.size() &&
                bufferView.byteLength <= storage->bytes.size() - bufferView.byteOffset)
            {
                data =
                    reinterpret_cast<const stbi_uc*>(storage->bytes.data() + bufferView.byteOffset);
                size = bufferView.byteLength;
            }
        }
        if (data != nullptr && size <= static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            image.pixels = stbi_load_from_memory(data, static_cast<int>(size), &image.width,
                                                 &image.height, &image.channels, STBI_rgb_alpha);
        }
    }
    if (image.pixels == nullptr || image.width <= 0 || image.height <= 0)
    {
        LOG_ERROR_AND_THROW("Failed to decode glTF texture '{}'", textureName);
    }
    const size_t byteCount = size_t(image.width) * size_t(image.height) * STBI_rgb_alpha;
    TextureInfo info(static_cast<uint32_t>(image.width), static_cast<uint32_t>(image.height),
                     GetTextureFormat(textureIndex, &m_gltfAsset),
                     std::vector<uint8_t>(image.pixels, image.pixels + byteCount), samplerIndex);
    UniquePtr<sg::Texture> texture = MakeUnique<sg::Texture>(textureName);
    texture->Init(textureIndex, info);
    sg::Texture* result = texture.Get();
    texture.Release();
    return result;
}

HeapVector<UniquePtr<sg::Texture>> FastGLTFLoader::LoadGltfTextureBatch(uint32_t begin,
                                                                        uint32_t end)
{
    HeapVector<UniquePtr<sg::Texture>> textures;
    textures.reserve(end - begin);
    for (uint32_t index = begin; index < end; ++index)
    {
        textures.emplace_back(LoadGltfTextureVisitor(index));
    }
    return textures;
}

void FastGLTFLoader::LoadGltfTextures(sg::Scene* pScene)
{
    const uint32_t groupSize = std::max(1u, rc::RenderConfig::GetInstance().numThreads);
    UniquePtr<ThreadPool<void, uint32_t>> threadPool =
        MakeUnique<ThreadPool<void, uint32_t>>(groupSize);

    std::vector<UniquePtr<sg::Texture>> textures;
    size_t numTextures = m_gltfAsset.textures.size();
    textures.resize(numTextures);

    uint32_t groupWorkLoad = numTextures / groupSize;
    uint32_t workRemained  = numTextures % groupSize;

    uint32_t startIdx = 0;
    HeapVector<std::future<HeapVector<UniquePtr<sg::Texture>>>> futures;
    futures.reserve(groupSize);
    for (size_t i = 0; i < groupSize; ++i)
    {
        uint32_t endIdx = startIdx + groupWorkLoad;
        if (workRemained > 0)
        {
            workRemained--;
            endIdx++;
        }
        std::future<HeapVector<UniquePtr<sg::Texture>>> future = threadPool->Push(
            [this, startIdx, endIdx](uint32_t) { return LoadGltfTextureBatch(startIdx, endIdx); });
        futures.emplace_back(std::move(future));
        startIdx = endIdx;
    }
    for (std::future<HeapVector<UniquePtr<sg::Texture>>>& fut : futures)
    {
        HeapVector<UniquePtr<sg::Texture>> batch = fut.get();
        for (UniquePtr<sg::Texture>& texture : batch)
        {
            const uint32_t index = texture->index;
            textures[index]      = std::move(texture);
        }
    }
    // Texture indices, not image indices, carry material usage. A single glTF texture
    // may also serve both roles, in which case its linear interpretation needs a new slot.
    m_linearTextureIndices.resize(numTextures);
    for (uint32_t index = 0; index < numTextures; ++index)
    {
        m_linearTextureIndices[index] = index;
        const sg::Texture& source     = *textures[index];
        if (source.format == Format::R8G8B8A8_SRGB && UsesLinearTexture(index, m_gltfAsset))
        {
            const uint32_t linearIndex = static_cast<uint32_t>(textures.size());
            textures.emplace_back(MakeUnique<sg::Texture>(
                source.GetName() + "_linear", linearIndex, source.width, source.height,
                Format::R8G8B8A8_UNORM, source.bytesData, source.samplerIndex));
            m_linearTextureIndices[index] = linearIndex;
        }
    }
    sg::Scene::LoadDefaultTextures(textures.size());
    sg::Scene::DefaultTextures defaultTextures = sg::Scene::GetDefaultTextures();
    textures.emplace_back(defaultTextures.pBaseColor);
    textures.emplace_back(defaultTextures.pMetallicRoughness);
    textures.emplace_back(defaultTextures.pNormal);
    textures.emplace_back(defaultTextures.pEmissive);
    textures.emplace_back(defaultTextures.pOcclusion);
    pScene->SetComponents(std::move(textures));
}

void FastGLTFLoader::LoadGltfMaterials(sg::Scene* pScene)
{
    sg::Scene::DefaultTextures defaultTextures = sg::Scene::GetDefaultTextures();
    std::vector<UniquePtr<sg::Material>> materials;
    materials.resize(m_gltfAsset.materials.size());
    const std::vector<sg::Texture*> sceneTextures = pScene->GetComponents<sg::Texture>();
    uint32_t currIndex                            = 0;
    for (const fastgltf::Material& mat : m_gltfAsset.materials)
    {
        sg::Material* pSgMat    = new sg::Material(std::string(mat.name));
        pSgMat->index           = static_cast<uint32_t>(currIndex);
        pSgMat->doubleSided     = mat.doubleSided;
        pSgMat->alphaCutoff     = mat.alphaCutoff;
        pSgMat->baseColorFactor = glm::make_vec4(mat.pbrData.baseColorFactor.data());
        pSgMat->roughnessFactor = mat.pbrData.roughnessFactor;
        pSgMat->metallicFactor  = mat.pbrData.metallicFactor;
        if (mat.pbrData.baseColorTexture.has_value())
        {
            const fastgltf::TextureInfo* textureInfo = &mat.pbrData.baseColorTexture.value();
            pSgMat->texCoordSets.baseColor           = textureInfo->texCoordIndex;
            pSgMat->m_pBaseColorTexture              = sceneTextures[textureInfo->textureIndex];
        }
        else
        {
            pSgMat->m_pBaseColorTexture = defaultTextures.pBaseColor;
        }

        if (mat.pbrData.metallicRoughnessTexture.has_value())
        {
            const fastgltf::TextureInfo* textureInfo =
                &mat.pbrData.metallicRoughnessTexture.value();
            pSgMat->texCoordSets.metallicRoughness = textureInfo->texCoordIndex;
            pSgMat->m_pMetallicRoughnessTexture =
                sceneTextures[m_linearTextureIndices[textureInfo->textureIndex]];
        }
        else
        {
            pSgMat->m_pMetallicRoughnessTexture = defaultTextures.pMetallicRoughness;
        }

        if (mat.normalTexture.has_value())
        {
            const fastgltf::TextureInfo* textureInfo = &mat.normalTexture.value();
            pSgMat->texCoordSets.normal              = textureInfo->texCoordIndex;
            pSgMat->m_pNormalTexture =
                sceneTextures[m_linearTextureIndices[textureInfo->textureIndex]];
            pSgMat->normalScale = mat.normalTexture->scale;
        }
        else
        {
            pSgMat->m_pNormalTexture = defaultTextures.pNormal;
        }

        if (mat.emissiveTexture.has_value())
        {
            const fastgltf::TextureInfo* textureInfo = &mat.emissiveTexture.value();
            pSgMat->texCoordSets.emissive            = textureInfo->texCoordIndex;
            pSgMat->m_pEmissiveTexture               = sceneTextures[textureInfo->textureIndex];
        }
        else
        {
            pSgMat->m_pEmissiveTexture = defaultTextures.pEmissive;
        }
        {
            pSgMat->emissiveStrength = mat.emissiveStrength;
            pSgMat->emissiveFactor   = Vec4(glm::make_vec3(mat.emissiveFactor.data()), 0.0f);
        }
        if (mat.occlusionTexture.has_value())
        {
            const fastgltf::TextureInfo* textureInfo = &mat.occlusionTexture.value();
            pSgMat->texCoordSets.occlusion           = textureInfo->texCoordIndex;
            pSgMat->m_pOcclusionTexture =
                sceneTextures[m_linearTextureIndices[textureInfo->textureIndex]];
        }
        else
        {
            pSgMat->m_pOcclusionTexture = defaultTextures.pOcclusion;
        }

        if (mat.alphaMode == fastgltf::AlphaMode::Blend)
        {
            pSgMat->alphaMode = sg::AlphaMode::Blend;
        }
        else if (mat.alphaMode == fastgltf::AlphaMode::Mask)
        {
            pSgMat->alphaMode = sg::AlphaMode::Mask;
        }
        else
        {
            pSgMat->alphaMode = sg::AlphaMode::Opaque;
        }
        // todo: support gltf extensions

        pSgMat->SetData();
        materials[currIndex] = UniquePtr<sg::Material>(pSgMat);
        currIndex++;
    }
    // Push a default material at the end of the list for meshes with no material assigned
    UniquePtr<sg::Material> defaultMaterial      = sg::Material::CreateDefaultUnique();
    defaultMaterial->m_pBaseColorTexture         = defaultTextures.pBaseColor;
    defaultMaterial->m_pMetallicRoughnessTexture = defaultTextures.pMetallicRoughness;
    defaultMaterial->m_pNormalTexture            = defaultTextures.pNormal;
    defaultMaterial->m_pOcclusionTexture         = defaultTextures.pOcclusion;
    defaultMaterial->m_pEmissiveTexture          = defaultTextures.pEmissive;
    defaultMaterial->index                       = static_cast<uint32_t>(materials.size());
    defaultMaterial->SetData();
    materials.emplace_back(defaultMaterial);
    pScene->SetComponents(std::move(materials));
}

const fastgltf::Accessor* FastGLTFLoader::GetVertexAccessor(const fastgltf::Primitive& primitive,
                                                            const char* name,
                                                            fastgltf::AccessorType type,
                                                            size_t vertexCount) const
{
    const fastgltf::Accessor* result = nullptr;
    const auto attribute             = primitive.findAttribute(name);
    if (attribute != primitive.attributes.end())
    {
        const fastgltf::Accessor& accessor = m_gltfAsset.accessors[attribute->accessorIndex];
        const std::string_view semantic(name);
        const bool integer = accessor.componentType == fastgltf::ComponentType::UnsignedByte ||
            accessor.componentType == fastgltf::ComponentType::UnsignedShort;
        bool componentValid = accessor.componentType == fastgltf::ComponentType::Float;
        if (semantic == "JOINTS_0")
        {
            componentValid = integer && !accessor.normalized;
        }
        else if (semantic == "COLOR_0" || semantic == "TEXCOORD_0" || semantic == "TEXCOORD_1" ||
                 semantic == "WEIGHTS_0")
        {
            componentValid |= integer && accessor.normalized;
        }
        const bool typeValid = accessor.type == type ||
            (semantic == "COLOR_0" && accessor.type == fastgltf::AccessorType::Vec3);
        if (componentValid && typeValid && (vertexCount == 0 || accessor.count == vertexCount))
        {
            result = &accessor;
        }
        else
        {
            LOGE("Invalid {} accessor; using the attribute default", name);
        }
    }
    return result;
}

template <typename T> static T DecodeAttribute(const fastgltf::Asset& asset,
                                               const fastgltf::Accessor* accessor,
                                               size_t index,
                                               const T& fallback)
{
    T result = fallback;
    if (accessor != nullptr)
    {
        result = fastgltf::getAccessorElement<T>(asset, *accessor, index);
    }
    return result;
}

static bool NormalizeSurfaceNormal(Vec3& normal)
{
    const float lengthSquared = glm::dot(normal, normal);
    const bool valid          = std::isfinite(lengthSquared) && lengthSquared > 1e-12f;
    if (valid)
    {
        normal /= std::sqrt(lengthSquared);
    }
    return valid;
}

static void GenerateFlatNormals(HeapVector<Vertex>& vertices, HeapVector<uint32_t>& indices)
{
    // Indexed vertices can belong to faces with different normals. Split them so both
    // the raster path and voxel attribute resolve receive the same flat-shaded surface.
    HeapVector<Vertex> flatVertices(indices.size());
    for (size_t first = 0; first < indices.size(); first += 3)
    {
        const Vec3 a(vertices[indices[first]].pos);
        const Vec3 b(vertices[indices[first + 1]].pos);
        const Vec3 c(vertices[indices[first + 2]].pos);
        Vec3 normal = glm::cross(b - a, c - a);
        if (!NormalizeSurfaceNormal(normal))
        {
            normal = Vec3(0, 1, 0);
        }
        for (size_t corner = 0; corner < 3; ++corner)
        {
            const size_t index          = first + corner;
            flatVertices[index]         = vertices[indices[index]];
            flatVertices[index].normal  = Vec4(normal, 0);
            flatVertices[index].tangent = Vec4(0);
            indices[index]              = static_cast<uint32_t>(index);
        }
    }
    vertices = std::move(flatVertices);
}

bool FastGLTFLoader::LoadPrimitive(const fastgltf::Primitive& primitive,
                                   HeapVector<Vertex>& vertices,
                                   HeapVector<uint32_t>& indices) const
{
    vertices.clear();
    indices.clear();
    const fastgltf::Accessor* positions =
        GetVertexAccessor(primitive, "POSITION", fastgltf::AccessorType::Vec3, 0);
    bool valid = positions != nullptr && primitive.type == fastgltf::PrimitiveType::Triangles;
    if (valid)
    {
        const size_t count = positions->count;
        const fastgltf::Accessor* normals =
            GetVertexAccessor(primitive, "NORMAL", fastgltf::AccessorType::Vec3, count);
        const fastgltf::Accessor* tangents =
            GetVertexAccessor(primitive, "TANGENT", fastgltf::AccessorType::Vec4, count);
        const fastgltf::Accessor* colors =
            GetVertexAccessor(primitive, "COLOR_0", fastgltf::AccessorType::Vec4, count);
        const fastgltf::Accessor* uv0 =
            GetVertexAccessor(primitive, "TEXCOORD_0", fastgltf::AccessorType::Vec2, count);
        const fastgltf::Accessor* uv1 =
            GetVertexAccessor(primitive, "TEXCOORD_1", fastgltf::AccessorType::Vec2, count);
        const fastgltf::Accessor* joints =
            GetVertexAccessor(primitive, "JOINTS_0", fastgltf::AccessorType::Vec4, count);
        const fastgltf::Accessor* weights =
            GetVertexAccessor(primitive, "WEIGHTS_0", fastgltf::AccessorType::Vec4, count);
        bool generateNormals = normals == nullptr;
        vertices.resize(count);
        for (size_t index = 0; index < count; ++index)
        {
            Vertex& vertex = vertices[index];
            vertex.pos     = Vec4(DecodeAttribute(m_gltfAsset, positions, index, Vec3(0)), 1);
            valid &= std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y) &&
                std::isfinite(vertex.pos.z);
            Vec3 normal = DecodeAttribute(m_gltfAsset, normals, index, Vec3(0));
            generateNormals |= !NormalizeSurfaceNormal(normal);
            vertex.normal  = Vec4(normal, 0);
            vertex.tangent = DecodeAttribute(m_gltfAsset, tangents, index, Vec4(0));
            vertex.uv0     = DecodeAttribute(m_gltfAsset, uv0, index, Vec2(0));
            vertex.uv1     = DecodeAttribute(m_gltfAsset, uv1, index, Vec2(0));
            vertex.joint0  = DecodeAttribute(m_gltfAsset, joints, index, Vec4(0));
            vertex.weight0 = DecodeAttribute(m_gltfAsset, weights, index, Vec4(0));
            vertex.color   = colors != nullptr && colors->type == fastgltf::AccessorType::Vec3 ?
                Vec4(DecodeAttribute(m_gltfAsset, colors, index, Vec3(1)), 1) :
                DecodeAttribute(m_gltfAsset, colors, index, Vec4(1));
        }
        if (primitive.indicesAccessor.has_value())
        {
            const fastgltf::Accessor& accessor = m_gltfAsset.accessors[*primitive.indicesAccessor];
            valid &= accessor.type == fastgltf::AccessorType::Scalar && !accessor.normalized &&
                (accessor.componentType == fastgltf::ComponentType::UnsignedByte ||
                 accessor.componentType == fastgltf::ComponentType::UnsignedShort ||
                 accessor.componentType == fastgltf::ComponentType::UnsignedInt);
            if (valid)
            {
                indices.resize(accessor.count);
                fastgltf::copyFromAccessor<uint32_t>(m_gltfAsset, accessor, indices.data());
            }
        }
        else
        {
            indices.resize(count);
            for (uint32_t index = 0; index < count; ++index)
            {
                indices[index] = index;
            }
        }
        valid &= !indices.empty() && indices.size() % 3 == 0;
        for (uint32_t index : indices)
        {
            valid &= index < count;
        }
        if (valid && generateNormals)
        {
            GenerateFlatNormals(vertices, indices);
        }
    }
    if (!valid)
    {
        LOGE("Skipping invalid or unsupported glTF triangle primitive");
    }
    return valid;
}

void FastGLTFLoader::LoadGltfMeshes(sg::Scene* pScene)
{
    m_vertices.clear();
    m_indices.clear();
    const std::vector<sg::Material*> materials = pScene->GetComponents<sg::Material>();
    HeapVector<Vertex> vertices;
    HeapVector<uint32_t> indices;
    for (const fastgltf::Mesh& gltfMesh : m_gltfAsset.meshes)
    {
        UniquePtr<sg::Mesh> mesh = MakeUnique<sg::Mesh>(std::string(gltfMesh.name));
        uint32_t subMeshIndex    = 0;
        for (const fastgltf::Primitive& primitive : gltfMesh.primitives)
        {
            if (LoadPrimitive(primitive, vertices, indices))
            {
                const uint32_t vertexStart = static_cast<uint32_t>(m_vertices.size());
                const uint32_t indexStart  = static_cast<uint32_t>(m_indices.size());
                sg::AABB bounds;
                for (const Vertex& vertex : vertices)
                {
                    bounds.SetMin(Vec3(vertex.pos));
                    bounds.SetMax(Vec3(vertex.pos));
                    m_vertices.push_back(vertex);
                }
                for (uint32_t index : indices)
                {
                    m_indices.push_back(vertexStart + index);
                }
                const uint32_t materialIndex =
                    static_cast<uint32_t>(primitive.materialIndex.value_or(materials.size() - 1));
                const std::string name =
                    fmt::format("Mesh_{}_SubMesh#{}", gltfMesh.name, subMeshIndex);
                UniquePtr<sg::SubMesh> subMesh =
                    MakeUnique<sg::SubMesh>(name, indexStart, static_cast<uint32_t>(indices.size()),
                                            static_cast<uint32_t>(vertices.size()));
                subMesh->SetMaterial(materialIndex, materials[materialIndex]);
                subMesh->SetAABB(bounds.GetMin(), bounds.GetMax());
                mesh->AddSubMesh(subMesh.Get());
                mesh->SetAABB(bounds.GetMin(), bounds.GetMax());
                pScene->AddComponent(std::move(subMesh));
            }
            ++subMeshIndex;
        }
        pScene->AddComponent(std::move(mesh));
    }
}

void FastGLTFLoader::LoadGltfRenderableNodes(sg::Scene* pScene)
{
    std::vector<UniquePtr<sg::Node>> sgNodes;
    sgNodes.reserve(m_gltfAsset.nodes.size());
    const fastgltf::Scene& gltfScene =
        m_gltfAsset
            .scenes[m_gltfAsset.defaultScene.has_value() ? m_gltfAsset.defaultScene.value() : 0];
    for (size_t nodeIndex : gltfScene.nodeIndices)
    {
        //        const fastgltf::Node& node = m_gltfAsset.nodes[nodeIndex];
        LoadGltfRenderableNodes(nodeIndex, nullptr, sgNodes, pScene);
    }

    pScene->SetNodes(std::move(sgNodes));
}

void FastGLTFLoader::LoadGltfRenderableNodes(uint32_t nodeIndex,
                                             sg::Node* pParent,
                                             std::vector<UniquePtr<sg::Node>>& sgNodes,
                                             sg::Scene* pScene)
{
    const fastgltf::Node& gltfNode = m_gltfAsset.nodes[nodeIndex];

    UniquePtr<sg::Node> newNode = MakeUnique<sg::Node>(nodeIndex, std::string(gltfNode.name));
    UniquePtr<sg::Transform> transform = MakeUnique<sg::Transform>(*newNode);
    newNode->SetParent(pParent);

    const fastgltf::TRS* TRS = std::get_if<fastgltf::TRS>(&gltfNode.transform);
    if (TRS->translation.size() == 3)
    {
        transform->SetTranslation(glm::make_vec3(TRS->translation.data()));
    }

    if (TRS->rotation.size() == 4)
    {
        transform->SetRotation(glm::make_quat(TRS->rotation.data()));
    }

    if (TRS->scale.size() == 3)
    {
        transform->SetScale(glm::make_vec3(TRS->scale.data()));
    }

    // Descendants need their ancestors' transforms even when those nodes have no mesh.
    newNode->AddComponent(transform.Get());
    pScene->AddComponent(std::move(transform));

    // Node with children
    if (!gltfNode.children.empty())
    {
        for (size_t child : gltfNode.children)
        {
            LoadGltfRenderableNodes(child, newNode.Get(), sgNodes, pScene);
        }
    }

    if (gltfNode.meshIndex.has_value())
    {
        sg::Mesh* pSgMesh = pScene->GetComponents<sg::Mesh>()[gltfNode.meshIndex.value()];
        newNode->AddComponent(pSgMesh);
        newNode->SetData(pScene->GetRenderableCount(),
                         newNode->GetComponent<sg::Transform>()->GetWorldMatrix());
        pSgMesh->AddNode(newNode.Get());
        // store
        pScene->AddRenderableNode(newNode.Get());
    }

    if (pParent)
    {
        pParent->AddChild(newNode.Get());
    }
    sgNodes.push_back(newNode);
}
} // namespace zen::asset
