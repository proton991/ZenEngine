#include <future>
#include <stb_image.h>
#include "AssetLib/FastGLTFLoader.h"
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
    static constexpr fastgltf::Extensions supportedExtensions{fastgltf::Extensions::None};
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

static Format GetTextureFormat(uint32_t imageIndex, const fastgltf::Asset* asset)
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
                const fastgltf::Texture& texture = asset->textures[info->value().textureIndex];
                if (texture.imageIndex.has_value() && texture.imageIndex.value() == imageIndex)
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
                     GetTextureFormat(imageIndex, &m_gltfAsset),
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
            pSgMat->m_pMetallicRoughnessTexture    = sceneTextures[textureInfo->textureIndex];
        }
        else
        {
            pSgMat->m_pMetallicRoughnessTexture = defaultTextures.pMetallicRoughness;
        }

        if (mat.normalTexture.has_value())
        {
            const fastgltf::TextureInfo* textureInfo = &mat.normalTexture.value();
            pSgMat->texCoordSets.normal              = textureInfo->texCoordIndex;
            pSgMat->m_pNormalTexture                 = sceneTextures[textureInfo->textureIndex];
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
            pSgMat->m_pOcclusionTexture              = sceneTextures[textureInfo->textureIndex];
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
    materials.emplace_back(defaultMaterial);
    pScene->SetComponents(std::move(materials));
}

void FastGLTFLoader::LoadGltfMeshes(sg::Scene* pScene)
{
    size_t totalVertexCount = 0;
    size_t totalIndexCount  = 0;
    for (const fastgltf::Mesh& mesh : m_gltfAsset.meshes)
    {
        for (const fastgltf::Primitive& primitive : mesh.primitives)
        {
            totalVertexCount +=
                m_gltfAsset.accessors[primitive.findAttribute("POSITION")->accessorIndex].count;
            if (primitive.indicesAccessor.has_value())
            {
                totalIndexCount += m_gltfAsset.accessors[primitive.indicesAccessor.value()].count;
            }
        }
    }
    m_vertices.resize(totalVertexCount);
    m_indices.resize(totalIndexCount);

    for (const fastgltf::Mesh& gltfMesh : m_gltfAsset.meshes)
    {
        UniquePtr<sg::Mesh> sgMesh = MakeUnique<sg::Mesh>(std::string(gltfMesh.name));
        uint32_t subMeshIndex      = 0;
        for (const fastgltf::Primitive& primitive : gltfMesh.primitives)
        {
            uint32_t vertexStart = static_cast<uint32_t>(m_vertexPos);
            uint32_t indexStart  = static_cast<uint32_t>(m_indexPos);
            uint32_t indexCount  = 0;
            uint32_t vertexCount = 0;
            Vec3 posMin{};
            Vec3 posMax{};
            Vec4 diffuseColor = Vec4(1.0f);
            // get sub mesh material
            sg::Material* pSgMaterial = nullptr;
            uint32_t materialIndex;
            const std::vector<sg::Material*>& sgMaterials = pScene->GetComponents<sg::Material>();
            if (primitive.materialIndex.has_value())
            {
                materialIndex = primitive.materialIndex.value();
                pSgMaterial   = sgMaterials[materialIndex];
                diffuseColor  = glm::make_vec4(
                    m_gltfAsset.materials[materialIndex].pbrData.baseColorFactor.data());
            }
            else
            {
                materialIndex = sgMaterials.size() - 1;
                pSgMaterial   = sgMaterials.back();
            }

            // Vertices
            const float* pBufferPos          = nullptr;
            const float* pBufferNormals      = nullptr;
            const float* pBufferTangents     = nullptr;
            const float* pBufferTexCoordSet0 = nullptr;
            const float* pBufferTexCoordSet1 = nullptr;
            const void* pBufferColorSet0     = nullptr;
            const uint32_t* pBufferJoints    = nullptr;
            const float* pBufferWeights      = nullptr;

            fastgltf::ComponentType jointsBufferComponentType = fastgltf::ComponentType::Invalid;
            fastgltf::ComponentType colorBufferComponentType  = fastgltf::ComponentType::Invalid;
            // Get buffer data for vertex positions
            if (primitive.findAttribute("POSITION") != primitive.attributes.end())
            {
                fastgltf::Accessor& accessor =
                    m_gltfAsset.accessors[primitive.findAttribute("POSITION")->accessorIndex];
                LoadAccessor<float>(accessor, pBufferPos, &vertexCount);
                const FASTGLTF_STD_PMR_NS::vector<double>& minValues =
                    *(std::get_if<FASTGLTF_STD_PMR_NS::vector<double>>(&accessor.min));
                const FASTGLTF_STD_PMR_NS::vector<double>& maxValues =
                    *(std::get_if<FASTGLTF_STD_PMR_NS::vector<double>>(&accessor.max));
                // update min max position
                posMin = Vec3(minValues[0], minValues[1], minValues[2]);
                posMax = Vec3(maxValues[0], maxValues[1], maxValues[2]);
            }
            // Get buffer data for vertex color
            if (primitive.findAttribute("COLOR_0") != primitive.attributes.end())
            {
                fastgltf::Accessor& accessor =
                    m_gltfAsset.accessors[primitive.findAttribute("COLOR_0")->accessorIndex];
                colorBufferComponentType = accessor.componentType;
                switch (colorBufferComponentType)
                {
                    case fastgltf::ComponentType::Float:
                    {
                        const float* pBuffer;
                        LoadAccessor<float>(
                            m_gltfAsset
                                .accessors[primitive.findAttribute("COLOR_0")->accessorIndex],
                            pBuffer);
                        pBufferColorSet0 = pBuffer;
                        break;
                    }
                    case fastgltf::ComponentType::UnsignedShort:
                    {
                        const uint16_t* pBuffer;
                        LoadAccessor<uint16_t>(
                            m_gltfAsset
                                .accessors[primitive.findAttribute("COLOR_0")->accessorIndex],
                            pBuffer);
                        pBufferColorSet0 = pBuffer;
                        break;
                    }
                    case fastgltf::ComponentType::UnsignedByte:
                    {
                        const uint8_t* pBuffer;
                        LoadAccessor<uint8_t>(
                            m_gltfAsset
                                .accessors[primitive.findAttribute("COLOR_0")->accessorIndex],
                            pBuffer);
                        pBufferColorSet0 = pBuffer;
                        break;
                    }
                    default:
                    {
                        LOGE("Unexpected component type {}", (uint16_t)colorBufferComponentType);
                        break;
                    }
                }
            }
            // Get buffer data for vertex normals
            if (primitive.findAttribute("NORMAL") != primitive.attributes.end())
            {
                LoadAccessor<float>(
                    m_gltfAsset.accessors[primitive.findAttribute("NORMAL")->accessorIndex],
                    pBufferNormals);
            }
            // Get buffer data for vertex tangents
            if (primitive.findAttribute("TANGENT") != primitive.attributes.end())
            {
                LoadAccessor<float>(
                    m_gltfAsset.accessors[primitive.findAttribute("TANGENT")->accessorIndex],
                    pBufferTangents);
            }
            // Get buffer data for vertex texture coordinates
            // glTF supports multiple sets
            if (primitive.findAttribute("TEXCOORD_0") != primitive.attributes.end())
            {
                LoadAccessor<float>(
                    m_gltfAsset.accessors[primitive.findAttribute("TEXCOORD_0")->accessorIndex],
                    pBufferTexCoordSet0);
            }
            if (primitive.findAttribute("TEXCOORD_1") != primitive.attributes.end())
            {
                LoadAccessor<float>(
                    m_gltfAsset.accessors[primitive.findAttribute("TEXCOORD_1")->accessorIndex],
                    pBufferTexCoordSet1);
            }

            // Get buffer data for joints
            if (primitive.findAttribute("JOINTS_0") != primitive.attributes.end())
            {
                fastgltf::Accessor& accessor =
                    m_gltfAsset.accessors[primitive.findAttribute("JOINTS_0")->accessorIndex];
                LoadAccessor<uint32_t>(accessor, pBufferJoints);
                jointsBufferComponentType = accessor.componentType;
            }
            // Get buffer data for joint weights
            if (primitive.findAttribute("WEIGHTS_0") != primitive.attributes.end())
            {
                LoadAccessor<float>(
                    m_gltfAsset.accessors[primitive.findAttribute("WEIGHTS_0")->accessorIndex],
                    pBufferWeights);
            }
            // Append data to model's vertex buffer
            for (size_t vertexIterator = 0; vertexIterator < vertexCount; ++vertexIterator)
            {
                Vertex vertex{};
                // position
                const Vec3 position =
                    pBufferPos ? glm::make_vec3(&pBufferPos[vertexIterator * 3]) : glm::vec3(0.0f);
                vertex.pos = glm::vec4(position.x, position.y, position.z, 1.0f);
                // color
                glm::vec3 vertexColor{1.0f};
                switch (colorBufferComponentType)
                {
                    case fastgltf::ComponentType::Float:
                    {
                        vertexColor = pBufferColorSet0 ?
                            glm::make_vec3(&((
                                static_cast<const float*>(pBufferColorSet0))[vertexIterator * 3])) :
                            glm::vec3(1.0f);
                        break;
                    }
                    case fastgltf::ComponentType::UnsignedShort:
                    {
                        const uint16_t* pVec3 =
                            &((static_cast<const uint16_t*>(pBufferColorSet0))[vertexIterator * 3]);
                        float norm  = 0xFFFF;
                        vertexColor = pBufferColorSet0 ?
                            glm::vec3(pVec3[0] / norm, pVec3[1] / norm, pVec3[2] / norm) :
                            glm::vec3(1.0f);
                        break;
                    }
                    case fastgltf::ComponentType::UnsignedByte:
                    {
                        const uint8_t* pVec3 =
                            &((static_cast<const uint8_t*>(pBufferColorSet0))[vertexIterator * 3]);
                        float norm  = 0xFF;
                        vertexColor = pBufferColorSet0 ?
                            glm::vec3(pVec3[0] / norm, pVec3[1] / norm, pVec3[2] / norm) :
                            glm::vec3(1.0f);
                        break;
                    }
                    default:
                    {
                        break;
                    }
                }
                vertex.color =
                    glm::vec4(vertexColor.x, vertexColor.y, vertexColor.z, 1.0f) * diffuseColor;
                // normal
                vertex.normal = glm::normalize(glm::vec4(
                    pBufferNormals ?
                        glm::vec4(glm::make_vec3(&pBufferNormals[vertexIterator * 3]), 0.0f) :
                        glm::vec4(0.0f)));
                // uv0
                const Vec2 uv0 = pBufferTexCoordSet0 ?
                    glm::make_vec2(&pBufferTexCoordSet0[vertexIterator * 2]) :
                    glm::vec2(0.0f);
                vertex.uv0     = uv0;
                // uv1
                const Vec2 uv1 = pBufferTexCoordSet1 ?
                    glm::make_vec2(&pBufferTexCoordSet1[vertexIterator * 2]) :
                    glm::vec2(0.0f);
                vertex.uv1     = uv1;
                // tangent
                glm::vec4 tangent = pBufferTangents ?
                    glm::make_vec4(&pBufferTangents[vertexIterator * 4]) :
                    glm::vec4(0.0f);
                vertex.tangent =
                    Vec4(glm::vec3(tangent.x, tangent.y, tangent.z) * tangent.w, tangent.w);
                // joint indices and joint weights
                if (pBufferJoints && pBufferWeights)
                {
                    switch (jointsBufferComponentType)
                    {
                        case fastgltf::ComponentType::Byte:
                        case fastgltf::ComponentType::UnsignedByte:
                            vertex.joint0 =
                                glm::ivec4(glm::make_vec4(&(reinterpret_cast<const int8_t*>(
                                    pBufferJoints)[vertexIterator * 4])));
                            break;
                        case fastgltf::ComponentType::Short:
                        case fastgltf::ComponentType::UnsignedShort:
                            vertex.joint0 =
                                glm::ivec4(glm::make_vec4(&(reinterpret_cast<const int16_t*>(
                                    pBufferJoints)[vertexIterator * 4])));
                            break;
                        case fastgltf::ComponentType::Int:
                        case fastgltf::ComponentType::UnsignedInt:
                            vertex.joint0 =
                                glm::ivec4(glm::make_vec4(&(reinterpret_cast<const int32_t*>(
                                    pBufferJoints)[vertexIterator * 4])));
                            break;
                        default: LOGE("data type of joints buffer not found"); break;
                    }
                    vertex.weight0 = glm::make_vec4(&pBufferWeights[vertexIterator * 4]);
                }
                m_vertices[m_vertexPos] = vertex;
                m_vertexPos++;
            }
            // Indices
            if (primitive.indicesAccessor.has_value())
            {
                const fastgltf::Accessor& accessor =
                    m_gltfAsset.accessors[primitive.indicesAccessor.value()];
                indexCount = accessor.count;
                switch (accessor.componentType)
                {
                    case fastgltf::ComponentType::UnsignedInt:
                    {
                        const uint32_t* pBufferIndices = nullptr;
                        LoadAccessor<uint32_t>(accessor, pBufferIndices);
                        for (size_t index = 0; index < accessor.count; index++)
                        {
                            m_indices[m_indexPos] = pBufferIndices[index] + vertexStart;
                            m_indexPos++;
                        }
                        break;
                    }
                    case fastgltf::ComponentType::UnsignedShort:
                    {
                        const uint16_t* pBufferIndices = nullptr;
                        LoadAccessor<uint16_t>(accessor, pBufferIndices);
                        for (size_t index = 0; index < accessor.count; index++)
                        {
                            m_indices[m_indexPos] = pBufferIndices[index] + vertexStart;
                            m_indexPos++;
                        }
                        break;
                    }
                    case fastgltf::ComponentType::UnsignedByte:
                    {
                        const uint8_t* pBufferIndices = nullptr;
                        LoadAccessor<uint8_t>(accessor, pBufferIndices);
                        for (size_t index = 0; index < accessor.count; index++)
                        {
                            m_indices[m_indexPos] = pBufferIndices[index] + vertexStart;
                            m_indexPos++;
                        }
                        break;
                    }
                    default: LOGE("Unsupported gltf index component type!"); break;
                }
            }

            const std::string subMeshName =
                fmt::format("Mesh_{}_SubMesh#{}", std::string(gltfMesh.name), subMeshIndex);
            // create sub mesh
            UniquePtr<sg::SubMesh> subMesh =
                MakeUnique<sg::SubMesh>(subMeshName, indexStart, indexCount, vertexCount);
            subMesh->SetMaterial(materialIndex, pSgMaterial);
            subMesh->SetAABB(posMin, posMax);

            sgMesh->AddSubMesh(subMesh.Get());
            sgMesh->SetAABB(posMin, posMax);

            pScene->AddComponent(std::move(subMesh));
            subMeshIndex++;
        }
        pScene->AddComponent(std::move(sgMesh));
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
