#include "AssetLib/ProcessedSceneAdapter.h"
#include "AssetLib/SceneRuntimeBuilder.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Vulkan/VulkanBuffer.h"
#include "Vulkan/VulkanCommandList.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanRHI.h"
#include "Vulkan/VulkanTexture.h"
#include "SceneGraph/Texture.h"
#include "ZenFoundation/Templates/HashMap.h"
#include <ZenAsset/Reader/Scene.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

namespace
{
using namespace zen;
using Clock = std::chrono::steady_clock;

struct Options
{
    const char* path{nullptr};
    const char* output{nullptr};
    uint32_t    iterations{1};
    uint32_t    warmup{0};
    bool        validation{true};
};

struct Payload
{
    const uint8_t* data{nullptr};
    uint64_t       size{0};
    uint64_t       offset{0};
};

struct TextureUpload
{
    RHITextureCreateInfo                   info;
    HeapVector<Payload>                    payloads;
    HeapVector<RHIBufferTextureCopyRegion> regions;
    HeapVector<RHITextureViewCreateInfo>   layerViews;
    VulkanTexture*                         texture{nullptr};
    VulkanBuffer*                          staging{nullptr};
    uint64_t                               payloadBytes{0};
    const char*                            kind{"scene_texture"};
    uint32_t                               index{0};
};

struct BufferUpload
{
    RHIBufferCreateInfo info;
    Payload             payload;
    VulkanBuffer*       buffer{nullptr};
    VulkanBuffer*       staging{nullptr};
    const char*         kind{nullptr};
};

struct Timings
{
    double read{0}, adapt{0}, runtime{0}, plan{0}, allocate{0}, stage{0};
    double textureRecord{0}, textureSubmit{0}, textureWait{0};
    double geometryRecord{0}, geometrySubmit{0}, geometryWait{0};
    double textureGpu{0}, geometryGpu{0};
};

std::atomic<uint32_t>            validationErrors{0};
VkPhysicalDeviceMemoryProperties memoryProperties{};
FILE*                            output = stdout;

bool DeviceLocal(uint32_t memoryType)
{
    return (memoryProperties.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
}

VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                          VkDebugUtilsMessageTypeFlagsEXT,
                                          const VkDebugUtilsMessengerCallbackDataEXT* data,
                                          void*)
{
    ++validationErrors;
    std::fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
    return VK_FALSE;
}

double Milliseconds(Clock::time_point start, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

std::string JsonString(std::string_view text)
{
    std::string result{"\""};
    for (unsigned char value : text)
    {
        if (value == '"' || value == '\\')
        {
            result.push_back('\\');
            result.push_back(static_cast<char>(value));
        }
        else if (value < 32)
        {
            char escaped[7]{};
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", value);
            result.append(escaped);
        }
        else
        {
            result.push_back(static_cast<char>(value));
        }
    }
    result.push_back('"');
    return result;
}

bool ParseCount(std::string_view value, uint32_t& result)
{
    const std::from_chars_result parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result <= 1000;
}

bool ParseOptions(int argc, char** argv, Options& options)
{
    bool valid = argc >= 2;
    if (valid)
    {
        options.path = argv[1];
        for (int index = 2; valid && index < argc; ++index)
        {
            const std::string_view arg(argv[index]);
            if (arg.starts_with("--iterations="))
            {
                valid = ParseCount(arg.substr(13), options.iterations) && options.iterations != 0;
            }
            else if (arg.starts_with("--warmup="))
            {
                valid = ParseCount(arg.substr(9), options.warmup);
            }
            else if (arg == "--validation=0" || arg == "--validation=1")
            {
                options.validation = arg.back() == '1';
            }
            else if (arg.starts_with("--output=") && arg.size() > 9)
            {
                options.output = argv[index] + 9;
            }
            else
            {
                valid = false;
            }
        }
    }
    return valid;
}

void SetTextureUsage(RHITextureCreateInfo& info)
{
    // Mirrors CreateTextureSampled(copyUsage=true), including its default sampled view.
    info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eTransferSrc,
                             RHITextureUsageFlagBits::eTransferDst);
}

bool AppendLevel(TextureUpload& upload,
                 uint32_t       level,
                 uint32_t       face,
                 const uint8_t* data,
                 uint64_t       size,
                 uint32_t       layerCount = 1)
{
    const uint32_t width    = std::max(1u, upload.info.width >> level);
    const uint32_t height   = std::max(1u, upload.info.height >> level);
    uint64_t       expected = 0;
    const bool     valid    = layerCount > 0 && face < upload.info.arrayLayers && layerCount <= upload.info.arrayLayers - face
                           && GetTextureFormatLevelSize(upload.info.format, width, height, 1, expected) && data != nullptr
                           && expected <= UINT32_MAX / layerCount && size == expected * layerCount && size != 0
                           && upload.payloadBytes <= UINT32_MAX - size;
    if (valid)
    {
        RHIBufferTextureCopyRegion region{};
        region.bufferOffset = upload.payloadBytes;
        region.textureSize  = {width, height, 1};
        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.textureSubresources.mipmap         = level;
        region.textureSubresources.baseArrayLayer = face;
        region.textureSubresources.layerCount     = layerCount;
        upload.payloads.push_back({data, size, upload.payloadBytes});
        upload.regions.push_back(region);
        upload.payloadBytes += size;
    }
    return valid;
}

bool PlanLayerView(TextureUpload& upload, const sg::Texture& source)
{
    RHITextureViewCreateInfo view;
    view.type            = RHITextureType::e2D;
    view.format          = static_cast<DataFormat>(source.format);
    view.baseArrayLayer  = source.GetArrayLayer();
    view.arrayLayers     = 1;
    view.mipLevels       = static_cast<uint32_t>(source.GetMipBytes().size());
    const bool valid     = view.baseArrayLayer < upload.info.arrayLayers && source.GetArrayLayers() == upload.info.arrayLayers
                        && source.width == upload.info.width && source.height == upload.info.height
                        && view.mipLevels == upload.info.mipmaps && view.mipLevels > 0
                        && AreTextureColorViewFormatsCompatible(upload.info.format, view.format);
    bool       duplicate = false;
    for (const RHITextureViewCreateInfo& existing : upload.layerViews)
    {
        duplicate |= existing.format == view.format && existing.baseArrayLayer == view.baseArrayLayer;
    }
    if (valid && !duplicate)
    {
        upload.layerViews.push_back(view);
    }
    return valid;
}

bool PlanTextures(const asset::LoadedScene& loaded, size_t authoredTextureCount, HeapVector<TextureUpload>& uploads)
{
    bool                                       valid    = true;
    const HeapVector<sg::Texture*>             textures = loaded.scene->GetComponents<sg::Texture>();
    HashMap<const asset::ImagePixels*, size_t> pages;
    for (size_t index = 0; valid && index < textures.size(); ++index)
    {
        const sg::Texture&        source = *textures[index];
        const asset::ImagePixels* owner =
            source.GetArrayLayers() > 1 && source.UsesSharedPixels() ? source.pixels.Get() : nullptr;
        const HashMap<const asset::ImagePixels*, size_t>::iterator page = pages.find(owner);
        if (owner != nullptr && page != pages.end())
        {
            valid = PlanLayerView(uploads[page->second], source);
        }
        else
        {
            TextureUpload upload;
            upload.index            = static_cast<uint32_t>(index);
            upload.kind             = index < authoredTextureCount ? "scene_texture" : "scene_default";
            upload.info.type        = RHITextureType::e2D;
            upload.info.format      = static_cast<DataFormat>(source.format);
            upload.info.width       = source.width;
            upload.info.height      = source.height;
            upload.info.mipmaps     = source.GetMipBytes().empty() ? 1u : static_cast<uint32_t>(source.GetMipBytes().size());
            upload.info.arrayLayers = source.GetArrayLayers();
            upload.info.mutableFormat =
                owner != nullptr
                && (upload.info.format == DataFormat::eR8G8B8A8UNORM || upload.info.format == DataFormat::eR8G8B8A8SRGB
                    || upload.info.format == DataFormat::eBC7UNORMBlock || upload.info.format == DataFormat::eBC7SRGBBlock);
            SetTextureUsage(upload.info);
            // The comparison requires explicit identical offline chains. Raw base-only images
            // would trigger runtime mip blits and are a different experiment.
            if (source.GetMipBytes().empty() && GetTextureFormatBlockInfo(upload.info.format).width == 1
                && (source.width > 1 || source.height > 1))
            {
                std::fprintf(stderr, "Texture %zu has no offline mip chain; cook matching raw mip levels first\n", index);
                valid = false;
            }
            for (uint32_t level = 0; valid && level < upload.info.mipmaps; ++level)
            {
                const VectorView<const uint8_t> bytes =
                    source.GetMipBytes().empty()
                        ? source.GetBytes()
                        : VectorView<const uint8_t>(source.GetMipBytes()[level].data(), source.GetMipBytes()[level].size());
                valid = AppendLevel(upload, level, 0, bytes.data(), bytes.size(), upload.info.arrayLayers);
            }
            if (valid && owner != nullptr)
            {
                valid = PlanLayerView(upload, source);
            }
            if (valid)
            {
                if (owner != nullptr)
                {
                    pages.try_emplace(owner, uploads.size());
                }
                uploads.push_back(std::move(upload));
            }
        }
    }
    const sg::SceneAssetData& scene = loaded.scene->GetAssetData();
    if (valid && scene.imageBasedLight >= 0)
    {
        const asset::ImageBasedLightAsset& light = scene.imageBasedLights[scene.imageBasedLight];
        TextureUpload                      upload;
        upload.kind        = "authored_environment";
        upload.index       = static_cast<uint32_t>(scene.imageBasedLight);
        upload.info.type   = RHITextureType::eCube;
        upload.info.format = static_cast<DataFormat>(light.specularFormat);
        upload.info.width = upload.info.height = light.size;
        upload.info.mipmaps                    = light.mipLevels;
        upload.info.arrayLayers                = 6;
        SetTextureUsage(upload.info);
        for (uint32_t level = 0; valid && level < light.mipLevels; ++level)
        {
            for (uint32_t face = 0; valid && face < 6; ++face)
            {
                const size_t   offset     = level * 6 + face;
                const bool     compressed = light.specularBlockMipFaces != nullptr;
                const uint8_t* data = compressed ? (*light.specularBlockMipFaces)[offset].data()
                                                 : reinterpret_cast<const uint8_t*>(light.specularMipFaces[offset].data());
                const uint64_t size = compressed ? (*light.specularBlockMipFaces)[offset].size()
                                                 : light.specularMipFaces[offset].size() * sizeof(Vec4);
                valid               = AppendLevel(upload, level, face, data, size);
            }
        }
        if (valid)
        {
            uploads.push_back(std::move(upload));
        }
    }
    return valid;
}

void PlanGeometry(const asset::SceneAsset& scene, bool rayQuery, HeapVector<BufferUpload>& buffers)
{
    static const asset::Vertex emptyVertex{};
    static const uint32_t      emptyIndex = 0;
    for (uint32_t index = 0; index < 2; ++index)
    {
        BufferUpload upload;
        upload.kind = index == 0 ? "canonical_vertices" : "canonical_indices";
        upload.payload.data =
            index == 0 ? reinterpret_cast<const uint8_t*>(scene.vertices.empty() ? &emptyVertex : scene.vertices.data())
                       : reinterpret_cast<const uint8_t*>(scene.indices.empty() ? &emptyIndex : scene.indices.data());
        upload.payload.size      = index == 0 ? std::max<size_t>(1, scene.vertices.size()) * sizeof(asset::Vertex)
                                              : std::max<size_t>(1, scene.indices.size()) * sizeof(uint32_t);
        upload.info.size         = upload.payload.size;
        upload.info.allocateType = RHIBufferAllocateType::eGPU;
        upload.info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eTransferDstBuffer, RHIBufferUsageFlagBits::eStorageBuffer,
                                        index == 0 ? RHIBufferUsageFlagBits::eVertexBuffer
                                                   : RHIBufferUsageFlagBits::eIndexBuffer);
        if (rayQuery)
        {
            upload.info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eAccelerationStructureInput,
                                            RHIBufferUsageFlagBits::eDeviceAddress);
        }
        buffers.push_back(upload);
    }
}

VulkanBuffer* MakeStaging(VulkanRHI& rhi, uint64_t size)
{
    RHIBufferCreateInfo info{};
    info.size         = size;
    info.allocateType = RHIBufferAllocateType::eCPUWrite;
    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferSrcBuffer);
    return static_cast<VulkanBuffer*>(rhi.CreateBuffer(info));
}

bool Allocate(VulkanRHI& rhi, HeapVector<TextureUpload>& textures, HeapVector<BufferUpload>& buffers)
{
    bool valid = true;
    for (TextureUpload& upload : textures)
    {
        if (valid)
        {
            upload.texture = static_cast<VulkanTexture*>(rhi.CreateTexture(upload.info));
            upload.staging = MakeStaging(rhi, upload.payloadBytes);
            valid = upload.texture != nullptr && upload.texture->GetDefaultView() != nullptr && upload.staging != nullptr;
            for (const RHITextureViewCreateInfo& view : upload.layerViews)
            {
                if (valid)
                {
                    valid = upload.texture->CreateView(view) != nullptr;
                }
            }
        }
    }
    for (BufferUpload& upload : buffers)
    {
        if (valid)
        {
            upload.buffer  = static_cast<VulkanBuffer*>(rhi.CreateBuffer(upload.info));
            upload.staging = MakeStaging(rhi, upload.payload.size);
            valid          = upload.buffer != nullptr && upload.staging != nullptr;
        }
    }
    return valid;
}

bool Stage(HeapVector<TextureUpload>& textures, HeapVector<BufferUpload>& buffers)
{
    bool valid = true;
    for (TextureUpload& upload : textures)
    {
        uint8_t* mapped  = upload.staging->Map();
        valid           &= mapped != nullptr;
        if (mapped != nullptr)
        {
            for (const Payload& payload : upload.payloads)
            {
                std::memcpy(mapped + payload.offset, payload.data, static_cast<size_t>(payload.size));
            }
        }
        upload.staging->Unmap();
    }
    for (BufferUpload& upload : buffers)
    {
        uint8_t* mapped  = upload.staging->Map();
        valid           &= mapped != nullptr;
        if (mapped != nullptr)
        {
            std::memcpy(mapped, upload.payload.data, static_cast<size_t>(upload.payload.size));
        }
        upload.staging->Unmap();
    }
    return valid;
}

void Transition(VulkanCommandListContext& context, VulkanTexture& texture, bool initialize)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask       = initialize ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask       = initialize ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout           = initialize ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout           = initialize ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                                             = texture.GetVkImage();
    barrier.subresourceRange                                  = texture.GetVkSubresourceRange();
    vkCmdPipelineBarrier(context.GetCommandBuffer()->GetVkHandle(),
                         initialize ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                         initialize ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
}

bool Submit(VulkanCommandListContext& context, const RHIGPUTimingPtr& gpu, double& submitMs, double& waitMs, double& gpuUs)
{
    const Clock::time_point start = Clock::now();
    bool valid = !context.GetRecordingError().IsFailure() && context.SubmitRecordedWorkloads() == RHISubmissionResult::eSuccess;
    const Clock::time_point submitted = Clock::now();
    if (valid)
    {
        valid = context.GetQueue()->WaitForCompletion(context.GetQueue()->GetLastSubmittedSerial(), UINT64_MAX);
    }
    const Clock::time_point completed = Clock::now();
    submitMs                          = Milliseconds(start, submitted);
    waitMs                            = Milliseconds(submitted, completed);
    if (valid)
    {
        valid = gpu->GetStatus() == RHIGPUTimingStatus::eAvailable && std::isfinite(gpu->GetMicroseconds());
        if (valid)
        {
            gpuUs = gpu->GetMicroseconds();
        }
        else
        {
            std::fprintf(stderr, "GPU timestamp unavailable: %s\n", RHIGPUTimingStatusName(gpu->GetStatus()));
        }
    }
    return valid;
}

bool Upload(VulkanRHI& rhi, HeapVector<TextureUpload>& textures, HeapVector<BufferUpload>& buffers, Timings& times)
{
    bool valid = true;
    {
        VulkanCommandListContext context(RHICommandContextType::eGraphics, rhi.GetDevice());
        const RHIGPUTimingPtr    gpu   = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();
        const Clock::time_point  start = Clock::now();
        context.RHIBeginGPUTiming(gpu);
        for (TextureUpload& upload : textures)
        {
            Transition(context, *upload.texture, true);
            context.RHICopyBufferToTexture(upload.staging, upload.texture, MakeVecView(upload.regions));
            Transition(context, *upload.texture, false);
        }
        context.RHIEndGPUTiming(gpu);
        times.textureRecord = Milliseconds(start, Clock::now());
        valid               = Submit(context, gpu, times.textureSubmit, times.textureWait, times.textureGpu);
    }
    if (valid)
    {
        VulkanCommandListContext context(RHICommandContextType::eGraphics, rhi.GetDevice());
        const RHIGPUTimingPtr    gpu   = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();
        const Clock::time_point  start = Clock::now();
        context.RHIBeginGPUTiming(gpu);
        for (BufferUpload& upload : buffers)
        {
            RHIBufferCopyRegion region{};
            region.size = upload.payload.size;
            context.RHICopyBuffer(upload.staging, upload.buffer, region);
        }
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(context.GetCommandBuffer()->GetVkHandle(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        context.RHIEndGPUTiming(gpu);
        times.geometryRecord = Milliseconds(start, Clock::now());
        valid                = Submit(context, gpu, times.geometrySubmit, times.geometryWait, times.geometryGpu);
    }
    return valid;
}

void PrintResources(uint32_t                         iteration,
                    bool                             warmup,
                    const HeapVector<TextureUpload>& textures,
                    const HeapVector<BufferUpload>&  buffers)
{
    for (const TextureUpload& upload : textures)
    {
        const VmaAllocationInfo& memory  = upload.texture->GetMemoryAllocation().info;
        const VmaAllocationInfo& staging = upload.staging->GetMemoryAllocation().info;
        std::fprintf(
            output,
            "ZEN_UPLOAD_RESOURCE {\"iteration\":%u,\"warmup\":%s,\"kind\":\"%s\",\"index\":%u,"
            "\"native_view_count\":%zu,\"copy_region_count\":%zu,"
            "\"format\":%u,\"width\":%u,\"height\":%u,\"mips\":%u,\"layers\":%u,\"payload_bytes\":%llu,"
            "\"allocation_bytes\":%llu,\"memory_type\":%u,\"device_local\":%s,\"staging_allocation_bytes\":%llu,\"staging_memory_type\":%u,\"staging_device_local\":%s}\n",
            iteration, warmup ? "true" : "false", upload.kind, upload.index, upload.layerViews.size() + 1,
            upload.regions.size(), uint32_t(upload.info.format), upload.info.width, upload.info.height, upload.info.mipmaps,
            upload.info.arrayLayers, upload.payloadBytes, uint64_t(memory.size), memory.memoryType,
            DeviceLocal(memory.memoryType) ? "true" : "false", uint64_t(staging.size), staging.memoryType,
            DeviceLocal(staging.memoryType) ? "true" : "false");
    }
    for (const BufferUpload& upload : buffers)
    {
        const VmaAllocationInfo& memory  = upload.buffer->GetMemoryAllocation().info;
        const VmaAllocationInfo& staging = upload.staging->GetMemoryAllocation().info;
        std::fprintf(
            output,
            "ZEN_UPLOAD_RESOURCE {\"iteration\":%u,\"warmup\":%s,\"kind\":\"%s\",\"payload_bytes\":%llu,"
            "\"allocation_bytes\":%llu,\"memory_type\":%u,\"device_local\":%s,\"staging_allocation_bytes\":%llu,\"staging_memory_type\":%u,\"staging_device_local\":%s}\n",
            iteration, warmup ? "true" : "false", upload.kind, upload.payload.size, uint64_t(memory.size), memory.memoryType,
            DeviceLocal(memory.memoryType) ? "true" : "false", uint64_t(staging.size), staging.memoryType,
            DeviceLocal(staging.memoryType) ? "true" : "false");
    }
}

void PrintSummary(uint32_t                         iteration,
                  bool                             warmup,
                  const Timings&                   times,
                  const HeapVector<TextureUpload>& textures,
                  const HeapVector<BufferUpload>&  buffers,
                  const RHIGPUMemoryStats&         before,
                  const RHIGPUMemoryStats&         peak)
{
    uint64_t texturePayload = 0, textureAllocation = 0, geometryPayload = 0, geometryAllocation = 0, stagingAllocation = 0;
    uint64_t nativeViews = 0, copyRegions = 0;
    for (const TextureUpload& upload : textures)
    {
        texturePayload    += upload.payloadBytes;
        textureAllocation += upload.texture->GetMemoryAllocation().info.size;
        stagingAllocation += upload.staging->GetMemoryAllocation().info.size;
        nativeViews       += upload.layerViews.size() + 1;
        copyRegions       += upload.regions.size();
    }
    for (const BufferUpload& upload : buffers)
    {
        geometryPayload    += upload.payload.size;
        geometryAllocation += upload.buffer->GetMemoryAllocation().info.size;
        stagingAllocation  += upload.staging->GetMemoryAllocation().info.size;
    }
    std::fprintf(
        output,
        "ZEN_UPLOAD_SAMPLE {\"iteration\":%u,\"warmup\":%s,\"texture_count\":%zu,"
        "\"native_view_count\":%llu,\"copy_region_count\":%llu,"
        "\"texture_payload_bytes\":%llu,\"texture_allocation_bytes\":%llu,\"geometry_payload_bytes\":%llu,"
        "\"geometry_allocation_bytes\":%llu,\"explicit_staging_peak_allocation_bytes\":%llu,"
        "\"vma_committed_before_bytes\":%llu,\"vma_committed_resources_bytes\":%llu,\"vma_process_peak_committed_bytes\":%llu,"
        "\"read_cpu_ms\":%.6f,\"adapt_cpu_ms\":%.6f,\"runtime_cpu_ms\":%.6f,\"plan_cpu_ms\":%.6f,"
        "\"allocate_cpu_ms\":%.6f,\"stage_cpu_ms\":%.6f,\"texture_record_cpu_ms\":%.6f,\"texture_submit_cpu_ms\":%.6f,"
        "\"texture_wait_cpu_ms\":%.6f,\"geometry_record_cpu_ms\":%.6f,\"geometry_submit_cpu_ms\":%.6f,\"geometry_wait_cpu_ms\":%.6f,"
        "\"graphics_texture_upload_gpu_us\":%.6f,\"graphics_geometry_upload_gpu_us\":%.6f,\"validation_errors\":%u}\n",
        iteration, warmup ? "true" : "false", textures.size(), nativeViews, copyRegions, texturePayload, textureAllocation,
        geometryPayload, geometryAllocation, stagingAllocation, before.committedBytes, peak.committedBytes,
        peak.peakCommittedBytes, times.read, times.adapt, times.runtime, times.plan, times.allocate, times.stage,
        times.textureRecord, times.textureSubmit, times.textureWait, times.geometryRecord, times.geometrySubmit,
        times.geometryWait, times.textureGpu, times.geometryGpu, validationErrors.load(std::memory_order_relaxed));
}

bool Measure(VulkanRHI& rhi, const Options& options, uint32_t iteration, bool warmup)
{
    Timings                         times;
    Clock::time_point               start   = Clock::now();
    const zenasset::SceneReadResult decoded = zenasset::ReadScene(options.path);
    times.read                              = Milliseconds(start, Clock::now());
    bool                      valid         = decoded.status.Succeeded() && decoded.scene != nullptr;
    asset::SceneImportResult  adapted;
    asset::SceneLoadResult    runtime;
    HeapVector<TextureUpload> textures;
    HeapVector<BufferUpload>  buffers;
    const RHIGPUMemoryStats   before = rhi.GetGPUMemoryStats();
    if (valid)
    {
        start       = Clock::now();
        adapted     = asset::AdaptProcessedScene(*decoded.scene);
        times.adapt = Milliseconds(start, Clock::now());
        valid       = adapted.asset != nullptr;
    }
    if (valid)
    {
        start         = Clock::now();
        runtime       = asset::BuildSceneRuntime(adapted.asset);
        times.runtime = Milliseconds(start, Clock::now());
        valid         = runtime.scene.Get() != nullptr;
    }
    if (valid)
    {
        start = Clock::now();
        valid = PlanTextures(*runtime.scene, adapted.asset->textures.size(), textures);
        PlanGeometry(*adapted.asset, rhi.QueryGPUInfo().rayQuery.IsUsable(), buffers);
        times.plan = Milliseconds(start, Clock::now());
    }
    if (valid)
    {
        start          = Clock::now();
        valid          = Allocate(rhi, textures, buffers);
        times.allocate = Milliseconds(start, Clock::now());
    }
    if (valid)
    {
        start       = Clock::now();
        valid       = Stage(textures, buffers);
        times.stage = Milliseconds(start, Clock::now());
    }
    if (valid)
    {
        valid = Upload(rhi, textures, buffers, times) && validationErrors == 0;
    }
    if (valid)
    {
        PrintResources(iteration, warmup, textures, buffers);
        PrintSummary(iteration, warmup, times, textures, buffers, before, rhi.GetGPUMemoryStats());
    }
    else
    {
        std::fprintf(stderr, "Upload benchmark failed: read=%s adapt=%s runtime=%s\n", decoded.status.message.c_str(),
                     adapted.error.c_str(), runtime.error.c_str());
    }
    rhi.WaitDeviceIdle();
    for (TextureUpload& upload : textures)
    {
        if (upload.texture != nullptr)
        {
            rhi.DestroyTexture(upload.texture);
        }
        if (upload.staging != nullptr)
        {
            rhi.DestroyBuffer(upload.staging);
        }
    }
    for (BufferUpload& upload : buffers)
    {
        if (upload.buffer != nullptr)
        {
            rhi.DestroyBuffer(upload.buffer);
        }
        if (upload.staging != nullptr)
        {
            rhi.DestroyBuffer(upload.staging);
        }
    }
    return valid;
}
} // namespace

int main(int argc, char** argv)
{
    Options options;
    bool    valid = ParseOptions(argc, argv, options);
    if (valid && options.output != nullptr)
    {
        output = std::fopen(options.output, "wb");
        valid  = output != nullptr;
    }
    if (valid)
    {
        zen::rc::RenderConfig::GetInstance().rhiDevice.SetValidationEnabled(options.validation);
        VulkanRHI               rhi(zen::rc::RenderConfig::GetInstance().rhiDevice);
        const Clock::time_point start     = Clock::now();
        const RHIError          initError = rhi.InitChecked();
        ZEN_RHI_VERIFY_MSG_F(!initError.IsFailure(), "{} failed ({})", initError.operation, initError.nativeCode);
        const double             deviceInit = Milliseconds(start, Clock::now());
        VkDebugUtilsMessengerEXT messenger  = VK_NULL_HANDLE;
        if (options.validation)
        {
            VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            info.pfnUserCallback = Validation;
            valid                = vkCreateDebugUtilsMessengerEXT(rhi.GetInstance(), &info, nullptr, &messenger) == VK_SUCCESS;
        }
        const VkPhysicalDeviceProperties& properties = rhi.GetDevice()->GetPhysicalDeviceProperties();
        vkGetPhysicalDeviceMemoryProperties(rhi.GetDevice()->GetPhysicalDeviceHandle(), &memoryProperties);
        std::fprintf(
            output,
            "ZEN_UPLOAD_RUN {\"schema\":1,\"package\":%s,\"device\":%s,\"driver_version\":%u,"
            "\"api_version\":%u,\"queue_family\":%u,\"timestamp_period_ns\":%.6f,\"validation\":%s,\"device_init_cpu_ms\":%.6f,"
            "\"scope\":\"isolated_graphics_queue_upload\",\"os_file_cache\":\"uncontrolled\","
            "\"texture_policy\":\"standalone_bindings_or_shared_array_pages_with_layer_views_including_five_defaults_and_selected_authored_cube\","
            "\"excluded\":\"runtime_mip_blits,wood_fallback,external_environment,SH_irradiance,BRDF_LUT,UV_material_instance_buffers,deformation,acceleration_structures,production_staging_pool,async_transfer\"}\n",
            JsonString(options.path).c_str(), JsonString(properties.deviceName).c_str(), properties.driverVersion,
            properties.apiVersion, rhi.GetDevice()->GetGfxQueue()->GetFamilyIndex(), properties.limits.timestampPeriod,
            options.validation ? "true" : "false", deviceInit);
        for (uint32_t index = 0; valid && index < options.warmup + options.iterations; ++index)
        {
            valid = Measure(rhi, options, index, index < options.warmup);
        }
        if (messenger != VK_NULL_HANDLE)
        {
            vkDestroyDebugUtilsMessengerEXT(rhi.GetInstance(), messenger, nullptr);
        }
        rhi.Destroy();
        valid = valid && validationErrors == 0;
    }
    else
    {
        std::fprintf(stderr,
                     "Usage: SceneUploadBenchmark <package> [--iterations=1..1000] [--warmup=0..1000] [--validation=0|1]\n");
    }
    if (output != nullptr && output != stdout)
    {
        valid = std::fclose(output) == 0 && valid;
    }
    return valid ? 0 : 1;
}
