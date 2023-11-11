#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "AssetLib/GLTFLoader.h"
#include "Common/Errors.h"
#include "SceneGraph/Scene.h"
#include "SceneGraph/Transform.h"

namespace zen::gltf
{
void BoundingBox::Transform(const Mat4& matrix)
{
    Vec3 min = Vec3(matrix[3]);
    Vec3 max = min;
    Vec3 v0, v1;

    Vec3 right = Vec3(matrix[0]);
    Vec3 up    = Vec3(matrix[1]);
    Vec3 back  = Vec3(matrix[2]);

    v0 = right * this->min.x;
    v1 = right * this->max.x;
    min += glm::min(v0, v1);
    max += glm::max(v0, v1);

    v0 = up * this->min.y;
    v1 = up * this->max.y;
    min += glm::min(v0, v1);
    max += glm::max(v0, v1);

    v0 = back * this->min.z;
    v1 = back * this->max.z;
    min += glm::min(v0, v1);
    max += glm::max(v0, v1);

    this->min = min;
    this->max = max;
}

Node::~Node()
{
    for (auto* primitive : primitives) { delete primitive; }
    for (auto* child : children) { delete child; }
}

Model::~Model()
{
    for (auto* node : nodes) { delete node; }
}

Mat4 Node::GetMatrix() const
{
    Mat4  m = localMatrix;
    auto* p = parent;
    while (p)
    {
        m = p->localMatrix * m;
        p = p->parent;
    }
    return m;
}

float Model::GetSize() const { return glm::distance(bb.min, bb.max); }

void ModelLoader::LoadFromFile(const std::string& path, gltf::Model* pOutModel, float scale)
{
    tinygltf::Model    gltfModel;
    tinygltf::TinyGLTF gltfContext;

    std::string error;
    std::string warning;

    bool   binary = false;
    size_t extPos = path.rfind('.', path.length());
    if (extPos != std::string::npos)
    {
        binary = (path.substr(extPos + 1, path.length() - extPos) == "glb");
    }
    bool modelLoaded = binary ? gltfContext.LoadBinaryFromFile(&gltfModel, &error, &warning, path) :
                                gltfContext.LoadASCIIFromFile(&gltfModel, &error, &warning, path);
    if (!modelLoaded) { LOG_FATAL_ERROR_AND_THROW("Failed to load GLTF model {}!", path); }
    size_t vertexCount = 0;
    size_t indexCount  = 0;
    size_t nodeCount   = 0;
    LoadSamplers(gltfModel);
    LoadTextures(gltfModel);
    LoadMaterials(gltfModel);
    const tinygltf::Scene& scene =
        gltfModel.scenes[gltfModel.defaultScene > -1 ? gltfModel.defaultScene : 0];
    for (int node : scene.nodes)
    {
        GetModelProps(gltfModel.nodes[node], gltfModel, vertexCount, indexCount, nodeCount);
    }
    m_vertices.resize(vertexCount);
    m_indices.resize(indexCount);

    pOutModel->nodes.reserve(scene.nodes.size());
    pOutModel->flatNodes.reserve(nodeCount);

    for (int nodeIndex : scene.nodes)
    {
        const tinygltf::Node& node = gltfModel.nodes[nodeIndex];
        LoadNode(pOutModel, nullptr, node, nodeIndex, gltfModel, scale);
    }
    SetModelBoundingBox(pOutModel);
}

void ModelLoader::LoadSamplers(tinygltf::Model& gltfModel)
{
    for (const tinygltf::Sampler& s : gltfModel.samplers)
    {
        gltf::SamplerInfo sampler{};
        sampler.minFilter = s.minFilter;
        sampler.magFilter = s.magFilter;
        sampler.wrapS     = s.wrapS;
        sampler.wrapT     = s.wrapT;
        m_samplerInfos.push_back(sampler);
    }
}

void ModelLoader::LoadTextures(tinygltf::Model& gltfModel)
{
    for (const tinygltf::Texture& texture : gltfModel.textures)
    {
        // temp buffer for format conversion
        unsigned char* buffer = nullptr;
        // delete temp buffer?
        bool   deleteBuffer = false;
        size_t bufferSize   = 0;

        tinygltf::Image& gltfImage = gltfModel.images[texture.source];
        if (gltfImage.component == 3)
        {
            // Most devices don't support RGB only on Vulkan so convert if necessary
            // TODO: Check actual format support and transform only if required
            bufferSize          = gltfImage.width * gltfImage.height * 4;
            buffer              = new unsigned char[bufferSize];
            unsigned char* rgba = buffer;
            unsigned char* rgb  = &gltfImage.image[0];
            for (int32_t i = 0; i < gltfImage.width * gltfImage.height; ++i)
            {
                for (int32_t j = 0; j < 3; ++j) { rgba[j] = rgb[j]; }
                rgba += 4;
                rgb += 3;
            }
            deleteBuffer = true;
            m_textureInfos.emplace_back(texture.sampler, (uint32_t)gltfImage.width,
                                        (uint32_t)gltfImage.height, Format::R8G8B8A8_UNORM,
                                        std::vector<uint8_t>(buffer, buffer + bufferSize));
        }
        else if (gltfImage.component == 4)
        {
            m_textureInfos.emplace_back(texture.sampler, (uint32_t)gltfImage.width,
                                        (uint32_t)gltfImage.height, Format::R8G8B8A8_UNORM,
                                        gltfImage.image);
        }
        if (deleteBuffer) { delete[] buffer; }
    }
}

void ModelLoader::LoadMaterials(tinygltf::Model& gltfModel)
{
    m_materials.reserve(gltfModel.materials.size());
    for (tinygltf::Material& mat : gltfModel.materials)
    {
        gltf::Material material{};
        material.doubleSided     = mat.doubleSided;
        material.alphaCutoff     = mat.alphaCutoff;
        material.baseColorFactor = glm::make_vec4(mat.pbrMetallicRoughness.baseColorFactor.data());
        material.roughnessFactor = mat.pbrMetallicRoughness.roughnessFactor;
        material.metallicFactor  = mat.pbrMetallicRoughness.metallicFactor;
        if (mat.pbrMetallicRoughness.baseColorTexture.index != -1)
        {
            const auto& textureInfo         = mat.pbrMetallicRoughness.baseColorTexture;
            material.texCoordSets.baseColor = textureInfo.texCoord;
            material.baseColorTexture       = &m_textureInfos[textureInfo.index];
        }
        else { material.baseColorTexture = &m_defaultTextures.baseColor; }

        if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index != -1)
        {
            const auto& textureInfo = mat.pbrMetallicRoughness.metallicRoughnessTexture;
            material.texCoordSets.metallicRoughness = textureInfo.texCoord;
            material.metallicRoughnessTexture       = &m_textureInfos[textureInfo.index];
        }
        else { material.metallicRoughnessTexture = &m_defaultTextures.metallicRoughness; }

        if (mat.normalTexture.index != -1)
        {
            const auto& textureInfo      = mat.normalTexture;
            material.texCoordSets.normal = textureInfo.texCoord;
            material.normalTexture       = &m_textureInfos[textureInfo.index];
        }
        else { material.normalTexture = &m_defaultTextures.normal; }

        if (mat.emissiveTexture.index != -1)
        {
            const auto& textureInfo        = mat.emissiveTexture;
            material.texCoordSets.emissive = textureInfo.texCoord;
            material.emissiveTexture       = &m_textureInfos[textureInfo.index];
        }

        if (mat.occlusionTexture.index != -1)
        {
            const auto& textureInfo         = mat.occlusionTexture;
            material.texCoordSets.occlusion = textureInfo.texCoord;
            material.occlusionTexture       = &m_textureInfos[textureInfo.index];
        }

        if (mat.alphaMode == "BLEND") { material.alphaMode = gltf::AlphaMode::Blend; }
        else if (mat.alphaMode == "MASK") { material.alphaMode = gltf::AlphaMode::Mask; }

        // Extensions
        // @TODO: Find out if there is a nicer way of reading these properties with recent tinygltf headers
        if (mat.extensions.find("KHR_materials_pbrSpecularGlossiness") != mat.extensions.end())
        {
            auto ext = mat.extensions.find("KHR_materials_pbrSpecularGlossiness");
            if (ext->second.Has("specularGlossinessTexture"))
            {
                auto index = ext->second.Get("specularGlossinessTexture").Get("index");
                material.extension.specularGlossinessTexture = &m_textureInfos[index.Get<int>()];
                auto texCoordSet = ext->second.Get("specularGlossinessTexture").Get("texCoord");
                material.texCoordSets.specularGlossiness = texCoordSet.Get<int>();
                material.pbrWorkflows.specularGlossiness = true;
            }
            if (ext->second.Has("diffuseTexture"))
            {
                auto index                        = ext->second.Get("diffuseTexture").Get("index");
                material.extension.diffuseTexture = &m_textureInfos[index.Get<int>()];
            }
            if (ext->second.Has("diffuseFactor"))
            {
                auto factor = ext->second.Get("diffuseFactor");
                for (uint32_t i = 0; i < factor.ArrayLen(); i++)
                {
                    auto val = factor.Get(i);
                    material.extension.diffuseFactor[i] =
                        val.IsNumber() ? (float)val.Get<double>() : (float)val.Get<int>();
                }
            }
            if (ext->second.Has("specularFactor"))
            {
                auto factor = ext->second.Get("specularFactor");
                for (uint32_t i = 0; i < factor.ArrayLen(); i++)
                {
                    auto val = factor.Get(i);
                    material.extension.specularFactor[i] =
                        val.IsNumber() ? (float)val.Get<double>() : (float)val.Get<int>();
                }
            }
        }

        if (mat.extensions.find("KHR_materials_unlit") != mat.extensions.end())
        {
            material.unlit = true;
        }

        if (mat.extensions.find("KHR_materials_emissive_strength") != mat.extensions.end())
        {
            auto ext = mat.extensions.find("KHR_materials_emissive_strength");
            if (ext->second.Has("emissiveStrength"))
            {
                auto value                = ext->second.Get("emissiveStrength");
                material.emissiveStrength = (float)value.Get<double>();
            }
        }

        material.index = static_cast<uint32_t>(m_materials.size());
        m_materials.push_back(material);
    }
    // Push a default material at the end of the list for meshes with no material assigned
    m_materials.emplace_back();
}

void ModelLoader::LoadNode(gltf::Model*           pOutModel,
                           gltf::Node*            parent,
                           const tinygltf::Node&  node,
                           uint32_t               nodeIndex,
                           const tinygltf::Model& model,
                           float                  globalScale)

{
    auto* newNode      = new gltf::Node();
    newNode->index     = nodeIndex;
    newNode->parent    = parent;
    newNode->name      = node.name;
    newNode->skinIndex = node.skin;
    newNode->matrix    = Mat4(1.0f);

    if (node.translation.size() == 3)
    {
        newNode->translation = glm::make_vec3(node.translation.data());
    }

    if (node.rotation.size() == 4)
    {
        glm::quat q       = glm::make_quat(node.rotation.data());
        newNode->rotation = Mat4(q);
    }

    newNode->scale = Vec3(globalScale);
    if (node.scale.size() == 3) { newNode->scale = glm::make_vec3(node.scale.data()); }

    if (node.matrix.size() == 16) { newNode->matrix = glm::make_mat4x4(node.matrix.data()); };

    // Node with children
    if (!node.children.empty())
    {
        for (size_t i = 0; i < node.children.size(); i++)
        {
            LoadNode(pOutModel, newNode, model.nodes[node.children[i]], node.children[i], model,
                     globalScale);
        }
    }
    // Combine all transform matrix
    newNode->localMatrix = glm::translate(Mat4(1.0f), newNode->translation) *
        Mat4(newNode->rotation) * glm::scale(Mat4(1.0f), newNode->scale) * newNode->matrix;

    // Node contains mesh data
    if (node.mesh > -1)
    {
        const tinygltf::Mesh& mesh = model.meshes[node.mesh];
        for (const auto& primitive : mesh.primitives)
        {
            uint32_t vertexStart = static_cast<uint32_t>(m_vertexPos);
            uint32_t indexStart  = static_cast<uint32_t>(m_indexPos);
            uint32_t indexCount  = 0;
            uint32_t vertexCount = 0;
            Vec3     posMin{};
            Vec3     posMax{};
            bool     hasSkin    = false;
            bool     hasIndices = primitive.indices > -1;
            // Vertices
            {
                const float* bufferPos          = nullptr;
                const float* bufferNormals      = nullptr;
                const float* bufferTexCoordSet0 = nullptr;
                const float* bufferTexCoordSet1 = nullptr;
                const float* bufferColorSet0    = nullptr;
                const void*  bufferJoints       = nullptr;
                const float* bufferWeights      = nullptr;

                int posByteStride;
                int normByteStride;
                int uv0ByteStride;
                int uv1ByteStride;
                int color0ByteStride;
                int jointByteStride;
                int weightByteStride;

                int jointComponentType;

                // Position attribute is required
                assert(primitive.attributes.find("POSITION") != primitive.attributes.end());

                const tinygltf::Accessor& posAccessor =
                    model.accessors[primitive.attributes.find("POSITION")->second];
                const tinygltf::BufferView& posView = model.bufferViews[posAccessor.bufferView];
                bufferPos                           = reinterpret_cast<const float*>(
                    &(model.buffers[posView.buffer]
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
                        model.accessors[primitive.attributes.find("NORMAL")->second];
                    const tinygltf::BufferView& normView =
                        model.bufferViews[normAccessor.bufferView];
                    bufferNormals = reinterpret_cast<const float*>(
                        &(model.buffers[normView.buffer]
                              .data[normAccessor.byteOffset + normView.byteOffset]));
                    normByteStride = normAccessor.ByteStride(normView) ?
                        (normAccessor.ByteStride(normView) / sizeof(float)) :
                        tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC3);
                }

                // UVs
                if (primitive.attributes.find("TEXCOORD_0") != primitive.attributes.end())
                {
                    const tinygltf::Accessor& uvAccessor =
                        model.accessors[primitive.attributes.find("TEXCOORD_0")->second];
                    const tinygltf::BufferView& uvView = model.bufferViews[uvAccessor.bufferView];
                    bufferTexCoordSet0                 = reinterpret_cast<const float*>(
                        &(model.buffers[uvView.buffer]
                              .data[uvAccessor.byteOffset + uvView.byteOffset]));
                    uv0ByteStride = uvAccessor.ByteStride(uvView) ?
                        (uvAccessor.ByteStride(uvView) / sizeof(float)) :
                        tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC2);
                }
                if (primitive.attributes.find("TEXCOORD_1") != primitive.attributes.end())
                {
                    const tinygltf::Accessor& uvAccessor =
                        model.accessors[primitive.attributes.find("TEXCOORD_1")->second];
                    const tinygltf::BufferView& uvView = model.bufferViews[uvAccessor.bufferView];
                    bufferTexCoordSet1                 = reinterpret_cast<const float*>(
                        &(model.buffers[uvView.buffer]
                              .data[uvAccessor.byteOffset + uvView.byteOffset]));
                    uv1ByteStride = uvAccessor.ByteStride(uvView) ?
                        (uvAccessor.ByteStride(uvView) / sizeof(float)) :
                        tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC2);
                }

                // Vertex colors
                if (primitive.attributes.find("COLOR_0") != primitive.attributes.end())
                {
                    const tinygltf::Accessor& accessor =
                        model.accessors[primitive.attributes.find("COLOR_0")->second];
                    const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
                    bufferColorSet0                  = reinterpret_cast<const float*>(
                        &(model.buffers[view.buffer].data[accessor.byteOffset + view.byteOffset]));
                    color0ByteStride = accessor.ByteStride(view) ?
                        (accessor.ByteStride(view) / sizeof(float)) :
                        tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC3);
                }

                // Skinning
                // Joints
                if (primitive.attributes.find("JOINTS_0") != primitive.attributes.end())
                {
                    const tinygltf::Accessor& jointAccessor =
                        model.accessors[primitive.attributes.find("JOINTS_0")->second];
                    const tinygltf::BufferView& jointView =
                        model.bufferViews[jointAccessor.bufferView];
                    bufferJoints       = &(model.buffers[jointView.buffer]
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
                        model.accessors[primitive.attributes.find("WEIGHTS_0")->second];
                    const tinygltf::BufferView& weightView =
                        model.bufferViews[weightAccessor.bufferView];
                    bufferWeights = reinterpret_cast<const float*>(
                        &(model.buffers[weightView.buffer]
                              .data[weightAccessor.byteOffset + weightView.byteOffset]));
                    weightByteStride = weightAccessor.ByteStride(weightView) ?
                        (weightAccessor.ByteStride(weightView) / sizeof(float)) :
                        tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC4);
                }

                hasSkin = (bufferJoints && bufferWeights);

                for (size_t v = 0; v < posAccessor.count; v++)
                {
                    gltf::Vertex& vert = m_vertices[m_vertexPos];
                    vert.pos           = Vec4(glm::make_vec3(&bufferPos[v * posByteStride]), 1.0f);
                    vert.normal        = glm::normalize(
                        Vec3(bufferNormals ? glm::make_vec3(&bufferNormals[v * normByteStride]) :
                                                    Vec3(0.0f)));
                    vert.uv0   = bufferTexCoordSet0 ?
                          glm::make_vec2(&bufferTexCoordSet0[v * uv0ByteStride]) :
                          Vec3(0.0f);
                    vert.uv1   = bufferTexCoordSet1 ?
                          glm::make_vec2(&bufferTexCoordSet1[v * uv1ByteStride]) :
                          Vec3(0.0f);
                    vert.color = bufferColorSet0 ?
                        glm::make_vec4(&bufferColorSet0[v * color0ByteStride]) :
                        Vec4(1.0f);

                    if (hasSkin)
                    {
                        switch (jointComponentType)
                        {
                            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                            {
                                const uint16_t* buf = static_cast<const uint16_t*>(bufferJoints);
                                vert.joint0 = Vec4(glm::make_vec4(&buf[v * jointByteStride]));
                                break;
                            }
                            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                            {
                                const uint8_t* buf = static_cast<const uint8_t*>(bufferJoints);
                                vert.joint0 = Vec4(glm::make_vec4(&buf[v * jointByteStride]));
                                break;
                            }
                            default:
                                // Not supported by spec
                                std::cerr << "Joint component type " << jointComponentType
                                          << " not supported!" << std::endl;
                                break;
                        }
                    }
                    else { vert.joint0 = Vec4(0.0f); }
                    vert.weight0 =
                        hasSkin ? glm::make_vec4(&bufferWeights[v * weightByteStride]) : Vec4(0.0f);
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
                    model.accessors[primitive.indices > -1 ? primitive.indices : 0];
                const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];

                const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];

                indexCount = static_cast<uint32_t>(accessor.count);

                const void* dataPtr = &(buffer.data[accessor.byteOffset + bufferView.byteOffset]);

                switch (accessor.componentType)
                {
                    case TINYGLTF_PARAMETER_TYPE_UNSIGNED_INT:
                    {
                        const auto* buf = static_cast<const uint32_t*>(dataPtr);
                        for (size_t index = 0; index < accessor.count; index++)
                        {
                            m_indices[m_indexPos] = buf[index] + vertexStart;
                            m_indexPos++;
                        }
                        break;
                    }
                    case TINYGLTF_PARAMETER_TYPE_UNSIGNED_SHORT:
                    {
                        const auto* buf = static_cast<const uint16_t*>(dataPtr);
                        for (size_t index = 0; index < accessor.count; index++)
                        {
                            m_indices[m_indexPos] = buf[index] + vertexStart;
                            m_indexPos++;
                        }
                        break;
                    }
                    case TINYGLTF_PARAMETER_TYPE_UNSIGNED_BYTE:
                    {
                        const auto* buf = static_cast<const uint8_t*>(dataPtr);
                        for (size_t index = 0; index < accessor.count; index++)
                        {
                            m_indices[m_indexPos] = buf[index] + vertexStart;
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
            auto* newPrimitive = new gltf::Primitive(
                indexStart, indexCount, vertexCount,
                primitive.material > -1 ? &m_materials[primitive.material] : &m_materials.back());
            newPrimitive->SetBoundingBox(posMin, posMax);
            newNode->primitives.push_back(newPrimitive);
        }
        // Mesh BB from BBs of primitives
        for (auto* p : newNode->primitives)
        {
            if (p->bb.valid && !newNode->bb.valid)
            {
                newNode->bb       = p->bb;
                newNode->bb.valid = true;
            }
            newNode->bb.min = glm::min(newNode->bb.min, p->bb.min);
            newNode->bb.max = glm::max(newNode->bb.max, p->bb.max);
        }
    }
    if (parent) { parent->children.push_back(newNode); }
    else { pOutModel->nodes.push_back(newNode); }
    if (node.mesh != -1) pOutModel->flatNodes.push_back(newNode);
}

void ModelLoader::GetModelProps(const tinygltf::Node&  node,
                                const tinygltf::Model& model,
                                // sum
                                size_t& vertexCount,
                                size_t& indexCount,
                                size_t& nodeCount)
{
    //    nodeCount += 1 + node.children.size();
    nodeCount++;
    if (!node.children.empty())
    {
        for (int child : node.children)
        {
            if (model.nodes[child].mesh != -1) nodeCount++;
            GetModelProps(model.nodes[child], model, vertexCount, indexCount, nodeCount);
        }
    }
    if (node.mesh > -1)
    {
        const tinygltf::Mesh mesh = model.meshes[node.mesh];
        for (size_t i = 0; i < mesh.primitives.size(); i++)
        {
            auto primitive = mesh.primitives[i];
            vertexCount += model.accessors[primitive.attributes.find("POSITION")->second].count;
            if (primitive.indices > -1) { indexCount += model.accessors[primitive.indices].count; }
        }
    }
}

void ModelLoader::SetModelBoundingBox(gltf::Model* pModel)
{
    for (auto* node : pModel->flatNodes)
    {
        node->bb.Transform(node->GetMatrix());
        pModel->bb.min = glm::min(pModel->bb.min, node->bb.min);
        pModel->bb.max = glm::min(pModel->bb.max, node->bb.max);
    }
}

ModelLoader::ModelLoader()
{
    m_defaultTextures.baseColor.format = Format::R8G8B8A8_UNORM;
    m_defaultTextures.baseColor.height = 1;
    m_defaultTextures.baseColor.width  = 1;
    m_defaultTextures.baseColor.data   = {129, 133, 137, 255};

    m_defaultTextures.metallicRoughness.format = Format::R8G8B8A8_UNORM;
    m_defaultTextures.metallicRoughness.height = 1;
    m_defaultTextures.metallicRoughness.width  = 1;
    m_defaultTextures.metallicRoughness.data   = {0, 255, 0, 255};

    m_defaultTextures.normal.format = Format::R8G8B8A8_UNORM;
    m_defaultTextures.normal.height = 1;
    m_defaultTextures.normal.width  = 1;
    m_defaultTextures.normal.data   = {127, 127, 255, 255};
}

void GltfLoader::LoadFromFile(const std::string& path, sg::Scene* scene)
{
    std::string error;
    std::string warning;

    bool   binary = false;
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
    LoadGltfSamplers(scene);
    LoadGltfTextures(scene);
    LoadGltfMaterials(scene);
    LoadGltfMeshes(scene);
    ParseGltfNodes(scene);
    scene->UpdateAABB();
}

static struct DefaultTextures
{
    sg::Texture* baseColor;
    sg::Texture* metallicRoughness;
    sg::Texture* normal;
} sDefaultTextures;

static void LoadDefaultTextures(uint32_t startIndex)
{
    sDefaultTextures.baseColor            = new sg::Texture("DefaultBaseColor");
    sDefaultTextures.baseColor->format    = Format::R8G8B8A8_UNORM;
    sDefaultTextures.baseColor->index     = startIndex;
    sDefaultTextures.baseColor->height    = 1;
    sDefaultTextures.baseColor->width     = 1;
    sDefaultTextures.baseColor->bytesData = {129, 133, 137, 255};

    sDefaultTextures.metallicRoughness            = new sg::Texture("DefaultMetallicRoughness");
    sDefaultTextures.metallicRoughness->format    = Format::R8G8B8A8_UNORM;
    sDefaultTextures.metallicRoughness->index     = startIndex + 1;
    sDefaultTextures.metallicRoughness->height    = 1;
    sDefaultTextures.metallicRoughness->width     = 1;
    sDefaultTextures.metallicRoughness->bytesData = {0, 255, 0, 255};

    sDefaultTextures.normal            = new sg::Texture("DefaultNormal");
    sDefaultTextures.normal->format    = Format::R8G8B8A8_UNORM;
    sDefaultTextures.normal->index     = startIndex + 2;
    sDefaultTextures.normal->height    = 1;
    sDefaultTextures.normal->width     = 1;
    sDefaultTextures.normal->bytesData = {127, 127, 255, 255};
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

void GltfLoader::LoadGltfSamplers(sg::Scene* scene)
{
    // Load Samplers
    std::vector<UniquePtr<sg::Sampler>> samplers;
    samplers.reserve(m_gltfModel.samplers.size());
    for (const tinygltf::Sampler& s : m_gltfModel.samplers)
    {
        auto* sampler = new sg::Sampler(s.name);
        // set props
        sampler->minFilter = FromTinyGltfFilter(s.minFilter);
        sampler->magFilter = FromTinyGltfFilter(s.magFilter);
        sampler->wrapS     = FromTintGltfWrap(s.wrapS);
        sampler->wrapT     = FromTintGltfWrap(s.wrapT);
        samplers.emplace_back(sampler);
    }
    scene->SetComponents(std::move(samplers));
}

void GltfLoader::LoadGltfTextures(sg::Scene* scene)
{
    std::vector<UniquePtr<sg::Texture>> textures;
    textures.reserve(m_gltfModel.textures.size() + 3);
    uint32_t textureIndex = textures.size();
    // load textures
    for (const tinygltf::Texture& tex : m_gltfModel.textures)
    {
        const tinygltf::Image& gltfImage = m_gltfModel.images[tex.source];

        auto* sgTex = new sg::Texture(gltfImage.uri);

        // temp buffer for format conversion
        unsigned char* buffer = nullptr;
        // delete temp buffer?
        bool   deleteBuffer = false;
        size_t bufferSize;

        // set props
        sgTex->index        = textureIndex;
        sgTex->format       = Format::R8G8B8A8_UNORM;
        sgTex->width        = gltfImage.width;
        sgTex->height       = gltfImage.height;
        sgTex->samplerIndex = tex.sampler;

        if (gltfImage.component == 3)
        {
            // Most devices don't support RGB only on Vulkan so convert if necessary
            // TODO: Check actual format support and transform only if required
            bufferSize = gltfImage.width * gltfImage.height * 4;
            buffer     = new unsigned char[bufferSize];

            unsigned char* rgba = buffer;
            unsigned char* rgb  = const_cast<unsigned char*>(gltfImage.image.data());
            for (int32_t i = 0; i < gltfImage.width * gltfImage.height; ++i)
            {
                for (int32_t j = 0; j < 3; ++j) { rgba[j] = rgb[j]; }
                rgba += 4;
                rgb += 3;
            }
            deleteBuffer     = true;
            sgTex->bytesData = std::move(std::vector<uint8_t>(buffer, buffer + bufferSize));
        }
        else if (gltfImage.component == 4) { sgTex->bytesData = gltfImage.image; }
        // free temp buffer
        if (deleteBuffer) { delete[] buffer; }

        textures.emplace_back(sgTex);
        textureIndex++;
    }
    LoadDefaultTextures(textures.size());
    textures.emplace_back(sDefaultTextures.baseColor);
    textures.emplace_back(sDefaultTextures.metallicRoughness);
    textures.emplace_back(sDefaultTextures.normal);
    scene->SetComponents(std::move(textures));
}

void GltfLoader::LoadGltfMaterials(sg::Scene* scene)
{
    std::vector<UniquePtr<sg::Material>> materials;
    materials.reserve(m_gltfModel.materials.size());
    uint32_t currIndex = 0;
    for (const tinygltf::Material& mat : m_gltfModel.materials)
    {
        auto* sgMat            = new sg::Material(mat.name);
        sgMat->doubleSided     = mat.doubleSided;
        sgMat->alphaCutoff     = mat.alphaCutoff;
        sgMat->baseColorFactor = glm::make_vec4(mat.pbrMetallicRoughness.baseColorFactor.data());
        sgMat->roughnessFactor = mat.pbrMetallicRoughness.roughnessFactor;
        sgMat->metallicFactor  = mat.pbrMetallicRoughness.metallicFactor;
        if (mat.pbrMetallicRoughness.baseColorTexture.index != -1)
        {
            const auto& textureInfo       = mat.pbrMetallicRoughness.baseColorTexture;
            sgMat->texCoordSets.baseColor = textureInfo.texCoord;
            sgMat->baseColorTexture       = scene->GetComponents<sg::Texture>()[textureInfo.index];
        }
        else { sgMat->baseColorTexture = sDefaultTextures.baseColor; }

        if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index != -1)
        {
            const auto& textureInfo = mat.pbrMetallicRoughness.metallicRoughnessTexture;
            sgMat->texCoordSets.metallicRoughness = textureInfo.texCoord;
            sgMat->metallicRoughnessTexture =
                scene->GetComponents<sg::Texture>()[textureInfo.index];
        }
        else { sgMat->metallicRoughnessTexture = sDefaultTextures.metallicRoughness; }

        if (mat.normalTexture.index != -1)
        {
            const auto& textureInfo    = mat.normalTexture;
            sgMat->texCoordSets.normal = textureInfo.texCoord;
            sgMat->normalTexture       = scene->GetComponents<sg::Texture>()[textureInfo.index];
        }
        else { sgMat->normalTexture = sDefaultTextures.normal; }

        if (mat.emissiveTexture.index != -1)
        {
            const auto& textureInfo      = mat.emissiveTexture;
            sgMat->texCoordSets.emissive = textureInfo.texCoord;
            sgMat->emissiveTexture       = scene->GetComponents<sg::Texture>()[textureInfo.index];
        }

        if (mat.occlusionTexture.index != -1)
        {
            const auto& textureInfo       = mat.occlusionTexture;
            sgMat->texCoordSets.occlusion = textureInfo.texCoord;
            sgMat->occlusionTexture       = scene->GetComponents<sg::Texture>()[textureInfo.index];
        }

        if (mat.alphaMode == "BLEND") { sgMat->alphaMode = sg::AlphaMode::Blend; }
        else if (mat.alphaMode == "MASK") { sgMat->alphaMode = sg::AlphaMode::Mask; }

        // Extensions
        // @TODO: Find out if there is a nicer way of reading these properties with recent tinygltf headers
        if (mat.extensions.find("KHR_materials_pbrSpecularGlossiness") != mat.extensions.end())
        {
            auto ext = mat.extensions.find("KHR_materials_pbrSpecularGlossiness");
            if (ext->second.Has("specularGlossinessTexture"))
            {
                auto index = ext->second.Get("specularGlossinessTexture").Get("index");
                sgMat->extension.specularGlossinessTexture =
                    scene->GetComponents<sg::Texture>()[index.Get<int>()];
                auto texCoordSet = ext->second.Get("specularGlossinessTexture").Get("texCoord");
                sgMat->texCoordSets.specularGlossiness = texCoordSet.Get<int>();
                sgMat->pbrWorkflows.specularGlossiness = true;
            }
            if (ext->second.Has("diffuseTexture"))
            {
                auto index = ext->second.Get("diffuseTexture").Get("index");
                sgMat->extension.diffuseTexture =
                    scene->GetComponents<sg::Texture>()[index.Get<int>()];
            }
            if (ext->second.Has("diffuseFactor"))
            {
                auto factor = ext->second.Get("diffuseFactor");
                for (uint32_t i = 0; i < factor.ArrayLen(); i++)
                {
                    auto val = factor.Get(i);
                    sgMat->extension.diffuseFactor[i] =
                        val.IsNumber() ? (float)val.Get<double>() : (float)val.Get<int>();
                }
            }
            if (ext->second.Has("specularFactor"))
            {
                auto factor = ext->second.Get("specularFactor");
                for (uint32_t i = 0; i < factor.ArrayLen(); i++)
                {
                    auto val = factor.Get(i);
                    sgMat->extension.specularFactor[i] =
                        val.IsNumber() ? (float)val.Get<double>() : (float)val.Get<int>();
                }
            }
        }

        if (mat.extensions.find("KHR_materials_unlit") != mat.extensions.end())
        {
            sgMat->unlit = true;
        }

        if (mat.extensions.find("KHR_materials_emissive_strength") != mat.extensions.end())
        {
            auto ext = mat.extensions.find("KHR_materials_emissive_strength");
            if (ext->second.Has("emissiveStrength"))
            {
                auto value              = ext->second.Get("emissiveStrength");
                sgMat->emissiveStrength = (float)value.Get<double>();
            }
        }

        sgMat->index = static_cast<uint32_t>(currIndex);
        currIndex++;
        materials.emplace_back(sgMat);
    }
    // Push a default material at the end of the list for meshes with no material assigned
    materials.emplace_back(sg::Material::CreateDefaultUnique());
    scene->SetComponents(std::move(materials));
}

void GltfLoader::LoadGltfMeshes(sg::Scene* scene)
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
            Vec3     posMin{};
            Vec3     posMax{};
            bool     hasSkin    = false;
            bool     hasIndices = primitive.indices > -1;
            // Vertices
            {
                const float* bufferPos          = nullptr;
                const float* bufferNormals      = nullptr;
                const float* bufferTexCoordSet0 = nullptr;
                const float* bufferTexCoordSet1 = nullptr;
                const float* bufferColorSet0    = nullptr;
                const void*  bufferJoints       = nullptr;
                const float* bufferWeights      = nullptr;

                int posByteStride;
                int normByteStride;
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
                bufferPos = reinterpret_cast<const float*>(
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
                    bufferNormals = reinterpret_cast<const float*>(
                        &(m_gltfModel.buffers[normView.buffer]
                              .data[normAccessor.byteOffset + normView.byteOffset]));
                    normByteStride = normAccessor.ByteStride(normView) ?
                        (normAccessor.ByteStride(normView) / sizeof(float)) :
                        tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC3);
                }

                // UVs
                if (primitive.attributes.find("TEXCOORD_0") != primitive.attributes.end())
                {
                    const tinygltf::Accessor& uvAccessor =
                        m_gltfModel.accessors[primitive.attributes.find("TEXCOORD_0")->second];
                    const tinygltf::BufferView& uvView =
                        m_gltfModel.bufferViews[uvAccessor.bufferView];
                    bufferTexCoordSet0 = reinterpret_cast<const float*>(
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
                    bufferTexCoordSet1 = reinterpret_cast<const float*>(
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
                    bufferColorSet0                  = reinterpret_cast<const float*>(
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
                    bufferJoints       = &(m_gltfModel.buffers[jointView.buffer]
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
                    bufferWeights = reinterpret_cast<const float*>(
                        &(m_gltfModel.buffers[weightView.buffer]
                              .data[weightAccessor.byteOffset + weightView.byteOffset]));
                    weightByteStride = weightAccessor.ByteStride(weightView) ?
                        (weightAccessor.ByteStride(weightView) / sizeof(float)) :
                        tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC4);
                }

                hasSkin = (bufferJoints && bufferWeights);

                for (size_t v = 0; v < posAccessor.count; v++)
                {
                    gltf::Vertex& vert = m_vertices[m_vertexPos];
                    vert.pos           = Vec4(glm::make_vec3(&bufferPos[v * posByteStride]), 1.0f);
                    vert.normal        = glm::normalize(
                        Vec3(bufferNormals ? glm::make_vec3(&bufferNormals[v * normByteStride]) :
                                                    Vec3(0.0f)));
                    vert.uv0   = bufferTexCoordSet0 ?
                          glm::make_vec2(&bufferTexCoordSet0[v * uv0ByteStride]) :
                          Vec3(0.0f);
                    vert.uv1   = bufferTexCoordSet1 ?
                          glm::make_vec2(&bufferTexCoordSet1[v * uv1ByteStride]) :
                          Vec3(0.0f);
                    vert.color = bufferColorSet0 ?
                        glm::make_vec4(&bufferColorSet0[v * color0ByteStride]) :
                        Vec4(1.0f);

                    if (hasSkin)
                    {
                        switch (jointComponentType)
                        {
                            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                            {
                                const uint16_t* buf = static_cast<const uint16_t*>(bufferJoints);
                                vert.joint0 = Vec4(glm::make_vec4(&buf[v * jointByteStride]));
                                break;
                            }
                            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                            {
                                const uint8_t* buf = static_cast<const uint8_t*>(bufferJoints);
                                vert.joint0 = Vec4(glm::make_vec4(&buf[v * jointByteStride]));
                                break;
                            }
                            default:
                                // Not supported by spec
                                std::cerr << "Joint component type " << jointComponentType
                                          << " not supported!" << std::endl;
                                break;
                        }
                    }
                    else { vert.joint0 = Vec4(0.0f); }
                    vert.weight0 =
                        hasSkin ? glm::make_vec4(&bufferWeights[v * weightByteStride]) : Vec4(0.0f);
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

                const void* dataPtr = &(buffer.data[accessor.byteOffset + bufferView.byteOffset]);

                switch (accessor.componentType)
                {
                    case TINYGLTF_PARAMETER_TYPE_UNSIGNED_INT:
                    {
                        const auto* buf = static_cast<const uint32_t*>(dataPtr);
                        for (size_t index = 0; index < accessor.count; index++)
                        {
                            m_indices[m_indexPos] = buf[index] + vertexStart;
                            m_indexPos++;
                        }
                        break;
                    }
                    case TINYGLTF_PARAMETER_TYPE_UNSIGNED_SHORT:
                    {
                        const auto* buf = static_cast<const uint16_t*>(dataPtr);
                        for (size_t index = 0; index < accessor.count; index++)
                        {
                            m_indices[m_indexPos] = buf[index] + vertexStart;
                            m_indexPos++;
                        }
                        break;
                    }
                    case TINYGLTF_PARAMETER_TYPE_UNSIGNED_BYTE:
                    {
                        const auto* buf = static_cast<const uint8_t*>(dataPtr);
                        for (size_t index = 0; index < accessor.count; index++)
                        {
                            m_indices[m_indexPos] = buf[index] + vertexStart;
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
            auto* sgMat = primitive.material > -1 ?
                scene->GetComponents<sg::Material>()[primitive.material] :
                scene->GetComponents<sg::Material>().back();

            subMesh->SetMaterial(primitive.material, sgMat);
            subMesh->SetAABB(posMin, posMax);

            sgMesh->AddSubMesh(subMesh.Get());
            sgMesh->SetAABB(posMin, posMax);

            scene->AddComponent(std::move(subMesh));
            subMeshIndex++;
        }
        scene->AddComponent(std::move(sgMesh));
    }
}

void GltfLoader::ParseGltfNodes(sg::Scene* scene)
{
    std::vector<UniquePtr<sg::Node>> sgNodes;
    sgNodes.reserve(m_gltfModel.nodes.size());
    const tinygltf::Scene& gltfScene =
        m_gltfModel.scenes[m_gltfModel.defaultScene > -1 ? m_gltfModel.defaultScene : 0];
    for (int nodeIndex : gltfScene.nodes)
    {
        const tinygltf::Node& node = m_gltfModel.nodes[nodeIndex];
        ParseGltfNode(nodeIndex, nullptr, sgNodes, scene);
    }

    scene->SetNodes(std::move(sgNodes));
}

void GltfLoader::ParseGltfNode(
    // current node index
    uint32_t nodeIndex,
    // parent node
    sg::Node* parent,
    // store results
    std::vector<UniquePtr<sg::Node>>& sgNodes,
    // access materials
    sg::Scene* scene)
{
    const auto& gltfNode = m_gltfModel.nodes[nodeIndex];

    auto newNode   = MakeUnique<sg::Node>(nodeIndex, gltfNode.name);
    auto transform = MakeUnique<sg::Transform>(*newNode);

    newNode->SetParent(parent);

    if (gltfNode.translation.size() == 3)
    {
        transform->SetTranslation(glm::make_vec3(gltfNode.translation.data()));
    }

    if (gltfNode.rotation.size() == 4)
    {
        transform->SetRotation(glm::make_quat(gltfNode.rotation.data()));
    }

    if (gltfNode.scale.size() == 3) { transform->SetScale(glm::make_vec3(gltfNode.scale.data())); }

    if (gltfNode.matrix.size() == 16)
    {
        transform->SetLocalMatrix(glm::make_mat4x4(gltfNode.matrix.data()));
    }

    newNode->AddComponent(transform.Get());
    scene->AddComponent(transform);

    // Node with children
    if (!gltfNode.children.empty())
    {
        for (size_t i = 0; i < gltfNode.children.size(); i++)
        {
            ParseGltfNode(gltfNode.children[i], newNode.Get(), sgNodes, scene);
        }
    }

    if (gltfNode.mesh > -1)
    {
        auto* sgMesh = scene->GetComponents<sg::Mesh>()[gltfNode.mesh];
        newNode->AddComponent(sgMesh);
        sgMesh->AddNode(newNode.Get());
        // store
        scene->AddRenderableNode(newNode.Get());
    }

    if (parent) { parent->AddChild(newNode.Get()); }
    sgNodes.push_back(newNode);
}

} // namespace zen::gltf