#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "AssetLib/GLTFLoader.h"
#include "Utils/Errors.h"
#include "SceneGraph/Scene.h"
#include "SceneGraph/Transform.h"

namespace zen::asset
{
void GLTFLoader::LoadFromFile(const std::string& path, sg::Scene* pScene)
{
    std::string error;
    std::string warning;

    bool binary   = false;
    size_t extPos = path.rfind('.', path.length());
    if (extPos != std::string::npos)
    {
        binary = (path.substr(extPos + 1, path.length() - extPos) == "glb");
    }
    bool modelLoaded = binary ?
        m_gltfContext.LoadBinaryFromFile(&m_gltfModel, &error, &warning, path) :
        m_gltfContext.LoadASCIIFromFile(&m_gltfModel, &error, &warning, path);
    if (!modelLoaded)
    {
        LOG_FATAL_ERROR_AND_THROW("Failed to load GLTF model {}, warning: {}, error: {}!", path,
                                  warning, error);
    }
    LoadGltfSamplers(pScene);
    LoadGltfTextures(pScene);
    LoadGltfMaterials(pScene);
    LoadGltfMeshes(pScene);
    LoadGltfRenderableNodes(pScene);
    pScene->UpdateAABB();
}

static sg::TextureFilter FromTinyGltfFilter(int filter)
{
    switch (filter)
    {
        case TINYGLTF_TEXTURE_FILTER_NEAREST:
        case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST:
        case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST: return sg::TextureFilter::Nearest;
        case TINYGLTF_TEXTURE_FILTER_LINEAR:
        case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR:
        case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR: return sg::TextureFilter::Linear;
        default: break;
    }
    return sg::TextureFilter::Linear;
}

static sg::SamplerAddressMode FromTintGltfWrap(int wrap)
{
    switch (wrap)
    {
        case TINYGLTF_TEXTURE_WRAP_REPEAT: return sg::SamplerAddressMode::Repeat;
        case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE: return sg::SamplerAddressMode::ClampToEdge;
        case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT: return sg::SamplerAddressMode::MirroredRepeat;
        default: break;
    }
    return sg::SamplerAddressMode::Repeat;
}

void GLTFLoader::LoadGltfSamplers(sg::Scene* pScene)
{
    // Load Samplers
    std::vector<UniquePtr<sg::Sampler>> samplers;
    samplers.reserve(m_gltfModel.samplers.size());
    for (const tinygltf::Sampler& s : m_gltfModel.samplers)
    {
        auto* pSampler = new sg::Sampler(s.name);
        // set props
        pSampler->minFilter = FromTinyGltfFilter(s.minFilter);
        pSampler->magFilter = FromTinyGltfFilter(s.magFilter);
        pSampler->wrapS     = FromTintGltfWrap(s.wrapS);
        pSampler->wrapT     = FromTintGltfWrap(s.wrapT);
        samplers.emplace_back(pSampler);
    }
    pScene->SetComponents(std::move(samplers));
}

void GLTFLoader::LoadGltfTextures(sg::Scene* pScene)
{
    std::vector<UniquePtr<sg::Texture>> textures;
    textures.reserve(m_gltfModel.textures.size() + 3);
    uint32_t textureIndex = textures.size();
    // load textures
    for (const tinygltf::Texture& tex : m_gltfModel.textures)
    {
        const tinygltf::Image& gltfImage = m_gltfModel.images[tex.source];

        auto* pSgTex = new sg::Texture(gltfImage.uri);

        // temp buffer for format conversion
        unsigned char* pBuffer = nullptr;
        // delete temp buffer?
        bool deleteBuffer = false;
        size_t bufferSize;

        // set props
        pSgTex->index        = textureIndex;
        pSgTex->format       = Format::R8G8B8A8_UNORM;
        pSgTex->width        = gltfImage.width;
        pSgTex->height       = gltfImage.height;
        pSgTex->samplerIndex = tex.sampler;

        if (gltfImage.component == 3)
        {
            // Most devices don't support RGB only on Vulkan so convert if necessary
            // TODO: Check actual format support and transform only if required
            bufferSize = gltfImage.width * gltfImage.height * 4;
            pBuffer     = new unsigned char[bufferSize];

            unsigned char* pRgba = pBuffer;
            unsigned char* pRgb  = const_cast<unsigned char*>(gltfImage.image.data());
            for (int32_t i = 0; i < gltfImage.width * gltfImage.height; ++i)
            {
                for (int32_t j = 0; j < 3; ++j)
                {
                    pRgba[j] = pRgb[j];
                }
                pRgba += 4;
                pRgb += 3;
            }
            deleteBuffer     = true;
            pSgTex->bytesData = std::move(std::vector<uint8_t>(pBuffer, pBuffer + bufferSize));
        }
        else if (gltfImage.component == 4)
        {
            pSgTex->bytesData = gltfImage.image;
        }
        // free temp buffer
        if (deleteBuffer)
        {
            delete[] pBuffer;
        }

        textures.emplace_back(pSgTex);
        textureIndex++;
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

void GLTFLoader::LoadGltfMaterials(sg::Scene* pScene)
{
    sg::Scene::DefaultTextures defaultTextures = sg::Scene::GetDefaultTextures();
    std::vector<UniquePtr<sg::Material>> materials;
    materials.reserve(m_gltfModel.materials.size());
    uint32_t currIndex = 0;
    for (const tinygltf::Material& mat : m_gltfModel.materials)
    {
        auto* pSgMat            = new sg::Material(mat.name);
        pSgMat->doubleSided     = mat.doubleSided;
        pSgMat->alphaCutoff     = mat.alphaCutoff;
        pSgMat->baseColorFactor = glm::make_vec4(mat.pbrMetallicRoughness.baseColorFactor.data());
        pSgMat->roughnessFactor = mat.pbrMetallicRoughness.roughnessFactor;
        pSgMat->metallicFactor  = mat.pbrMetallicRoughness.metallicFactor;
        if (mat.pbrMetallicRoughness.baseColorTexture.index != -1)
        {
            const auto& textureInfo       = mat.pbrMetallicRoughness.baseColorTexture;
            pSgMat->texCoordSets.baseColor = textureInfo.texCoord;
            pSgMat->m_pBaseColorTexture       = pScene->GetComponents<sg::Texture>()[textureInfo.index];
        }
        else
        {
            pSgMat->m_pBaseColorTexture = defaultTextures.pBaseColor;
        }

        if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index != -1)
        {
            const auto& textureInfo = mat.pbrMetallicRoughness.metallicRoughnessTexture;
            pSgMat->texCoordSets.metallicRoughness = textureInfo.texCoord;
            pSgMat->m_pMetallicRoughnessTexture =
                pScene->GetComponents<sg::Texture>()[textureInfo.index];
        }
        else
        {
            pSgMat->m_pMetallicRoughnessTexture = defaultTextures.pMetallicRoughness;
        }

        if (mat.normalTexture.index != -1)
        {
            const auto& textureInfo    = mat.normalTexture;
            pSgMat->texCoordSets.normal = textureInfo.texCoord;
            pSgMat->m_pNormalTexture       = pScene->GetComponents<sg::Texture>()[textureInfo.index];
        }
        else
        {
            pSgMat->m_pNormalTexture = defaultTextures.pNormal;
        }

        if (mat.emissiveTexture.index != -1)
        {
            const auto& textureInfo      = mat.emissiveTexture;
            pSgMat->texCoordSets.emissive = textureInfo.texCoord;
            pSgMat->m_pEmissiveTexture       = pScene->GetComponents<sg::Texture>()[textureInfo.index];
        }
        else
        {
            pSgMat->m_pEmissiveTexture = defaultTextures.pEmissive;
        }

        if (mat.occlusionTexture.index != -1)
        {
            const auto& textureInfo       = mat.occlusionTexture;
            pSgMat->texCoordSets.occlusion = textureInfo.texCoord;
            pSgMat->m_pOcclusionTexture       = pScene->GetComponents<sg::Texture>()[textureInfo.index];
        }
        else
        {
            pSgMat->m_pOcclusionTexture = defaultTextures.pOcclusion;
        }

        if (mat.alphaMode == "BLEND")
        {
            pSgMat->alphaMode = sg::AlphaMode::Blend;
        }
        else if (mat.alphaMode == "MASK")
        {
            pSgMat->alphaMode = sg::AlphaMode::Mask;
        }

        // Extensions
        // @TODO: Find out if there is a nicer way of reading these properties with recent tinygltf headers
        if (mat.extensions.find("KHR_materials_pbrSpecularGlossiness") != mat.extensions.end())
        {
            auto ext = mat.extensions.find("KHR_materials_pbrSpecularGlossiness");
            if (ext->second.Has("specularGlossinessTexture"))
            {
                auto index = ext->second.Get("specularGlossinessTexture").Get("index");
                pSgMat->extension.pSpecularGlossinessTexture =
                    pScene->GetComponents<sg::Texture>()[index.Get<int>()];
                auto texCoordSet = ext->second.Get("specularGlossinessTexture").Get("texCoord");
                pSgMat->texCoordSets.specularGlossiness = texCoordSet.Get<int>();
                pSgMat->pbrWorkflows.specularGlossiness = true;
            }
            if (ext->second.Has("diffuseTexture"))
            {
                auto index = ext->second.Get("diffuseTexture").Get("index");
                pSgMat->extension.pDiffuseTexture =
                    pScene->GetComponents<sg::Texture>()[index.Get<int>()];
            }
            if (ext->second.Has("diffuseFactor"))
            {
                auto factor = ext->second.Get("diffuseFactor");
                for (uint32_t i = 0; i < factor.ArrayLen(); i++)
                {
                    auto val = factor.Get(i);
                    pSgMat->extension.diffuseFactor[i] =
                        val.IsNumber() ? (float)val.Get<double>() : (float)val.Get<int>();
                }
            }
            if (ext->second.Has("specularFactor"))
            {
                auto factor = ext->second.Get("specularFactor");
                for (uint32_t i = 0; i < factor.ArrayLen(); i++)
                {
                    auto val = factor.Get(i);
                    pSgMat->extension.specularFactor[i] =
                        val.IsNumber() ? (float)val.Get<double>() : (float)val.Get<int>();
                }
            }
        }

        if (mat.extensions.find("KHR_materials_unlit") != mat.extensions.end())
        {
            pSgMat->unlit = true;
        }

        if (mat.extensions.find("KHR_materials_emissive_strength") != mat.extensions.end())
        {
            auto ext = mat.extensions.find("KHR_materials_emissive_strength");
            if (ext->second.Has("emissiveStrength"))
            {
                auto value              = ext->second.Get("emissiveStrength");
                pSgMat->emissiveStrength = (float)value.Get<double>();
            }
        }

        pSgMat->index = static_cast<uint32_t>(currIndex);
        pSgMat->SetData();
        currIndex++;
        materials.emplace_back(pSgMat);
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

void GLTFLoader::LoadGltfMeshes(sg::Scene* pScene)
{
    size_t totalVertexCount = 0;
    size_t totalIndexCount  = 0;
    for (const tinygltf::Node& node : m_gltfModel.nodes)
    {
        if (node.mesh > -1)
        {
            const tinygltf::Mesh mesh = m_gltfModel.meshes[node.mesh];
            for (size_t i = 0; i < mesh.primitives.size(); i++)
            {
                auto primitive = mesh.primitives[i];
                totalVertexCount +=
                    m_gltfModel.accessors[primitive.attributes.find("POSITION")->second].count;
                if (primitive.indices > -1)
                {
                    totalIndexCount += m_gltfModel.accessors[primitive.indices].count;
                }
            }
        }
    }
    m_vertices.resize(totalVertexCount);
    m_indices.resize(totalIndexCount);

    for (const tinygltf::Mesh& gltfMesh : m_gltfModel.meshes)
    {
        UniquePtr<sg::Mesh> sgMesh = MakeUnique<sg::Mesh>(gltfMesh.name);

        uint32_t subMeshIndex = 0;
        for (const auto& primitive : gltfMesh.primitives)
        {
            uint32_t vertexStart = static_cast<uint32_t>(m_vertexPos);
            uint32_t indexStart  = static_cast<uint32_t>(m_indexPos);
            uint32_t indexCount  = 0;
            uint32_t vertexCount = 0;
            Vec3 posMin{};
            Vec3 posMax{};
            bool hasSkin    = false;
            bool hasIndices = primitive.indices > -1;
            // Vertices
            {
                const float* pBufferPos          = nullptr;
                const float* pBufferNormals      = nullptr;
                const float* pBufferTangents     = nullptr;
                const float* pBufferTexCoordSet0 = nullptr;
                const float* pBufferTexCoordSet1 = nullptr;
                const float* pBufferColorSet0    = nullptr;
                const void* pBufferJoints        = nullptr;
                const float* pBufferWeights      = nullptr;

                int posByteStride;
                int normByteStride;
                int tanByteStride;
                int uv0ByteStride;
                int uv1ByteStride;
                int color0ByteStride;
                int jointByteStride;
                int weightByteStride;

                int jointComponentType;

                // Position attribute is required
                assert(primitive.attributes.find("POSITION") != primitive.attributes.end());

                const tinygltf::Accessor& posAccessor =
                    m_gltfModel.accessors[primitive.attributes.find("POSITION")->second];
                const tinygltf::BufferView& posView =
                    m_gltfModel.bufferViews[posAccessor.bufferView];
                pBufferPos = reinterpret_cast<const float*>(
                    &(m_gltfModel.buffers[posView.buffer]
                          .data[posAccessor.byteOffset + posView.byteOffset]));
                posMin        = Vec3(posAccessor.minValues[0], posAccessor.minValues[1],
                                     posAccessor.minValues[2]);
                posMax        = Vec3(posAccessor.maxValues[0], posAccessor.maxValues[1],
                                     posAccessor.maxValues[2]);
                vertexCount   = static_cast<uint32_t>(posAccessor.count);
                posByteStride = posAccessor.ByteStride(posView) ?
                    (posAccessor.ByteStride(posView) / sizeof(float)) :
                    tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC3);

                if (primitive.attributes.find("NORMAL") != primitive.attributes.end())
                {
                    const tinygltf::Accessor& normAccessor =
                        m_gltfModel.accessors[primitive.attributes.find("NORMAL")->second];
                    const tinygltf::BufferView& normView =
                        m_gltfModel.bufferViews[normAccessor.bufferView];
                    pBufferNormals = reinterpret_cast<const float*>(
                        &(m_gltfModel.buffers[normView.buffer]
                              .data[normAccessor.byteOffset + normView.byteOffset]));
                    normByteStride = normAccessor.ByteStride(normView) ?
                        (normAccessor.ByteStride(normView) / sizeof(float)) :
                        tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC3);
                }

                if (primitive.attributes.find("TANGENT") != primitive.attributes.end())
                {
                    const tinygltf::Accessor& tangentAccessor =
                        m_gltfModel.accessors[primitive.attributes.find("TANGENT")->second];
                    const tinygltf::BufferView& tangentView =
                        m_gltfModel.bufferViews[tangentAccessor.bufferView];
                    pBufferTangents = reinterpret_cast<const float*>(
                        &(m_gltfModel.buffers[tangentView.buffer]
                              .data[tangentAccessor.byteOffset + tangentView.byteOffset]));
                    tanByteStride = tangentAccessor.ByteStride(tangentView) ?
                        (tangentAccessor.ByteStride(tangentView) / sizeof(float)) :
                        tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC4);
                }
                // UVs
                if (primitive.attributes.find("TEXCOORD_0") != primitive.attributes.end())
                {
                    const tinygltf::Accessor& uvAccessor =
                        m_gltfModel.accessors[primitive.attributes.find("TEXCOORD_0")->second];
                    const tinygltf::BufferView& uvView =
                        m_gltfModel.bufferViews[uvAccessor.bufferView];
                    pBufferTexCoordSet0 = reinterpret_cast<const float*>(
                        &(m_gltfModel.buffers[uvView.buffer]
                              .data[uvAccessor.byteOffset + uvView.byteOffset]));
                    uv0ByteStride = uvAccessor.ByteStride(uvView) ?
                        (uvAccessor.ByteStride(uvView) / sizeof(float)) :
                        tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC2);
                }
                if (primitive.attributes.find("TEXCOORD_1") != primitive.attributes.end())
                {
                    const tinygltf::Accessor& uvAccessor =
                        m_gltfModel.accessors[primitive.attributes.find("TEXCOORD_1")->second];
                    const tinygltf::BufferView& uvView =
                        m_gltfModel.bufferViews[uvAccessor.bufferView];
                    pBufferTexCoordSet1 = reinterpret_cast<const float*>(
                        &(m_gltfModel.buffers[uvView.buffer]
                              .data[uvAccessor.byteOffset + uvView.byteOffset]));
                    uv1ByteStride = uvAccessor.ByteStride(uvView) ?
                        (uvAccessor.ByteStride(uvView) / sizeof(float)) :
                        tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC2);
                }

                // Vertex colors
                if (primitive.attributes.find("COLOR_0") != primitive.attributes.end())
                {
                    const tinygltf::Accessor& accessor =
                        m_gltfModel.accessors[primitive.attributes.find("COLOR_0")->second];
                    const tinygltf::BufferView& view = m_gltfModel.bufferViews[accessor.bufferView];
                    pBufferColorSet0                  = reinterpret_cast<const float*>(
                        &(m_gltfModel.buffers[view.buffer]
                              .data[accessor.byteOffset + view.byteOffset]));
                    color0ByteStride = accessor.ByteStride(view) ?
                        (accessor.ByteStride(view) / sizeof(float)) :
                        tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC3);
                }

                // Skinning
                // Joints
                if (primitive.attributes.find("JOINTS_0") != primitive.attributes.end())
                {
                    const tinygltf::Accessor& jointAccessor =
                        m_gltfModel.accessors[primitive.attributes.find("JOINTS_0")->second];
                    const tinygltf::BufferView& jointView =
                        m_gltfModel.bufferViews[jointAccessor.bufferView];
                    pBufferJoints       = &(m_gltfModel.buffers[jointView.buffer]
                                         .data[jointAccessor.byteOffset + jointView.byteOffset]);
                    jointComponentType = jointAccessor.componentType;
                    jointByteStride    = jointAccessor.ByteStride(jointView) ?
                           (jointAccessor.ByteStride(jointView) /
                         tinygltf::GetComponentSizeInBytes(jointComponentType)) :
                           tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC4);
                }

                if (primitive.attributes.find("WEIGHTS_0") != primitive.attributes.end())
                {
                    const tinygltf::Accessor& weightAccessor =
                        m_gltfModel.accessors[primitive.attributes.find("WEIGHTS_0")->second];
                    const tinygltf::BufferView& weightView =
                        m_gltfModel.bufferViews[weightAccessor.bufferView];
                    pBufferWeights = reinterpret_cast<const float*>(
                        &(m_gltfModel.buffers[weightView.buffer]
                              .data[weightAccessor.byteOffset + weightView.byteOffset]));
                    weightByteStride = weightAccessor.ByteStride(weightView) ?
                        (weightAccessor.ByteStride(weightView) / sizeof(float)) :
                        tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC4);
                }

                hasSkin = (pBufferJoints && pBufferWeights);

                for (size_t v = 0; v < posAccessor.count; v++)
                {
                    Vertex& vert = m_vertices[m_vertexPos];
                    vert.pos     = Vec4(glm::make_vec3(&pBufferPos[v * posByteStride]), 1.0f);
                    vert.normal  = glm::normalize(
                        Vec4(pBufferNormals ?
                                  Vec4(glm::make_vec3(&pBufferNormals[v * normByteStride]), 0.0f) :
                                  Vec4(0.0f)));
                    vert.tangent = glm::normalize(
                        Vec4(pBufferTangents ? glm::make_vec4(&pBufferTangents[v * tanByteStride]) :
                                              Vec4(0.0f)));
                    vert.uv0   = pBufferTexCoordSet0 ?
                          glm::make_vec2(&pBufferTexCoordSet0[v * uv0ByteStride]) :
                          Vec3(0.0f);
                    vert.uv1   = pBufferTexCoordSet1 ?
                          glm::make_vec2(&pBufferTexCoordSet1[v * uv1ByteStride]) :
                          Vec3(0.0f);
                    vert.color = pBufferColorSet0 ?
                        glm::make_vec4(&pBufferColorSet0[v * color0ByteStride]) :
                        Vec4(1.0f);

                    if (hasSkin)
                    {
                        switch (jointComponentType)
                        {
                            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                            {
                                const uint16_t* pBuf = static_cast<const uint16_t*>(pBufferJoints);
                                vert.joint0 = Vec4(glm::make_vec4(&pBuf[v * jointByteStride]));
                                break;
                            }
                            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                            {
                                const uint8_t* pBuf = static_cast<const uint8_t*>(pBufferJoints);
                                vert.joint0 = Vec4(glm::make_vec4(&pBuf[v * jointByteStride]));
                                break;
                            }
                            default:
                                // Not supported by spec
                                std::cerr << "Joint component type " << jointComponentType
                                          << " not supported!" << std::endl;
                                break;
                        }
                    }
                    else
                    {
                        vert.joint0 = Vec4(0.0f);
                    }
                    vert.weight0 =
                        hasSkin ? glm::make_vec4(&pBufferWeights[v * weightByteStride]) : Vec4(0.0f);
                    // Fix for all zero weights
                    if (glm::length(vert.weight0) == 0.0f)
                    {
                        vert.weight0 = Vec4(1.0f, 0.0f, 0.0f, 0.0f);
                    }
                    m_vertexPos++;
                }
            }
            // Indices
            if (hasIndices)
            {
                const tinygltf::Accessor& accessor =
                    m_gltfModel.accessors[primitive.indices > -1 ? primitive.indices : 0];
                const tinygltf::BufferView& bufferView =
                    m_gltfModel.bufferViews[accessor.bufferView];

                const tinygltf::Buffer& buffer = m_gltfModel.buffers[bufferView.buffer];

                indexCount = static_cast<uint32_t>(accessor.count);

                const void* pDataPtr = &(buffer.data[accessor.byteOffset + bufferView.byteOffset]);

                switch (accessor.componentType)
                {
                    case TINYGLTF_PARAMETER_TYPE_UNSIGNED_INT:
                    {
                        const auto* pBuf = static_cast<const uint32_t*>(pDataPtr);
                        for (size_t index = 0; index < accessor.count; index++)
                        {
                            m_indices[m_indexPos] = pBuf[index] + vertexStart;
                            m_indexPos++;
                        }
                        break;
                    }
                    case TINYGLTF_PARAMETER_TYPE_UNSIGNED_SHORT:
                    {
                        const auto* pBuf = static_cast<const uint16_t*>(pDataPtr);
                        for (size_t index = 0; index < accessor.count; index++)
                        {
                            m_indices[m_indexPos] = pBuf[index] + vertexStart;
                            m_indexPos++;
                        }
                        break;
                    }
                    case TINYGLTF_PARAMETER_TYPE_UNSIGNED_BYTE:
                    {
                        const auto* pBuf = static_cast<const uint8_t*>(pDataPtr);
                        for (size_t index = 0; index < accessor.count; index++)
                        {
                            m_indices[m_indexPos] = pBuf[index] + vertexStart;
                            m_indexPos++;
                        }
                        break;
                    }
                    default:
                        std::cerr << "Index component type " << accessor.componentType
                                  << " not supported!" << std::endl;
                        return;
                }
            }
            const auto subMeshName = fmt::format("Mesh {} SubMesh#{}", gltfMesh.name, subMeshIndex);
            // create sub mesh
            UniquePtr<sg::SubMesh> subMesh =
                MakeUnique<sg::SubMesh>(subMeshName, indexStart, indexCount, vertexCount);
            // get sub mesh material
            auto* pSgMat = primitive.material > -1 ?
                pScene->GetComponents<sg::Material>()[primitive.material] :
                pScene->GetComponents<sg::Material>().back();

            subMesh->SetMaterial(primitive.material, pSgMat);
            subMesh->SetAABB(posMin, posMax);

            sgMesh->AddSubMesh(subMesh.Get());
            sgMesh->SetAABB(posMin, posMax);

            pScene->AddComponent(std::move(subMesh));
            subMeshIndex++;
        }
        pScene->AddComponent(std::move(sgMesh));
    }
}

void GLTFLoader::LoadGltfRenderableNodes(sg::Scene* pScene)
{
    std::vector<UniquePtr<sg::Node>> sgNodes;
    sgNodes.reserve(m_gltfModel.nodes.size());
    const tinygltf::Scene& gltfScene =
        m_gltfModel.scenes[m_gltfModel.defaultScene > -1 ? m_gltfModel.defaultScene : 0];
    for (int nodeIndex : gltfScene.nodes)
    {
        const tinygltf::Node& node = m_gltfModel.nodes[nodeIndex];
        LoadGltfRenderableNodes(nodeIndex, nullptr, sgNodes, pScene);
    }

    pScene->SetNodes(std::move(sgNodes));
}

void GLTFLoader::LoadGltfRenderableNodes(
    // current node index
    uint32_t nodeIndex,
    // parent node
    sg::Node* pParent,
    // store results
    std::vector<UniquePtr<sg::Node>>& sgNodes,
    // access materials
    sg::Scene* pScene)
{
    const auto& gltfNode = m_gltfModel.nodes[nodeIndex];

    auto newNode   = MakeUnique<sg::Node>(nodeIndex, gltfNode.name);
    auto transform = MakeUnique<sg::Transform>(*newNode);

    newNode->SetParent(pParent);

    if (gltfNode.translation.size() == 3)
    {
        transform->SetTranslation(glm::make_vec3(gltfNode.translation.data()));
    }

    if (gltfNode.rotation.size() == 4)
    {
        transform->SetRotation(glm::make_quat(gltfNode.rotation.data()));
    }

    if (gltfNode.scale.size() == 3)
    {
        transform->SetScale(glm::make_vec3(gltfNode.scale.data()));
    }

    if (gltfNode.matrix.size() == 16)
    {
        transform->SetLocalMatrix(glm::make_mat4x4(gltfNode.matrix.data()));
    }

    newNode->AddComponent(transform.Get());
    pScene->AddComponent(transform);

    // Node with children
    if (!gltfNode.children.empty())
    {
        for (size_t i = 0; i < gltfNode.children.size(); i++)
        {
            LoadGltfRenderableNodes(gltfNode.children[i], newNode.Get(), sgNodes, pScene);
        }
    }

    if (gltfNode.mesh > -1)
    {
        auto* pSgMesh = pScene->GetComponents<sg::Mesh>()[gltfNode.mesh];
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