#pragma once

#include "Graphics/RenderCore/V2/RenderGraph/RDGDefs.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include <algorithm>

namespace zen::rc
{
// Internal copy validation shared by RDG declarations and staging uploads.
inline bool IsDepthStencilCopyFormat(DataFormat format)
{
    return FormatIsDepthOnly(format) || FormatIsStencilOnly(format) || FormatIsDepthStencil(format);
}

inline uint32_t CopyTexelBytes(DataFormat format, BitField<RHITextureAspectFlagBits> aspect)
{
    uint32_t result = 1;

    if (!aspect.HasFlag(RHITextureAspectFlagBits::eStencil))
    {
        switch (format)
        {
            case DataFormat::eR8UNORM:
            case DataFormat::eR8UInt: result = 1; break;
            case DataFormat::eD16UNORM:
            case DataFormat::eD16UNORMS8UInt: result = 2; break;
            case DataFormat::eR8G8B8SRGB:
            case DataFormat::eR8G8B8UNORM: result = 3; break;
            case DataFormat::eR8G8B8A8UInt:
            case DataFormat::eR8G8B8A8SRGB:
            case DataFormat::eR8G8B8A8UNORM:
            case DataFormat::eD32SFloat:
            case DataFormat::eD24UNORMS8UInt:
            case DataFormat::eD32SFloatS8UInt: result = 4; break;
            default:
                const uint32_t bytes = GetTextureFormatPixelSize(format);
                result               = bytes == 0x7fffffff ? 0 : bytes;
                break;
        }
    }

    return result;
}

inline uint32_t BufferTextureCopyAlignment(const RHITextureCreateInfo& info,
                                           const RHIBufferTextureCopyRegion& region)
{
    return IsDepthStencilCopyFormat(info.format) ?
        4 :
        CopyTexelBytes(info.format, region.textureSubresources.aspect);
}

inline bool ValidateTextureCopyBox(RDGResult& result,
                                   const RHITextureCreateInfo& info,
                                   NameID name,
                                   const RHITextureSubresourceLayers& layers,
                                   const Vec3i& offset,
                                   const Vec3i& size)
{
    bool valid = true;

    const BitField<RHITextureAspectFlagBits> aspect = FormatIsDepthStencil(info.format) ?
        RHITextureSubResourceRange::DepthStencil().aspect :
        FormatIsDepthOnly(info.format)   ? RHITextureSubResourceRange::Depth().aspect :
        FormatIsStencilOnly(info.format) ? RHITextureSubResourceRange::Stencil().aspect :
                                           RHITextureSubResourceRange::Color().aspect;
    valid                                           = result.Check(
        layers.mipmap < info.mipmaps && layers.mipmap < 32 && layers.layerCount > 0 &&
            layers.baseArrayLayer < info.arrayLayers &&
            layers.layerCount <= info.arrayLayers - layers.baseArrayLayer &&
            !layers.aspect.IsEmpty() && (int64_t(layers.aspect) & ~int64_t(aspect)) == 0,
        RDGErrorCode::eRange, "Invalid copy subresources for '" + name.ToString() + "'");
    valid = valid &&
        (result.Check(info.type < RHITextureType::eMax &&
                          (info.type != RHITextureType::e1D || (offset.y == 0 && size.y == 1)) &&
                          (info.type == RHITextureType::e3D || (offset.z == 0 && size.z == 1)) &&
                          (info.type != RHITextureType::e3D ||
                           (layers.baseArrayLayer == 0 && layers.layerCount == 1)),
                      RDGErrorCode::eRange,
                      "Copy box does not match texture dimension for '" + name.ToString() + "'"));

    if (valid)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            const uint32_t extent = axis == 0 ? info.width : axis == 1 ? info.height : info.depth;
            const uint32_t mipExtent = std::max(1u, extent >> layers.mipmap);
            valid = result.Check(offset[axis] >= 0 && size[axis] > 0 &&
                                     uint64_t(offset[axis]) + size[axis] <= mipExtent,
                                 RDGErrorCode::eRange,
                                 "Copy box exceeds texture '" + name.ToString() + "'");

            if (!valid)
            {
                break;
            }
        }
    }

    return valid;
}

// Call after ValidateTextureCopyBox has established positive extents and layer count.
// bufferSize is the accessible source span, not necessarily the native buffer capacity.
inline bool ValidateBufferTextureFootprint(RDGResult& result,
                                           uint64_t bufferSize,
                                           const RHITextureCreateInfo& texture,
                                           const RHIBufferTextureCopyRegion& region,
                                           uint64_t* footprint = nullptr)
{
    bool valid = true;

    valid = result.Check(region.bufferOffset <= bufferSize, RDGErrorCode::eRange,
                         "Buffer-to-texture source offset is out of bounds");

    if (valid)
    {
        uint64_t bytes        = CopyTexelBytes(texture.format, region.textureSubresources.aspect);
        const uint64_t aspect = uint64_t(int64_t(region.textureSubresources.aspect));
        const uint32_t alignment = BufferTextureCopyAlignment(texture, region);
        valid = result.Check(bytes != 0 && texture.samples == SampleCount::e1, RDGErrorCode::eRange,
                             "Unsupported buffer-to-texture copy format or sample count");
        valid = valid &&
            (result.Check(
                aspect != 0 && (aspect & (aspect - 1)) == 0 && region.bufferOffset % alignment == 0,
                RDGErrorCode::eRange,
                "Buffer-to-texture copy requires one aspect and an aligned buffer offset"));

        if (valid)
        {
            const uint64_t available = bufferSize - region.bufferOffset;

            for (const uint64_t count :
                 {uint64_t(region.textureSize.x), uint64_t(region.textureSize.y),
                  uint64_t(region.textureSize.z), uint64_t(region.textureSubresources.layerCount)})
            {
                valid = result.Check(count <= available / bytes, RDGErrorCode::eRange,
                                     "Buffer-to-texture copy exceeds the source buffer");

                if (valid)
                {
                    bytes *= count;
                }

                if (!valid)
                {
                    break;
                }
            }

            if ((valid) && (footprint != nullptr))
            {
                *footprint = bytes;
            }
        }
    }

    return valid;
}

// Call after box validation. Non-multiple extents are legal at a mip edge.
inline bool QueueSupportsCopyBox(const RHIQueueCopyCapabilities& queue,
                                 const RHITextureCreateInfo& info,
                                 uint32_t mip,
                                 const Vec3i& offset,
                                 const Vec3i& size)
{
    bool valid = true;

    valid = queue.transfer;

    if (valid)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            const uint32_t extent = axis == 0 ? info.width : axis == 1 ? info.height : info.depth;
            const uint32_t mipExtent   = std::max(1u, extent >> mip);
            const uint32_t granularity = queue.minImageTransferGranularity[axis];

            if (granularity == 0)
            {
                valid = !(offset[axis] != 0 || uint32_t(size[axis]) != mipExtent);
            }
            else if (uint32_t(offset[axis]) % granularity != 0 ||
                     (uint32_t(size[axis]) % granularity != 0 &&
                      uint64_t(offset[axis]) + size[axis] != mipExtent))
            {
                valid = false;
            }

            if (!valid)
            {
                break;
            }
        }
    }

    return valid;
}

inline bool SupportsBufferTextureCopy(const RHITextureCreateInfo& info,
                                      const RHIBufferTextureCopyRegion& region,
                                      RHICommandContextType type)
{
    const RHIQueueCopyCapabilities queue = RenderDevice::GetQueueCopyCapabilities(type);

    // The backend does not enable maintenance10/11: depth/stencil uploads need graphics,
    // and transfer-only queues require four-byte buffer offsets.
    return QueueSupportsCopyBox(queue, info, region.textureSubresources.mipmap,
                                region.textureOffset, region.textureSize) &&
        (!IsDepthStencilCopyFormat(info.format) || queue.graphics) &&
        (queue.graphics || queue.compute || region.bufferOffset % 4 == 0);
}

inline bool ValidateBufferTextureCopyCapabilities(RDGResult& result,
                                                  const RHITextureCreateInfo& info,
                                                  const RHIBufferTextureCopyRegion& region,
                                                  RDGTransferQueueCapabilities& queues)
{
    bool returnValue{};

    if (result.Check(RenderDevice::GetTextureCopyCapabilities(info.format).transferDst,
                     RDGErrorCode::eBinding,
                     "Texture format does not support transfer destination"))
    {
        if (result.Check(SupportsBufferTextureCopy(info, region, RHICommandContextType::eGraphics),
                         RDGErrorCode::eBinding,
                         "Buffer-to-texture copy is unsupported by the graphics queue"))
        {
            queues.transfer &=
                SupportsBufferTextureCopy(info, region, RHICommandContextType::eTransfer);
            queues.compute &=
                SupportsBufferTextureCopy(info, region, RHICommandContextType::eAsyncCompute);
            returnValue = true;
        }
    }

    return returnValue;
}

inline bool SupportsTextureCopy(const RHITextureCreateInfo& src,
                                const RHITextureCreateInfo& dst,
                                const RHITextureCopyRegion& region,
                                RHICommandContextType type)
{
    const RHIQueueCopyCapabilities queue = RenderDevice::GetQueueCopyCapabilities(type);

    return QueueSupportsCopyBox(queue, src, region.srcSubresources.mipmap, region.srcOffset,
                                region.size) &&
        QueueSupportsCopyBox(queue, dst, region.dstSubresources.mipmap, region.dstOffset,
                             region.size) &&
        (queue.graphics || !IsDepthStencilCopyFormat(src.format) || src.samples == SampleCount::e1);
}

inline bool ValidateTextureCopyCapabilities(RDGResult& result,
                                            const RHITextureCreateInfo& src,
                                            const RHITextureCreateInfo& dst,
                                            const RHITextureCopyRegion& region,
                                            RDGTransferQueueCapabilities& queues)
{
    bool returnValue{};

    // Keep the same-format, same-sample contract for both raw and logical texture copies.
    if (result.Check(src.format == dst.format && src.samples == dst.samples, RDGErrorCode::eRange,
                     "Texture copy requires matching formats and samples"))
    {
        const RHITextureCopyCapabilities caps =
            RenderDevice::GetTextureCopyCapabilities(src.format);

        if (result.Check(caps.transferSrc && caps.transferDst, RDGErrorCode::eBinding,
                         "Texture format does not support image copies"))
        {
            if (result.Check(
                    SupportsTextureCopy(src, dst, region, RHICommandContextType::eGraphics),
                    RDGErrorCode::eBinding, "Texture copy is unsupported by the graphics queue"))
            {
                queues.transfer &=
                    SupportsTextureCopy(src, dst, region, RHICommandContextType::eTransfer);
                queues.compute &=
                    SupportsTextureCopy(src, dst, region, RHICommandContextType::eAsyncCompute);
                returnValue = true;
            }
        }
    }

    return returnValue;
}

inline bool ValidateMipmapCapabilities(RDGResult& result, const RHITextureCreateInfo& info)
{
    const RHITextureCopyCapabilities caps = RenderDevice::GetTextureCopyCapabilities(info.format);

    return result.Check(info.mipmaps > 0 && info.mipmaps <= 32 && info.samples == SampleCount::e1 &&
                            !IsDepthStencilCopyFormat(info.format),
                        RDGErrorCode::eRange,
                        "Linear mip generation requires a single-sampled color texture") &&
        result.Check(
            caps.transferSrc && caps.transferDst && caps.blitSrc && caps.blitDst &&
                caps.linearFilter &&
                RenderDevice::GetQueueCopyCapabilities(RHICommandContextType::eGraphics).graphics,
            RDGErrorCode::eBinding, "Texture format does not support linear mip blits");
}
} // namespace zen::rc
