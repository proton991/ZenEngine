#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/Renderer/ForwardDrawCache.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "SceneGraph/Texture.h"
#include "AssetLib/AssetHash.h"
#include <algorithm>
#include <iterator>

namespace zen::rc
{
bool RenderScene::SetTransparencySettings(const TransparencySettings& settings)
{
    const bool valid = ValidateTransparencySettings(settings);
    if (valid && settings != m_transparency)
    {
        m_transparency      = settings;
        m_transparencyDirty = true;
    }
    return valid;
}

bool RenderScene::IsNearlyBinaryTexture(uint32_t texture, const sg::TextureTransformData& transform)
{
    const HeapVector<sg::Texture*>& textures = m_pScene->GetComponents<sg::Texture>();
    bool valid = texture < textures.size() && texture < m_sceneTextures.size() && m_sceneTextures[texture] != nullptr;
    RHISamplerCreateInfo sampler      = RHISamplerCreateInfo::CreateLinearRepeat();
    const uint32_t       samplerIndex = static_cast<uint32_t>(transform.row0.w);
    if (samplerIndex > 0 && samplerIndex <= m_sceneSamplers.size())
    {
        sampler = m_sceneSamplers[samplerIndex - 1]->GetCreateInfo();
    }
    valid = valid && samplerIndex <= m_sceneSamplers.size() && (SamplerCoverageFlags(sampler) & 2u) != 0
         && sampler.repeatU != RHISamplerRepeatMode::eClampToBorder && sampler.repeatV != RHISamplerRepeatMode::eClampToBorder;
    if (valid)
    {
        HashMap<uint32_t, HeapVector<asset::AlphaMipStatistics>>::iterator found = m_textureAlphaStatistics.find(texture);
        if (found == m_textureAlphaStatistics.end())
        {
            const sg::Texture&                    source     = *textures[texture];
            HeapVector<asset::AlphaMipStatistics> statistics = source.alphaMips;
            if (statistics.empty())
            {
                SharedPtr<const asset::ImagePixels, MultiThreadCounter> pixels;
                const bool  deferred = source.UsesSharedPixels() && source.pixels->payloadSource;
                std::string error;
                valid = !deferred || asset::ResolveImagePixels(source.pixels, pixels, &error);
                const sg::Texture  resident(source, pixels);
                const sg::Texture& input  = deferred ? resident : source;
                const uint32_t     levels = static_cast<uint32_t>(std::max(size_t(1), input.GetMipBytes().size()));
                uint32_t           width  = input.width;
                uint32_t           height = input.height;
                for (uint32_t level = 0; valid && level < levels; ++level)
                {
                    asset::AlphaMipStatistics mip;
                    valid = asset::AnalyzeImageAlpha(input.format, width, height, input.GetLayerMipBytes(level),
                                                     input.GetAtlasRegion(), mip, error);
                    statistics.push_back(mip);
                    width  = std::max(1u, width / 2);
                    height = std::max(1u, height / 2);
                }
                if (!valid)
                {
                    statistics.clear();
                    LOGD("Opaque-core statistics unavailable for '{}': {}", source.GetName(), error);
                }
            }
            found = m_textureAlphaStatistics.emplace(texture, std::move(statistics)).first;
        }
        const HeapVector<asset::AlphaMipStatistics>& statistics = found->second;
        const uint32_t levels = sampler.maxLod == 0.0f ? 1u : m_sceneTextures[texture]->GetTexture()->GetNumMipmaps();
        valid                 = !statistics.empty() && statistics.size() >= levels;
        for (uint32_t level = 0; valid && level < levels; ++level)
        {
            const asset::AlphaMipStatistics& mip = statistics[level];
            // A conservative range includes every mip the sampler can access. Unknown
            // runtime-generated levels keep blending. Count mixed bilinear footprints,
            // not just binary source texels or the texture's mostly empty background.
            const bool linear =
                sampler.minFilter == RHISamplerFilter::eLinear || sampler.magFilter == RHISamplerFilter::eLinear;
            const uint64_t zero = linear ? mip.zeroCells : mip.zeroTexels;
            const uint64_t one  = linear ? mip.oneCells : mip.oneTexels;
            valid               = mip.texels != 0 && zero <= mip.texels && one != 0 && one <= mip.texels - zero;
            if (valid)
            {
                const uint64_t visible = mip.texels - zero;
                valid                  = double(visible - one) / double(visible) < 0.02;
                // Trilinear interpolation can mix opposite endpoints between mips.
                // Only fully opaque levels establish nearly binary filtering then.
                valid = valid && (levels == 1 || sampler.mipFilter == RHISamplerFilter::eNearest || one == mip.texels);
            }
        }
    }
    return valid;
}

void RenderScene::RefreshTransparency()
{
    HeapVector<uint8_t> selected(m_materialsData.size(), 0);
    const bool          deferred = SupportsDeferredOpaque(m_materialsData);
    for (uint32_t index = 0; index < m_materialsData.size(); ++index)
    {
        const sg::MaterialData& material = m_materialsData[index];
        bool                    split    = deferred && m_transparency.coreMode != TransparencyCoreMode::eDisabled
                                        && material.surfaceProperties.y == static_cast<float>(sg::AlphaMode::Blend)
                                        && IsBasicForwardMaterial(material) && material.volumeIridescence.x == 0.0f;
        if (split && m_transparency.coreMode == TransparencyCoreMode::eAutomatic)
        {
            split = material.baseColorFactor.a == 1.0f
                 && (material.bcTexIndex < 0
                     || IsNearlyBinaryTexture(static_cast<uint32_t>(material.bcTexIndex), material.textureTransforms[0]));
        }
        selected[index] = split ? 1 : 0;
    }
    if (m_transparency.coreMode == TransparencyCoreMode::eAutomatic)
    {
        for (const sg::Node* node : GetRenderableNodes())
        {
            for (const sg::SubMesh* mesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                const uint32_t material = mesh->GetMaterial()->index;
                if (selected[material] != 0 && mesh->topology == sg::MeshTopology::Triangles)
                {
                    for (uint32_t index = 0; selected[material] != 0 && index < mesh->GetIndexCount(); ++index)
                    {
                        selected[material] = m_vertices[m_indices[mesh->GetFirstIndex() + index]].color.a == 1.0f ? 1 : 0;
                    }
                }
            }
        }
    }
    for (uint32_t index = 0; index < m_materialsData.size(); ++index)
    {
        const float cutoff = selected[index] != 0 ? m_transparency.coreCutoff : 0.0f;
        if (m_materialsData[index].textureSampling.z != cutoff)
        {
            m_materialsData[index].textureSampling.z = cutoff;
            m_materialBuffers.MarkDirty(index * sizeof(sg::MaterialData), sizeof(sg::MaterialData));
            m_materialsDirty       = true;
            m_dirtySurfaceClasses |= GI_ALL;
            std::fill(m_dirtyShadowInstances.begin(), m_dirtyShadowInstances.end(), uint8_t(1));
        }
    }
    m_transparencyDirty = false;
}

bool RenderScene::GetTextureContentHash(uint32_t texture, uint64_t& hash)
{
    const HashMap<uint32_t, uint64_t>::const_iterator cached = m_textureContentHashes.find(texture);
    bool                                              valid  = cached != m_textureContentHashes.end();
    if (valid)
    {
        hash = cached->second;
    }
    else
    {
        const HeapVector<sg::Texture*>& textures = m_pScene->GetComponents<sg::Texture>();
        valid                                    = texture < textures.size() && texture < m_sceneTextures.size();
        if (valid)
        {
            const sg::Texture*                                      source = textures[texture];
            SharedPtr<const asset::ImagePixels, MultiThreadCounter> pixels;
            const bool  deferred = source->UsesSharedPixels() && source->pixels->payloadSource;
            std::string error;
            valid = !deferred || asset::ResolveImagePixels(source->pixels, pixels, &error);
            const sg::Texture               resident(*source, pixels);
            const sg::Texture&              input = deferred ? resident : *source;
            const VectorView<const uint8_t> bytes = input.GetLayerMipBytes(0);
            valid                                 = valid && !bytes.empty();
            if (valid)
            {
                asset::AssetHash content;
                const uint32_t   description[] = {input.width, input.height, static_cast<uint32_t>(input.format)};
                content.Update(description, sizeof(description));
                content.Update(bytes.data(), bytes.size());
                hash                            = content.Finish();
                m_textureContentHashes[texture] = hash;
            }
            else
            {
                LOGW("Cannot fingerprint texture '{}' for opacity caching: {}", source->GetName(), error);
            }
        }
    }
    return valid;
}

std::string RenderScene::GetResourceError(const RHIGPUInfo& gpu, const sg::Scene& scene, uint64_t vertices, uint64_t indices)
{
    std::string error = GetSceneBindlessCapacityError(gpu.bindlessHeapCapacities, scene.GetComponents<sg::Texture>().size(),
                                                      scene.GetComponents<sg::Sampler>().size());

    for (const sg::Texture* texture : scene.GetComponents<sg::Texture>())
    {
        if (error.empty() && texture->GetAtlasRegion() != asset::ImageRegion{}
            && (gpu.subTexelPrecisionBits == 0 || gpu.subTexelPrecisionBits > 23))
        {
            error = fmt::format("Scene texture '{}' has a spatial region that requires representable GPU subtexel precision.",
                                texture->GetName());
        }
    }

    // Storage helpers align their allocation in a 32-bit size. Validate before that
    // programmer assertion and before any multiplication/narrowing at scene upload.
    uint64_t uvStride = 2;

    for (const sg::VertexTextureCoordinates& sets : scene.GetAssetData().vertexTexCoords)
    {
        uvStride = std::max(uvStride, uint64_t(sets.size()));
    }

    const uint64_t uvCount = vertices > (UINT64_MAX - 1) / uvStride ? UINT64_MAX : 1 + vertices * uvStride;

    uint64_t triangles     = 0;

    for (const sg::Node* node : scene.GetRenderableNodes())
    {
        for (const sg::SubMesh* mesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            if (mesh->topology == sg::MeshTopology::Triangles && triangles <= UINT32_MAX / sizeof(glm::uvec4))
            {
                triangles += mesh->GetIndexCount() / 3;
            }
        }
    }

    struct BufferSize
    {
        const char* name;
        uint64_t    count;
        uint32_t    stride;
        bool        padded;
    };

    const BufferSize buffers[] = {
        {"vertices", std::max(vertices, uint64_t(1)), sizeof(asset::Vertex), false},
        {"indices", std::max(indices, uint64_t(1)), sizeof(uint32_t), false},
        {"texture coordinates", uvCount, sizeof(Vec2), true},
        {"nodes", std::max(uint64_t(scene.GetRenderableCount()), uint64_t(1)), sizeof(sg::NodeData), true},
        {"materials", std::max(uint64_t(scene.GetComponents<sg::Material>().size()), uint64_t(1)), sizeof(sg::MaterialData),
         true},
        {"voxel triangles", triangles, sizeof(glm::uvec4), true}};

    for (const BufferSize& buffer : buffers)
    {
        uint64_t bytes  = 0;

        const bool fits = buffer.padded
                            ? ValidateGIStorageBuffer(buffer.count, buffer.stride, gpu, bytes) == GIResourceStatus::eSuccess
                            : buffer.count <= gpu.maxStorageBufferRange / buffer.stride;

        if (error.empty() && !fits)
        {
            error = fmt::format("Scene {} exceed the supported GPU buffer size or allocation alignment limit.", buffer.name);
        }
    }

    return error;
}

bool RenderScene::HasRequiredResources() const
{
    bool valid = GetVertexBuffer() != nullptr && m_pIndexBuffer != nullptr && GetUVBuffer() != nullptr
              && GetNodesDataSSBO() != nullptr && GetMaterialsDataSSBO() != nullptr
              && (m_voxelTriangleCount == 0 || m_pVoxelTriangleBuffer != nullptr)
              && m_sceneTextures.size() == m_pScene->GetComponents<sg::Texture>().size()
              && m_sceneSamplers.size() == m_pScene->GetComponents<sg::Sampler>().size();

    for (RHITextureView* view : m_sceneTextures)
    {
        valid = valid && view != nullptr && view->GetTexture() != nullptr && view->GetTextureType() == RHITextureType::e2D
             && view->GetSubResourceRange().layerCount == 1;
    }

    for (RHISampler* sampler : m_sceneSamplers)
    {
        valid = valid && sampler != nullptr;
    }

    valid = valid && m_envTexture->IsComplete();

    return valid;
}

void RenderScene::Destroy()
{
    if (m_pRenderDevice != nullptr)
    {
        m_pRenderDevice->DestroyBuffer(m_pIndexBuffer);

        m_pRenderDevice->DestroyBuffer(m_pVoxelTriangleBuffer);

        m_vertexBuffers.Destroy(*m_pRenderDevice);

        m_uvBuffers.Destroy(*m_pRenderDevice);

        m_nodeBuffers.Destroy(*m_pRenderDevice);

        m_materialBuffers.Destroy(*m_pRenderDevice);

        m_pIndexBuffer         = nullptr;

        m_pVoxelTriangleBuffer = nullptr;

        m_voxelTriangleCount   = 0;

        m_instanceTriangleRanges.clear();

        m_materialTriangleCounts.clear();

        m_numIndices = 0;

        HeapVector<RHITexture*> images;
        images.reserve(m_sceneTextures.size());
        for (RHITextureView* view : m_sceneTextures)
        {
            if (view != nullptr && std::find(images.begin(), images.end(), view->GetTexture()) == images.end())
            {
                images.push_back(view->GetTexture());
            }
        }
        m_sceneTextures.clear();
        for (RHITexture* image : images)
        {
            m_pRenderDevice->ReleaseSceneTexture(image);
        }

        m_pRenderDevice->ReleaseSceneEnvironment(m_envTexture.Get());

        // Samplers and the default file texture belong to device caches.
        m_sceneSamplers.clear();

        m_pDefaultBaseColorTexture = nullptr;
    }
}
} // namespace zen::rc
