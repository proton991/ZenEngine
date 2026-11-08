#include "Graphics/RenderCore/V2/SceneOpacityMicromap.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "AssetLib/AssetHash.h"
#include "Templates/FlatHashMap.h"
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace zen::rc
{
namespace
{
constexpr uint32_t kMaxStateBytes = 32u * 1024u * 1024u;
constexpr uint64_t kCacheVersion  = 1;
struct Pattern
{
    uint32_t values[9]{};
    bool     operator==(const Pattern&) const = default;
};
struct PatternHash
{
    size_t operator()(const Pattern& pattern) const
    {
        asset::AssetHash hash;
        hash.Update(&pattern, sizeof(pattern));
        return static_cast<size_t>(hash.Finish());
    }
};
struct CacheHeader
{
    uint64_t version;
    uint64_t key;
    uint64_t bytes;
    uint64_t checksum;
};

bool NormalizedAlpha(DataFormat format)
{
    return format == DataFormat::eR8G8B8A8UNORM || format == DataFormat::eR8G8B8A8SRGB || format == DataFormat::eB8G8R8A8UNORM
        || format == DataFormat::eB8G8R8A8SRGB || format == DataFormat::eBC7UNORMBlock || format == DataFormat::eBC7SRGBBlock;
}

bool ReadCache(const std::string& path, uint64_t key, HeapVector<uint32_t>& states)
{
    std::ifstream  input(path, std::ios::binary | std::ios::ate);
    const uint64_t bytes = states.size() * sizeof(uint32_t);
    bool           valid = input && input.tellg() == static_cast<std::streamoff>(sizeof(CacheHeader) + bytes);
    if (valid)
    {
        input.seekg(0);
        CacheHeader header{};
        input.read(reinterpret_cast<char*>(&header), sizeof(header));
        valid = input && header.version == kCacheVersion && header.key == key && header.bytes == bytes;
        if (valid)
        {
            input.read(reinterpret_cast<char*>(states.data()), static_cast<std::streamsize>(bytes));
            asset::AssetHash hash;
            hash.Update(states.data(), bytes);
            valid = input && hash.Finish() == header.checksum;
        }
    }
    if (!valid)
    {
        std::fill(states.begin(), states.end(), 0u);
    }
    return valid;
}

void WriteCache(const std::string& path, uint64_t key, const void* states, uint32_t bytes)
{
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    if (!error)
    {
        asset::AssetHash hash;
        hash.Update(states, bytes);
        const CacheHeader header{kCacheVersion, key, bytes, hash.Finish()};
        const std::string temporary =
            path + "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        output.write(static_cast<const char*>(states), bytes);
        output.close();
        if (output)
        {
            std::filesystem::rename(temporary, path, error);
            // Another process may already have published this immutable entry.
            // A corrupt old entry is replaced on the next successful bake.
            if (error)
            {
                HeapVector<uint32_t> existing(bytes / sizeof(uint32_t));
                if (!ReadCache(path, key, existing))
                {
                    std::filesystem::remove(path, error);
                    if (!error)
                    {
                        std::filesystem::rename(temporary, path, error);
                    }
                }
            }
        }
        else
        {
            LOGW("Could not write opacity cache '{}'", path);
        }
        std::filesystem::remove(temporary, error);
    }
}
} // namespace

SceneOpacityMicromap::~SceneOpacityMicromap()
{
    PollCache();
    m_device->DeferReleaseResource(m_structure);
    for (RHIBuffer* buffer : {m_mapping, m_states, m_triangles, m_corners, m_readback})
    {
        m_device->DestroyBuffer(buffer);
    }
}

SharedPtr<SceneOpacityMicromap> SceneOpacityMicromap::Prepare(RenderDevice*                           device,
                                                              RenderScene&                            scene,
                                                              const RHIAccelerationStructureGeometry& geometry,
                                                              uint32_t                                material,
                                                              const SharedPtr<SceneOpacityMicromap>&  previous)
{
    SharedPtr<SceneOpacityMicromap> result;
    // Small primitives do not amortize the extra build/storage. Uncommitted CPU
    // vertices cannot describe coverage in the current GPU snapshot.
    if (device->GetGPUInfo().rayQuery.opacityMicromap && !geometry.opaque && geometry.indexCount >= 384
        && geometry.indexCount % 3 == 0 && material < scene.GetMaterialsData().size() && !scene.HasUncommittedVertices())
    {
        SharedPtr<SceneOpacityMicromap> candidate = MakeShared<SceneOpacityMicromap>(device);
        bool                            reused    = false;
        if (candidate->Initialize(scene, geometry, material, previous, reused))
        {
            result = reused ? previous : candidate;
        }
    }
    return result;
}

bool SceneOpacityMicromap::Initialize(RenderScene&                            scene,
                                      const RHIAccelerationStructureGeometry& geometry,
                                      uint32_t                                materialIndex,
                                      const SharedPtr<SceneOpacityMicromap>&  previous,
                                      bool&                                   reused)
{
    const sg::MaterialData& material = scene.GetMaterialsData()[materialIndex];
    const bool              specular = material.surfaceProperties.w > .5f;
    const int      textureIndex = specular ? static_cast<int>(material.featureTextures[2].properties.x) : material.bcTexIndex;
    const uint32_t uvSet        = static_cast<uint32_t>(
        std::max(specular ? static_cast<int>(material.featureTextures[2].properties.y) : material.bcTexSet, 0));
    const sg::TextureTransformData& transform =
        specular ? material.featureTextures[2].transform : material.textureTransforms[0];
    const float factor = specular ? material.diffuseFactor.a : material.baseColorFactor.a;
    const float cutoff = material.surfaceProperties.y == float(sg::AlphaMode::Mask) ? material.surfaceProperties.x : .5f;
    bool        valid  = std::isfinite(factor) && factor >= 0 && std::isfinite(cutoff);
    for (uint32_t component = 0; component < 4; ++component)
    {
        valid &= std::isfinite(transform.row0[component]) && std::isfinite(transform.row1[component]);
    }
    const uint32_t       samplerIndex = valid ? static_cast<uint32_t>(std::max(transform.row0.w, 0.0f)) : 0;
    RHISamplerCreateInfo sampler      = RHISamplerCreateInfo::CreateLinearRepeat();
    if (samplerIndex > 0)
    {
        valid &= samplerIndex <= scene.GetSceneSamplers().size();
        if (valid)
        {
            sampler = scene.GetSceneSamplers()[samplerIndex - 1]->GetCreateInfo();
        }
    }
    m_samplerFlags       = SamplerCoverageFlags(sampler);
    uint64_t textureHash = 0;
    Vec2     dimensions(1);
    if (textureIndex >= 0)
    {
        valid = valid && (m_samplerFlags & 2u) != 0 && m_device->GetGPUInfo().subTexelPrecisionBits > 0
             && uint32_t(textureIndex) < scene.GetSceneTextureViews().size();
        if (valid)
        {
            const RHITextureView* view = scene.GetSceneTextureViews()[textureIndex];
            valid      = NormalizedAlpha(view->GetFormat()) && scene.GetTextureContentHash(textureIndex, textureHash);
            dimensions = Vec2(view->GetTexture()->GetWidth(), view->GetTexture()->GetHeight());
        }
    }
    const uint64_t first = geometry.indexOffset / sizeof(uint32_t);
    valid = valid && first <= scene.GetIndices().size() && geometry.indexCount <= scene.GetIndices().size() - first;
    FlatHashMap<Pattern, uint32_t, PatternHash> patterns;
    HeapVector<Vec4>                            corners;
    HeapVector<uint32_t>                        mapping;
    float                                       extent = 1.0f;
    for (uint32_t triangle = 0; valid && triangle < geometry.indexCount / 3; ++triangle)
    {
        Pattern key;
        Vec4    values[3];
        Vec2    lower(FLT_MAX), upper(-FLT_MAX);
        for (uint32_t corner = 0; valid && corner < 3; ++corner)
        {
            const uint32_t vertex = scene.GetIndices()[first + triangle * 3 + corner];
            valid                 = vertex < scene.GetVertices().size();
            if (valid)
            {
                const Vec2  uv    = scene.GetVertexUV(vertex, uvSet);
                const float alpha = scene.GetVertices()[vertex].color.a;
                valid             = std::isfinite(uv.x) && std::isfinite(uv.y) && std::isfinite(alpha) && alpha >= 0;
                values[corner]    = Vec4(uv, alpha, 0);
                for (uint32_t component = 0; component < 3; ++component)
                {
                    key.values[corner * 3 + component] = std::bit_cast<uint32_t>(values[corner][component]);
                }
                const Vec3 coordinate(uv, 1);
                const Vec2 transformed(glm::dot(Vec3(transform.row0), coordinate), glm::dot(Vec3(transform.row1), coordinate));
                lower = glm::min(lower, transformed);
                upper = glm::max(upper, transformed);
            }
        }
        if (valid)
        {
            const Vec2 pixels = (upper - lower) * dimensions;
            extent            = std::max(extent, std::max(pixels.x, pixels.y));
            const std::pair<FlatHashMap<Pattern, uint32_t, PatternHash>::iterator, bool> inserted =
                patterns.try_emplace(key, uint32_t(patterns.size()));
            if (inserted.second)
            {
                corners.insert(corners.end(), std::begin(values), std::end(values));
            }
            mapping.push_back(inserted.first->second);
            // Bound both storage and the Z dispatch dimension. Unique, arbitrary
            // UV meshes retain ordinary ray candidates instead of a huge bake.
            valid = patterns.size() <= 65535 && patterns.size() <= m_device->GetGPUInfo().rayQuery.maxMicromapTriangles;
        }
    }
    uint32_t level = 0;
    while (float(1u << level) * 2.0f < extent
           && level < std::min(10u, m_device->GetGPUInfo().rayQuery.maxOpacitySubdivisionLevel))
    {
        ++level;
    }
    while (level > 0 && patterns.size() * std::max(4u, (1u << (2 * level)) / 4u) > kMaxStateBytes)
    {
        --level;
    }
    m_usage                     = {static_cast<uint32_t>(patterns.size()), level};
    const uint32_t patternBytes = std::max(4u, (1u << (2 * level)) / 4u);
    m_stateBytes                = m_usage.count * patternBytes;
    m_material                  = materialIndex;
    valid                       = valid && m_usage.count > 0 && m_stateBytes <= kMaxStateBytes;
    if (valid)
    {
        asset::AssetHash  key;
        const RHIGPUInfo& gpu        = m_device->GetGPUInfo();
        const uint32_t    settings[] = {gpu.vendorID,
                                        gpu.deviceID,
                                        gpu.driverVersionRaw,
                                        gpu.subTexelPrecisionBits,
                                        level,
                                        m_samplerFlags,
                                        uint32_t(sampler.repeatU),
                                        uint32_t(sampler.repeatV),
                                        uint32_t(sampler.borderColor)};
        uint64_t          shaderHash = 0;
        valid = asset::HashAssetFile(std::string(SPV_SHADER_PATH) + "VoxelGI/opacity_micromap_bake.comp.spv", shaderHash);
        key.Update(settings, sizeof(settings));
        key.Update(&textureHash, sizeof(textureHash));
        key.Update(&shaderHash, sizeof(shaderHash));
        key.Update(&factor, sizeof(factor));
        key.Update(&cutoff, sizeof(cutoff));
        key.Update(&transform, sizeof(transform));
        const Vec4 sampling(material.textureSampling.x, material.textureSampling.y, 0.0f, 0.0f);
        key.Update(&sampling, sizeof(sampling));
        key.Update(corners.data(), corners.size() * sizeof(Vec4));
        m_key = key.Finish();
        asset::AssetHash mappingHash;
        mappingHash.Update(mapping.data(), mapping.size() * sizeof(uint32_t));
        m_mappingHash = mappingHash.Finish();
        reused        = previous && previous->m_key == m_key && previous->m_mappingHash == m_mappingHash
                     && (previous->m_built || previous->m_material == m_material);
        if (valid && !reused)
        {
            m_cachePath = (std::filesystem::path(ZEN_CONFIG_PATH).parent_path().parent_path() / "Cache" / "OpacityMicromaps"
                           / (asset::DigestText(m_key) + ".omm"))
                              .string();
            HeapVector<uint32_t> states(m_stateBytes / sizeof(uint32_t), 0u);
            m_cached = ReadCache(m_cachePath, m_key, states);
            HeapVector<glm::uvec2> triangles;
            for (uint32_t pattern = 0; pattern < m_usage.count; ++pattern)
            {
                triangles.push_back({pattern * patternBytes, level | (2u << 16u)});
            }
            m_mapping = Upload(static_cast<uint32_t>(mapping.size() * sizeof(uint32_t)), mapping.data(), "opacity_indices");
            m_states  = Upload(m_stateBytes, states.data(), "opacity_states");
            m_triangles =
                Upload(static_cast<uint32_t>(triangles.size() * sizeof(glm::uvec2)), triangles.data(), "opacity_triangles");
            if (!m_cached)
            {
                m_corners = Upload(static_cast<uint32_t>(corners.size() * sizeof(Vec4)), corners.data(), "opacity_uv_alpha");
                RHIBufferCreateInfo readback;
                readback.size         = m_stateBytes;
                readback.allocateType = RHIBufferAllocateType::eCPURead;
                readback.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
                readback.tag = "opacity_cache_readback";
                m_readback   = m_device->CreateBuffer(readback);
            }
            valid = m_mapping && m_states && m_triangles && (m_cached || (m_corners && m_readback));
            if (valid)
            {
                // This packed SSBO starts at offset zero. Keep ordinary candidates
                // if its native address cannot satisfy the micromap input alignment.
                valid   = m_states->GetDeviceAddress() % 256u == 0;
                m_sizes = GDynamicRHI->GetAccelerationStructureBuildSizes(Description());
                valid   = valid && m_sizes.IsValid();
            }
            if (valid)
            {
                RHIAccelerationStructureCreateInfo info;
                info.type   = RHIAccelerationStructureType::eOpacityMicromap;
                info.size   = m_sizes.storageSize;
                info.tag    = "scene_opacity_micromap";
                m_structure = GDynamicRHI->CreateAccelerationStructure(info);
                valid       = m_structure != nullptr;
            }
            if (valid)
            {
                LOGI("Opacity micromap: material {}, {} triangles, {} patterns, level {}, {} state bytes, cache {}",
                     materialIndex, mapping.size(), m_usage.count, level, m_stateBytes, m_cached ? "hit" : "miss");
            }
            else
            {
                LOGW("Opacity micromap allocation unavailable; retaining ordinary alpha candidates");
            }
        }
    }
    return valid;
}

RHIBuffer* SceneOpacityMicromap::Upload(uint32_t bytes, const void* data, NameID name)
{
    RHIBufferCreateInfo info;
    info.size         = bytes;
    info.allocateType = RHIBufferAllocateType::eGPU;
    info.tag          = name;
    info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eDeviceAddress,
                             RHIBufferUsageFlagBits::eAccelerationStructureInput, RHIBufferUsageFlagBits::eTransferSrcBuffer,
                             RHIBufferUsageFlagBits::eTransferDstBuffer);
    RHIBuffer* buffer = m_device->CreateBuffer(info);
    if (buffer != nullptr && !m_device->UpdateBuffer(buffer, bytes, static_cast<const uint8_t*>(data)))
    {
        m_device->DestroyBuffer(buffer);
        buffer = nullptr;
    }
    return buffer;
}

RHIAccelerationStructureBuildDesc SceneOpacityMicromap::Description() const
{
    RHIAccelerationStructureBuildDesc desc;
    desc.type                     = RHIAccelerationStructureType::eOpacityMicromap;
    desc.micromap.pDataBuffer     = m_states;
    desc.micromap.pTriangleBuffer = m_triangles;
    desc.micromap.dataSize        = m_stateBytes;
    desc.micromap.usages          = MakeVecView(&m_usage, 1);
    return desc;
}

void SceneOpacityMicromap::Attach(RHIAccelerationStructureGeometry& geometry) const
{
    geometry.pOpacityMicromap    = m_structure;
    geometry.pOpacityIndexBuffer = m_mapping;
}

void SceneOpacityMicromap::Record(RenderScene& scene, RHIBuffer* scratch, uint64_t scratchOffset)
{
    if (!m_built && !m_recorded)
    {
        RenderGraph& graph = *m_device->GetCurrentFrameRDG();
        if (!m_cached)
        {
            ShaderProgramManager& programs = ShaderProgramManager::GetInstance();
            if (programs.RequestShaderProgram("SceneOpacityBakeSP") == nullptr)
            {
                programs.StoreProgram(
                    ZEN_NEW() ComputeFileSP(m_device, "SceneOpacityBakeSP", "VoxelGI/opacity_micromap_bake.comp.spv"));
            }
            RDGComputePassDesc pass;
            pass.SetShaderProgramName("SceneOpacityBakeSP");
            pass.BindStorageBuffer("MicromapTriangles", m_corners);
            pass.BindStorageBuffer("MaterialBuffer", scene.GetMaterialsDataSSBO());
            pass.BindStorageBuffer("MicromapStates", m_states);
            BindSceneTextureArray(pass, m_device->CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat()),
                                  scene.GetSceneTextureViews(), scene.GetSceneSamplers());
            const std::array<uint32_t, 5> parameters{m_material, m_usage.subdivisionLevel, m_usage.count, m_samplerFlags,
                                                     m_device->GetGPUInfo().subTexelPrecisionBits};
            graph.AddComputePass(std::move(pass)).RecordPassCommands([parameters](RDGPassCmdEncoder& encoder) {
                encoder.SetPushConstants(parameters.data(), uint32_t(sizeof(parameters)), 0);
                const uint32_t groups = ((1u << parameters[1]) + 7u) / 8u;
                encoder.Dispatch(groups, groups, parameters[2]);
            });
            graph.AddTransferPass("ReadOpacityCache").CopyBuffer(m_states, m_readback, {0, 0, m_stateBytes});
        }
        RHIAccelerationStructureBuildInfo build;
        build.description    = Description();
        build.pDestination   = m_structure;
        build.pScratchBuffer = scratch;
        build.scratchOffset  = scratchOffset;
        graph.AddTransferPass("BuildSceneOpacityMicromap").BuildAccelerationStructure(build);
        m_recorded = true;
    }
}

void SceneOpacityMicromap::OnExecuted(RenderFrameResult result)
{
    if (m_recorded)
    {
        m_built        = result == RenderFrameResult::eSubmitted;
        m_cachePending = m_built && !m_cached;
        if (m_cachePending)
        {
            m_retirement = m_device->CaptureResourceRetirement();
        }
        m_recorded = false;
    }
}

void SceneOpacityMicromap::PollCache()
{
    if (m_cachePending && m_device->IsResourceRetired(m_retirement))
    {
        const void* states = m_readback->Map();
        if (states != nullptr)
        {
            WriteCache(m_cachePath, m_key, states, m_stateBytes);
            m_readback->Unmap();
        }
        m_cachePending = false;
    }
}

uint64_t SceneOpacityMicromap::GetBufferBytes() const
{
    uint64_t bytes = 0;
    for (const RHIBuffer* buffer : {m_mapping, m_states, m_triangles, m_corners, m_readback})
    {
        bytes += buffer != nullptr ? buffer->GetRequiredSize() : 0;
    }
    return bytes;
}
} // namespace zen::rc
