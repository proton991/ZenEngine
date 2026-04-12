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
    if (!filter.has_value())
    {
        return result;
    }
    switch (filter.value())
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
    static constexpr auto supportedExtensions{fastgltf::Extensions::None};
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
    auto gltfFile = fastgltf::MappedGltfFile::FromPath(path);
    if (!bool(gltfFile))
    {
        LOGE("Failed to open glTF file: {}", fastgltf::getErrorMessage(gltfFile.error()));
        return;
    }
    auto loadedAsset = m_gltfParser.loadGltf(
        gltfFile.get(), std::filesystem::path(path).parent_path(), m_loadOptions);
    if (loadedAsset.error() != fastgltf::Error::None)
    {
        LOGE("Failed to load glTF: {}", fastgltf::getErrorMessage(loadedAsset.error()));
    }
    m_gltfAsset = std::move(loadedAsset.get());
    LoadGltfSamplers(pScene);
    LoadGltfTextures(pScene);
    LoadGltfMaterials(pScene);
    LoadGltfMeshes(pScene);
    LoadGltfRenderableNodes(pScene);
    pScene->UpdateAABB();
}

void FastGLTFLoader::LoadGltfSamplers(sg::Scene* pScene)
{ // Load Samplers
    std::vector<UniquePtr<sg::Sampler>> samplers;
    samplers.reserve(m_gltfAsset.samplers.size());
    for (const fastgltf::Sampler& s : m_gltfAsset.samplers)
    {
        auto* pSampler = new sg::Sampler(std::string(s.name));
        // set props
        pSampler->minFilter = FromFastGltfFilter(s.minFilter);
        pSampler->magFilter = FromFastGltfFilter(s.magFilter);
        pSampler->wrapS     = FromFastGltfWrap(s.wrapS);
        pSampler->wrapT     = FromFastGltfWrap(s.wrapT);
        samplers.emplace_back(pSampler);
    }
    pScene->SetComponents(std::move(samplers));
}

static Format GetTextureFormat(uint32_t imageIndex, fastgltf::Asset* pGltfAsset)
{
    for (fastgltf::Material& material : pGltfAsset->materials)
    {
        if (material.pbrData.baseColorTexture
                .has_value()) // albedo aka diffuse map aka bas color -> sRGB
        {
            uint32_t diffuseTextureIndex = material.pbrData.baseColorTexture.value().textureIndex;
            auto& diffuseTexture         = pGltfAsset->textures[diffuseTextureIndex];
            if (imageIndex == diffuseTexture.imageIndex.value())
            {
                return Format::R8G8B8A8_SRGB;
            }
        }
        if (material.emissiveTexture.has_value())
        {
            uint32_t emissiveTextureIndex = material.emissiveTexture.value().textureIndex;
            auto& emissiveTexture         = pGltfAsset->textures[emissiveTextureIndex];
            if (imageIndex == emissiveTexture.imageIndex.value())
            {
                return Format::R8G8B8A8_SRGB;
            }
        }
    }

    return Format::R8G8B8A8_UNORM;
}

sg::Texture* FastGLTFLoader::LoadGltfTextureVisitor(uint32_t imageIndex)
{
    fastgltf::Texture& gltfTexture = m_gltfAsset.textures[imageIndex];
    VERIFY_EXPR(gltfTexture.imageIndex.has_value());
    fastgltf::Image& gltfImage    = m_gltfAsset.images[gltfTexture.imageIndex.value()];
    const std::string textureName = m_name + "Texture_" + std::to_string(imageIndex);

    sg::Texture* pSgTexture = new sg::Texture(textureName);
    const int samplerIndex = gltfTexture.samplerIndex.has_value() ?
        static_cast<int>(gltfTexture.samplerIndex.value()) :
        -1;
    // image data is of type std::variant:
    // the data type can be a URI/filepath, an Array, or a BufferView
    // std::visit calls the appropriate function
    std::visit(
        fastgltf::visitor{
            [&](fastgltf::sources::URI& filePath) // load from file name
            {
                const std::string imageFilepath(filePath.uri.path().begin(),
                                                filePath.uri.path().end());

                int width = 0, height = 0, nrChannels = 0;
                unsigned char* pBuffer = stbi_load(imageFilepath.c_str(), &width, &height,
                                                  &nrChannels, 4 /*int desired_channels*/);
                size_t bufferSize     = width * height * 4;

                VERIFY_EXPR_MSG(nrChannels == 4, "wrong number of channels");

                TextureInfo textureInfo{
                    (uint32_t)width, (uint32_t)height, GetTextureFormat(imageIndex, &m_gltfAsset),
                    std::vector<uint8_t>(pBuffer, pBuffer + bufferSize), samplerIndex};

                pSgTexture->Init(imageIndex, textureInfo);

                stbi_image_free(pBuffer);
            },
            [&](fastgltf::sources::Array& vector) // load from memory
            {
                int width = 0, height = 0, nrChannels = 0;

                using byte = unsigned char;
                byte* pBuffer =
                    stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(vector.bytes.data()),
                                          static_cast<int>(vector.bytes.size()), &width, &height,
                                          &nrChannels, 4 /*int desired_channels*/);
                size_t bufferSize = width * height * 4;

                TextureInfo textureInfo{
                    (uint32_t)width, (uint32_t)height, GetTextureFormat(imageIndex, &m_gltfAsset),
                    std::vector<uint8_t>(pBuffer, pBuffer + bufferSize), samplerIndex};

                pSgTexture->Init(imageIndex, textureInfo);

                stbi_image_free(pBuffer);
            },
            [&](fastgltf::sources::BufferView& view) // load from buffer view
            {
                auto& bufferView           = m_gltfAsset.bufferViews[view.bufferViewIndex];
                auto& bufferFromBufferView = m_gltfAsset.buffers[bufferView.bufferIndex];

                std::visit(fastgltf::visitor{
                               [&](auto& arg) // default branch if image data is not supported
                               {
                                   LOGE("not supported default branch (image data = BUfferView) {}",
                                        textureName);
                               },
                               [&](fastgltf::sources::Array& vector) // load from memory
                               {
                                   int width = 0, height = 0, nrChannels = 0;
                                   using byte      = unsigned char;
                                   stbi_uc* pBuffer = stbi_load_from_memory(
                                       reinterpret_cast<const stbi_uc*>(vector.bytes.data() +
                                                                        bufferView.byteOffset),
                                       static_cast<int>(bufferView.byteLength), &width, &height,
                                       &nrChannels, 4);
                                   size_t bufferSize = width * height * 4;

                                   TextureInfo textureInfo{
                                       (uint32_t)width, (uint32_t)height,
                                       GetTextureFormat(imageIndex, &m_gltfAsset),
                                       std::vector<uint8_t>(pBuffer, pBuffer + bufferSize),
                                       samplerIndex};
                                   pSgTexture->Init(imageIndex, textureInfo);

                                   stbi_image_free(pBuffer);
                               }},
                           bufferFromBufferView.data);
            },
            [&](auto& arg) {
                // default branch if image data is not supported
                LOG_FATAL_ERROR("not supported default branch {}", textureName);
            },
        },
        gltfImage.data);
    return pSgTexture;
}

template <typename ReturnType, typename... Args> class Task
{
public:
    // Submit a task (lambda or callable) with parameters
    template <typename Callable, typename... Params>
    void SubmitTask(Callable&& task, Params&&... params)
    {
        // Using std::bind to allow passing arguments to the callable
        futures.push_back(std::async(std::launch::async, std::forward<Callable>(task),
                                     std::forward<Params>(params)...));
    }

    // Wait for all tasks to finish
    void Execute()
    {
        for (auto& f : futures)
        {
            f.get(); // Block until the task finishes
        }
    }

private:
    std::vector<std::future<ReturnType>> futures;
};

void FastGLTFLoader::LoadGltfTextures(sg::Scene* pScene)
{
    uint32_t groupSize = rc::RenderConfig::GetInstance().numThreads;
    auto threadPool    = MakeUnique<ThreadPool<void, uint32_t>>(groupSize);

    std::vector<UniquePtr<sg::Texture>> textures;
    size_t numTextures = m_gltfAsset.textures.size();
    textures.resize(numTextures);

    uint32_t groupWorkLoad = numTextures / groupSize;
    uint32_t workRemained  = numTextures % rc::RenderConfig::GetInstance().numThreads;

    uint32_t startIdx = 0;
    std::vector<std::future<std::vector<sg::Texture*>>> futures;
    for (size_t i = 0; i < groupSize; ++i)
    {
        uint32_t endIdx = startIdx + groupWorkLoad;
        if (workRemained > 0)
        {
            workRemained--;
            endIdx++;
        }
        auto future = threadPool->Push([this, startIdx, endIdx](uint32_t threadId) {
            std::vector<sg::Texture*> batchResult;
            for (uint32_t j = startIdx; j < endIdx; j++)
            {
                sg::Texture* pTexture = LoadGltfTextureVisitor(j);
                batchResult.push_back(pTexture);
            }
            return batchResult;
        });
        futures.emplace_back(std::move(future));
        startIdx = endIdx;
    }
    for (auto& fut : futures)
    {
        std::vector<sg::Texture*> batch = fut.get();
        for (auto* pSgTex : batch)
        {
            textures[pSgTex->index] = UniquePtr(pSgTex);
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
    uint32_t currIndex = 0;
    for (const fastgltf::Material& mat : m_gltfAsset.materials)
    {
        auto* pSgMat            = new sg::Material(std::string(mat.name));
        pSgMat->index           = static_cast<uint32_t>(currIndex);
        pSgMat->doubleSided     = mat.doubleSided;
        pSgMat->alphaCutoff     = mat.alphaCutoff;
        pSgMat->baseColorFactor = glm::make_vec4(mat.pbrData.baseColorFactor.data());
        pSgMat->roughnessFactor = mat.pbrData.roughnessFactor;
        pSgMat->metallicFactor  = mat.pbrData.metallicFactor;
        if (mat.pbrData.baseColorTexture.has_value())
        {
            const auto& textureInfo       = mat.pbrData.baseColorTexture;
            pSgMat->texCoordSets.baseColor = textureInfo->texCoordIndex;
            pSgMat->m_pBaseColorTexture =
                pScene->GetComponents<sg::Texture>()[textureInfo->textureIndex];
        }
        else
        {
            pSgMat->m_pBaseColorTexture = defaultTextures.pBaseColor;
        }

        if (mat.pbrData.metallicRoughnessTexture.has_value())
        {
            const auto& textureInfo               = mat.pbrData.metallicRoughnessTexture;
            pSgMat->texCoordSets.metallicRoughness = textureInfo->texCoordIndex;
            pSgMat->m_pMetallicRoughnessTexture =
                pScene->GetComponents<sg::Texture>()[textureInfo->textureIndex];
        }
        else
        {
            pSgMat->m_pMetallicRoughnessTexture = defaultTextures.pMetallicRoughness;
        }

        if (mat.normalTexture.has_value())
        {
            const auto& textureInfo    = mat.normalTexture;
            pSgMat->texCoordSets.normal = textureInfo->texCoordIndex;
            pSgMat->m_pNormalTexture = pScene->GetComponents<sg::Texture>()[textureInfo->textureIndex];
        }
        else
        {
            pSgMat->m_pNormalTexture = defaultTextures.pNormal;
        }

        if (mat.emissiveTexture.has_value())
        {
            const auto& textureInfo      = mat.emissiveTexture;
            pSgMat->texCoordSets.emissive = textureInfo->texCoordIndex;
            pSgMat->m_pEmissiveTexture = pScene->GetComponents<sg::Texture>()[textureInfo->textureIndex];
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
            const auto& textureInfo       = mat.occlusionTexture;
            pSgMat->texCoordSets.occlusion = textureInfo->texCoordIndex;
            pSgMat->m_pOcclusionTexture =
                pScene->GetComponents<sg::Texture>()[textureInfo->textureIndex];
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
    UniquePtr<sg::Material> defaultMaterial   = sg::Material::CreateDefaultUnique();
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
        for (const auto& primitive : mesh.primitives)
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
        for (const auto& primitive : gltfMesh.primitives)
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
            const auto& sgMaterials = pScene->GetComponents<sg::Material>();
            if (primitive.materialIndex.has_value())
            {
                materialIndex = primitive.materialIndex.value();
                pSgMaterial    = sgMaterials[materialIndex];
                diffuseColor  = glm::make_vec4(
                    m_gltfAsset.materials[materialIndex].pbrData.baseColorFactor.data());
            }
            else
            {
                materialIndex = sgMaterials.size() - 1;
                pSgMaterial    = sgMaterials.back();
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
                auto minValues = *(std::get_if<FASTGLTF_STD_PMR_NS::vector<double>>(&accessor.min));
                auto maxValues = *(std::get_if<FASTGLTF_STD_PMR_NS::vector<double>>(&accessor.max));
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
                auto position =
                    pBufferPos ? glm::make_vec3(&pBufferPos[vertexIterator * 3]) : glm::vec3(0.0f);
                vertex.pos = glm::vec4(position.x, position.y, position.z, 1.0f);
                // color
                glm::vec3 vertexColor{1.0f};
                switch (colorBufferComponentType)
                {
                    case fastgltf::ComponentType::Float:
                    {
                        vertexColor = pBufferColorSet0 ?
                            glm::make_vec3(&(
                                (static_cast<const float*>(pBufferColorSet0))[vertexIterator * 3])) :
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
                auto uv0   = pBufferTexCoordSet0 ?
                      glm::make_vec2(&pBufferTexCoordSet0[vertexIterator * 2]) :
                      glm::vec3(0.0f);
                vertex.uv0 = uv0;
                // uv1
                auto uv1   = pBufferTexCoordSet1 ?
                      glm::make_vec2(&pBufferTexCoordSet1[vertexIterator * 2]) :
                      glm::vec3(0.0f);
                vertex.uv1 = uv1;
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
                const auto& accessor = m_gltfAsset.accessors[primitive.indicesAccessor.value()];
                indexCount           = accessor.count;
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

            const auto subMeshName =
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
    const auto& gltfNode = m_gltfAsset.nodes[nodeIndex];

    auto newNode   = MakeUnique<sg::Node>(nodeIndex, std::string(gltfNode.name));
    auto transform = MakeUnique<sg::Transform>(*newNode);
    newNode->SetParent(pParent);

    auto TRS = std::get_if<fastgltf::TRS>(&gltfNode.transform);
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
        newNode->AddComponent(transform.Get());
        pScene->AddComponent(transform);

        auto* pSgMesh = pScene->GetComponents<sg::Mesh>()[gltfNode.meshIndex.value()];
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