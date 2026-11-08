#include "Graphics/VulkanRHI/VulkanAccelerationStructure.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanResourceAllocator.h"
#include <cstring>
#include <functional>

namespace zen
{
namespace
{
bool ValidInput(RHIBuffer* buffer, uint64_t offset, uint64_t size, uint64_t alignment)
{
    bool valid = buffer != nullptr && buffer->GetUsageFlags().HasFlag(RHIBufferUsageFlagBits::eAccelerationStructureInput)
              && buffer->GetDeviceAddress() != 0 && offset <= buffer->GetRequiredSize();
    if (valid)
    {
        valid = size <= buffer->GetRequiredSize() - offset && (buffer->GetDeviceAddress() + offset) % alignment == 0;
    }
    return valid;
}

struct NativeBuild
{
    VkAccelerationStructureBuildGeometryInfoKHR          info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    HeapVector<VkAccelerationStructureGeometryKHR>       geometries;
    HeapVector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    HeapVector<uint32_t>                                 counts;
    HeapVector<VkAccelerationStructureTrianglesOpacityMicromapKHR> opacity;
    HeapVector<VkMicromapUsageKHR>                                 usages;
    VkAccelerationStructureGeometryMicromapDataKHR                 micromap{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_MICROMAP_DATA_KHR};

    explicit NativeBuild(const RHIAccelerationStructureBuildDesc& desc, bool addresses, uint64_t instanceAddress = 0)
    {
        info.type  = static_cast<VkAccelerationStructureTypeKHR>(desc.type);
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                   | (desc.allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
        info.mode  = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        if (desc.type == RHIAccelerationStructureType::eTopLevel)
        {
            VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geometry.geometryType             = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
            geometry.geometry.instances.data.deviceAddress = instanceAddress;
            geometries.push_back(geometry);
            counts.push_back(static_cast<uint32_t>(desc.instances.size()));
            ranges.push_back({static_cast<uint32_t>(desc.instances.size()), 0, 0, 0});
        }
        else if (desc.type == RHIAccelerationStructureType::eOpacityMicromap)
        {
            for (const RHIOpacityMicromapUsage& usage : desc.micromap.usages)
            {
                usages.push_back({usage.count, usage.subdivisionLevel, VK_OPACITY_MICROMAP_FORMAT_4_STATE_KHR});
            }
            micromap.usageCountsCount    = static_cast<uint32_t>(usages.size());
            micromap.pUsageCounts        = usages.data();
            micromap.triangleArrayStride = sizeof(VkMicromapTriangleKHR);
            if (addresses)
            {
                micromap.data          = desc.micromap.pDataBuffer->GetDeviceAddress() + desc.micromap.dataOffset;
                micromap.triangleArray = desc.micromap.pTriangleBuffer->GetDeviceAddress() + desc.micromap.triangleOffset;
            }
            VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geometry.geometryType = VK_GEOMETRY_TYPE_MICROMAP_KHR;
            geometry.pNext        = &micromap;
            geometries.push_back(geometry);
        }
        else
        {
            opacity.reserve(desc.geometries.size());
            for (const RHIAccelerationStructureGeometry& source : desc.geometries)
            {
                VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
                geometry.geometryType                                      = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                geometry.flags                                             = source.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
                VkAccelerationStructureGeometryTrianglesDataKHR& triangles = geometry.geometry.triangles;
                triangles.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                triangles.vertexFormat = static_cast<VkFormat>(source.vertexFormat);
                triangles.vertexStride = source.vertexStride;
                triangles.maxVertex    = source.vertexCount - 1;
                triangles.indexType = source.indexFormat == DataFormat::eR16UInt ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
                if (addresses)
                {
                    triangles.vertexData.deviceAddress = source.pVertexBuffer->GetDeviceAddress() + source.vertexOffset;
                    triangles.indexData.deviceAddress  = source.pIndexBuffer->GetDeviceAddress() + source.indexOffset;
                }
                if (source.pOpacityMicromap != nullptr)
                {
                    VkAccelerationStructureTrianglesOpacityMicromapKHR mapping{
                        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_KHR};
                    mapping.micromap    = static_cast<VulkanAccelerationStructure*>(source.pOpacityMicromap)->GetVkHandle();
                    mapping.indexType   = VK_INDEX_TYPE_UINT32;
                    mapping.indexStride = sizeof(uint32_t);
                    if (addresses)
                    {
                        mapping.indexBuffer = source.pOpacityIndexBuffer->GetDeviceAddress() + source.opacityIndexOffset;
                    }
                    opacity.push_back(mapping);
                    triangles.pNext = &opacity.back();
                }
                geometries.push_back(geometry);
                counts.push_back(source.indexCount / 3);
                ranges.push_back({source.indexCount / 3, 0, 0, 0});
            }
        }
        info.geometryCount = static_cast<uint32_t>(geometries.size());
        info.pGeometries   = geometries.data();
    }
};
} // namespace

RHIAccelerationStructureBuildSizes VulkanRHI::GetAccelerationStructureBuildSizes(const RHIAccelerationStructureBuildDesc& desc)
{
    return AreSubmissionsBlocked() ? RHIAccelerationStructureBuildSizes{} : VulkanAccelerationStructure::GetBuildSizes(desc);
}

RHIAccelerationStructure* VulkanRHI::CreateAccelerationStructure(const RHIAccelerationStructureCreateInfo& info)
{
    return GetResourceFactory()->CreateAccelerationStructure(info);
}

RHIAccelerationStructure* VulkanResourceFactory::CreateAccelerationStructure(const RHIAccelerationStructureCreateInfo& info)
{
    return GVulkanRHI->AreSubmissionsBlocked() ? nullptr : VulkanAccelerationStructure::CreateObject(info);
}

bool VulkanAccelerationStructure::ValidateDescription(const RHIAccelerationStructureBuildDesc& desc, bool requireBuffers)
{
    const RHIRayQueryCapabilities& capabilities = GVulkanRHI->QueryGPUInfo().rayQuery;
    bool                           valid        = capabilities.IsUsable() && capabilities.bufferDeviceAddress;
    if (desc.type == RHIAccelerationStructureType::eTopLevel)
    {
        valid &= desc.geometries.empty() && desc.instances.size() <= capabilities.maxInstances
              && desc.instances.size() <= UINT32_MAX;
        if (requireBuffers)
        {
            for (const RHIAccelerationStructureInstance& instance : desc.instances)
            {
                const VulkanAccelerationStructure* structure =
                    dynamic_cast<const VulkanAccelerationStructure*>(instance.pStructure);
                valid &= instance.IsValid() && structure != nullptr && structure->GetDeviceAddress() != 0;
            }
        }
    }
    else if (desc.type == RHIAccelerationStructureType::eOpacityMicromap)
    {
        valid &= capabilities.opacityMicromap && !desc.allowUpdate && desc.geometries.empty() && desc.instances.empty()
              && !desc.micromap.usages.empty() && desc.micromap.usages.size() <= UINT32_MAX && desc.micromap.dataSize > 0;
        uint64_t triangles = 0;
        for (const RHIOpacityMicromapUsage& usage : desc.micromap.usages)
        {
            triangles += usage.count;
            valid     &= usage.count != 0 && usage.subdivisionLevel <= capabilities.maxOpacitySubdivisionLevel;
        }
        valid &= triangles > 0 && triangles <= UINT32_MAX && triangles <= capabilities.maxMicromapTriangles;
        if (requireBuffers && valid)
        {
            valid = ValidInput(desc.micromap.pDataBuffer, desc.micromap.dataOffset, desc.micromap.dataSize, 256)
                 && ValidInput(desc.micromap.pTriangleBuffer, desc.micromap.triangleOffset,
                               triangles * sizeof(VkMicromapTriangleKHR), 4);
        }
    }
    else
    {
        valid               &= desc.type == RHIAccelerationStructureType::eBottomLevel && !desc.geometries.empty()
                            && desc.geometries.size() <= capabilities.maxGeometries && desc.instances.empty();
        uint64_t primitives  = 0;
        for (const RHIAccelerationStructureGeometry& geometry : desc.geometries)
        {
            valid      &= geometry.vertexFormat == DataFormat::eR32G32B32SFloat && geometry.vertexCount > 0
                       && geometry.vertexStride >= 12 && geometry.vertexStride % 4 == 0 && geometry.indexCount % 3 == 0
                       && (geometry.indexFormat == DataFormat::eR16UInt || geometry.indexFormat == DataFormat::eR32UInt);
            primitives += geometry.indexCount / 3;
            if (geometry.pOpacityMicromap != nullptr)
            {
                const VulkanAccelerationStructure* micromap =
                    dynamic_cast<const VulkanAccelerationStructure*>(geometry.pOpacityMicromap);
                valid &= capabilities.opacityMicromap && micromap != nullptr
                      && micromap->GetType() == RHIAccelerationStructureType::eOpacityMicromap;
                if (requireBuffers && valid)
                {
                    valid = ValidInput(geometry.pOpacityIndexBuffer, geometry.opacityIndexOffset,
                                       uint64_t(geometry.indexCount / 3) * sizeof(uint32_t), 4);
                }
            }
            else
            {
                valid &= geometry.pOpacityIndexBuffer == nullptr && geometry.opacityIndexOffset == 0;
            }
            if (requireBuffers && valid)
            {
                const uint32_t indexSize  = geometry.indexFormat == DataFormat::eR16UInt ? 2 : 4;
                valid                    &= ValidInput(geometry.pVertexBuffer, geometry.vertexOffset,
                                                       uint64_t(geometry.vertexCount - 1) * geometry.vertexStride + 12, 4)
                                         && ValidInput(geometry.pIndexBuffer, geometry.indexOffset, uint64_t(geometry.indexCount) * indexSize,
                                                       indexSize);
            }
        }
        valid &= primitives <= capabilities.maxPrimitives;
    }
    return valid;
}

RHIAccelerationStructureBuildSizes VulkanAccelerationStructure::GetBuildSizes(const RHIAccelerationStructureBuildDesc& desc)
{
    RHIAccelerationStructureBuildSizes result;
    if (ValidateDescription(desc, false))
    {
        NativeBuild                              build(desc, false);
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(
            GVulkanRHI->GetVkDevice(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build.info,
            desc.type == RHIAccelerationStructureType::eOpacityMicromap ? nullptr : build.counts.data(), &sizes);
        result = {sizes.accelerationStructureSize, sizes.buildScratchSize, sizes.updateScratchSize};
        if (desc.type == RHIAccelerationStructureType::eTopLevel)
        {
            result.instanceDataSize = std::max(size_t(1), desc.instances.size()) * sizeof(VkAccelerationStructureInstanceKHR);
        }
    }
    return result;
}

VulkanAccelerationStructure* VulkanAccelerationStructure::CreateObject(const RHIAccelerationStructureCreateInfo& info)
{
    VulkanAccelerationStructure* result = nullptr;
    bool                         valid =
        GVulkanRHI->QueryGPUInfo().rayQuery.IsUsable() && info.size > 0
        && (info.type == RHIAccelerationStructureType::eBottomLevel || info.type == RHIAccelerationStructureType::eTopLevel
            || (info.type == RHIAccelerationStructureType::eOpacityMicromap
                && GVulkanRHI->QueryGPUInfo().rayQuery.opacityMicromap));
    for (RHIAccelerationStructure* dependency : info.referencedStructures)
    {
        const VulkanAccelerationStructure* native  = dynamic_cast<const VulkanAccelerationStructure*>(dependency);
        valid                                     &= dependency != nullptr && native != nullptr
                                                  && ((info.type == RHIAccelerationStructureType::eTopLevel
                                                       && dependency->GetType() == RHIAccelerationStructureType::eBottomLevel && native->GetDeviceAddress() != 0)
                                                      || (info.type == RHIAccelerationStructureType::eBottomLevel
                                                          && dependency->GetType() == RHIAccelerationStructureType::eOpacityMicromap));
    }
    if (valid)
    {
        result = VersatileResource::AllocMem<VulkanAccelerationStructure>(GVulkanRHI->GetResourceAllocator());
        new (result) VulkanAccelerationStructure(info);
        result->Init();
        if (result->m_vkHandle == VK_NULL_HANDLE || result->m_deviceAddress == 0)
        {
            result->ReleaseReference();
            result = nullptr;
        }
    }
    return result;
}

VulkanAccelerationStructure::VulkanAccelerationStructure(const RHIAccelerationStructureCreateInfo& info) :
    RHIAccelerationStructure(info), m_sortedReferences(GetReferencedStructures())
{
    std::sort(m_sortedReferences.begin(), m_sortedReferences.end(), std::less<RHIAccelerationStructure*>());
}

void VulkanAccelerationStructure::Init()
{
    RHIBufferCreateInfo storage;
    storage.size         = GetSize();
    storage.allocateType = RHIBufferAllocateType::eGPU;
    storage.usageFlags.SetFlags(RHIBufferUsageFlagBits::eAccelerationStructureStorage, RHIBufferUsageFlagBits::eDeviceAddress);
    storage.tag      = m_resourceTag;
    m_pStorageBuffer = GVulkanRHI->CreateBuffer(storage);
    if (m_pStorageBuffer != nullptr)
    {
        VkAccelerationStructureCreateInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        info.buffer     = static_cast<VulkanBuffer*>(m_pStorageBuffer)->GetVkBuffer();
        info.size       = GetSize();
        info.type       = static_cast<VkAccelerationStructureTypeKHR>(GetType());
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        if (GetType() == RHIAccelerationStructureType::eOpacityMicromap)
        {
            VkAccelerationStructureCreateInfo2KHR micromap{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_2_KHR};
            micromap.type         = VK_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_KHR;
            micromap.addressRange = {m_pStorageBuffer->GetDeviceAddress(), GetSize()};
            result = vkCreateAccelerationStructure2KHR(GVulkanRHI->GetVkDevice(), &micromap, nullptr, &m_vkHandle);
        }
        else
        {
            result = vkCreateAccelerationStructureKHR(GVulkanRHI->GetVkDevice(), &info, nullptr, &m_vkHandle);
        }
        if (result == VK_SUCCESS)
        {
            VkAccelerationStructureDeviceAddressInfoKHR address{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
            address.accelerationStructure = m_vkHandle;
            m_deviceAddress               = GetType() == RHIAccelerationStructureType::eOpacityMicromap
                                              ? m_pStorageBuffer->GetDeviceAddress()
                                              : vkGetAccelerationStructureDeviceAddressKHR(GVulkanRHI->GetVkDevice(), &address);
        }
        else
        {
            m_vkHandle            = VK_NULL_HANDLE;
            const char* operation = GetType() == RHIAccelerationStructureType::eOpacityMicromap
                                      ? "vkCreateAccelerationStructure2KHR"
                                      : "vkCreateAccelerationStructureKHR";
            ReportVulkanDeviceLoss(result, operation);
            LOGE("{} failed: {}", operation, GetResultString(result));
        }
    }
}

void VulkanAccelerationStructure::Destroy()
{
    if (m_vkHandle != VK_NULL_HANDLE)
    {
        vkDestroyAccelerationStructureKHR(GVulkanRHI->GetVkDevice(), m_vkHandle, nullptr);
    }
    if (m_pStorageBuffer != nullptr)
    {
        m_pStorageBuffer->ReleaseReference();
    }
    this->~VulkanAccelerationStructure();
    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

bool VulkanAccelerationStructure::IsUpdateCompatible(const RHIAccelerationStructureBuildDesc& desc) const
{
    bool compatible = m_buildRecorded && m_allowUpdate && desc.allowUpdate && GetType() == desc.type
                   && m_instanceCount == desc.instances.size() && m_geometries.size() == desc.geometries.size();
    for (size_t i = 0; compatible && i < m_geometries.size(); ++i)
    {
        const RHIAccelerationStructureGeometry& prior = m_geometries[i];
        const RHIAccelerationStructureGeometry& next  = desc.geometries[i];
        compatible &=
            prior.pOpacityMicromap == next.pOpacityMicromap && prior.pOpacityIndexBuffer == next.pOpacityIndexBuffer
            && (next.pOpacityIndexBuffer == nullptr || m_opacityIndexIdentities[i] == next.pOpacityIndexBuffer->GetStableId())
            && prior.opacityIndexOffset == next.opacityIndexOffset && prior.vertexFormat == next.vertexFormat
            && prior.vertexCount == next.vertexCount && prior.indexFormat == next.indexFormat
            && prior.indexCount == next.indexCount && prior.opaque == next.opaque && prior.indexOffset == next.indexOffset
            && next.pIndexBuffer != nullptr && m_indexIdentities[i] == next.pIndexBuffer->GetStableId();
    }
    return compatible;
}

bool VulkanAccelerationStructure::ValidateBuild(const RHIAccelerationStructureBuildInfo& info) const
{
    const bool topLevel = GetType() == RHIAccelerationStructureType::eTopLevel;
    bool       valid    = info.pDestination == this && info.description.type == GetType()
                       && ValidateDescription(info.description, !topLevel) && info.pScratchBuffer != nullptr
                       && info.pScratchBuffer->GetUsageFlags().HasFlag(RHIBufferUsageFlagBits::eStorageBuffer);
    if (valid)
    {
        RHIAccelerationStructureBuildSizes sizes;
        if (topLevel && m_cachedTLASSizes.IsValid() && m_cachedInstanceCount == info.description.instances.size()
            && m_cachedAllowUpdate == info.description.allowUpdate)
        {
            sizes = m_cachedTLASSizes;
        }
        else
        {
            sizes = GetBuildSizes(info.description);
            if (topLevel)
            {
                // Native TLAS size queries depend on instance count and build flags, not instance contents.
                m_cachedTLASSizes     = sizes;
                m_cachedInstanceCount = info.description.instances.size();
                m_cachedAllowUpdate   = info.description.allowUpdate;
            }
        }
        const uint64_t scratchSize = info.pSource == nullptr ? sizes.buildScratchSize : sizes.updateScratchSize;
        const uint64_t address     = info.pScratchBuffer->GetDeviceAddress();
        valid                      = sizes.IsValid() && sizes.storageSize <= GetSize() && address != 0
                                  && info.scratchOffset <= info.pScratchBuffer->GetRequiredSize()
                                  && scratchSize <= info.pScratchBuffer->GetRequiredSize() - info.scratchOffset
                                  && (address + info.scratchOffset) % GVulkanRHI->QueryGPUInfo().rayQuery.scratchAlignment == 0;
        if (info.pSource != nullptr)
        {
            const VulkanAccelerationStructure* source  = dynamic_cast<const VulkanAccelerationStructure*>(info.pSource);
            valid                                     &= source != nullptr && source->IsUpdateCompatible(info.description);
        }
        for (const RHIAccelerationStructureInstance& instance : info.description.instances)
        {
            // These immutable dependencies were already validated as native BLASes at creation.
            // Check membership before dereferencing an input pointer.
            valid &= std::binary_search(m_sortedReferences.begin(), m_sortedReferences.end(), instance.pStructure,
                                        std::less<RHIAccelerationStructure*>())
                  && instance.IsValid();
        }
        for (const RHIAccelerationStructureGeometry& geometry : info.description.geometries)
        {
            if (geometry.pOpacityMicromap != nullptr)
            {
                valid &= std::binary_search(m_sortedReferences.begin(), m_sortedReferences.end(), geometry.pOpacityMicromap,
                                            std::less<RHIAccelerationStructure*>());
            }
        }
    }
    return valid;
}

VkAccelerationStructureInstanceKHR VulkanAccelerationStructure::EncodeInstance(const RHIAccelerationStructureInstance& instance,
                                                                               uint64_t                                address)
{
    VkAccelerationStructureInstanceKHR native{};
    std::memcpy(native.transform.matrix, instance.transform, sizeof(native.transform.matrix));
    native.instanceCustomIndex            = instance.customIndex;
    native.mask                           = instance.mask;
    native.flags                          = uint32_t(instance.flags);
    native.accelerationStructureReference = address;
    return native;
}

void VulkanAccelerationStructure::RecordBuild(VkCommandBuffer                          commandBuffer,
                                              const RHIAccelerationStructureBuildInfo& info,
                                              uint64_t                                 instanceAddress)
{
    NativeBuild build(info.description, true, instanceAddress);
    build.info.dstAccelerationStructure  = m_vkHandle;
    build.info.scratchData.deviceAddress = info.pScratchBuffer->GetDeviceAddress() + info.scratchOffset;
    if (info.pSource != nullptr)
    {
        build.info.mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        build.info.srcAccelerationStructure = static_cast<VulkanAccelerationStructure*>(info.pSource)->GetVkHandle();
    }
    const VkAccelerationStructureBuildRangeInfoKHR* ranges =
        GetType() == RHIAccelerationStructureType::eOpacityMicromap ? nullptr : build.ranges.data();
    vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &build.info, &ranges);
    m_buildRecorded = true;
    m_allowUpdate   = info.description.allowUpdate;
    m_instanceCount = static_cast<uint32_t>(info.description.instances.size());
    m_geometries.resize(info.description.geometries.size());
    std::ranges::copy(info.description.geometries, m_geometries.begin());
    m_indexIdentities.clear();
    m_opacityIndexIdentities.clear();
    for (const RHIAccelerationStructureGeometry& geometry : info.description.geometries)
    {
        m_indexIdentities.push_back(geometry.pIndexBuffer->GetStableId());
        m_opacityIndexIdentities.push_back(geometry.pOpacityIndexBuffer != nullptr ? geometry.pOpacityIndexBuffer->GetStableId()
                                                                                   : 0);
    }
}

void FVulkanCommandListContext::RHIBuildAccelerationStructure(const RHIAccelerationStructureBuildInfo& info)
{
    VulkanAccelerationStructure* destination = dynamic_cast<VulkanAccelerationStructure*>(info.pDestination);
    if (destination == nullptr || GetContextType() != RHICommandContextType::eGraphics || !destination->ValidateBuild(info))
    {
        LatchError(MakeRHIError(RHIErrorCode::eInvalidArgument, "BuildAccelerationStructure", __FILE__, __LINE__));
    }
    else if (EnsureRecording())
    {
        RHIResourcePtr<RHIBuffer> instances;
        if (info.description.type == RHIAccelerationStructureType::eTopLevel)
        {
            RHIBufferCreateInfo upload;
            upload.size = std::max(size_t(1), info.description.instances.size()) * sizeof(VkAccelerationStructureInstanceKHR);
            upload.allocateType = RHIBufferAllocateType::eCPUWriteGPURead;
            upload.usageFlags.SetFlags(RHIBufferUsageFlagBits::eAccelerationStructureInput,
                                       RHIBufferUsageFlagBits::eDeviceAddress);
            upload.tag = "AS instance upload";
            instances  = RHIResourcePtr<RHIBuffer>::Adopt(GVulkanRHI->CreateBuffer(upload));
            VkAccelerationStructureInstanceKHR* mapped =
                instances ? reinterpret_cast<VkAccelerationStructureInstanceKHR*>(instances->Map()) : nullptr;
            if (mapped == nullptr || instances->GetDeviceAddress() == 0 || instances->GetDeviceAddress() % 16 != 0)
            {
                LatchError(MakeRHIError(RHIErrorCode::eBackendFailure, "Vulkan AS instance upload", __FILE__, __LINE__));
            }
            else
            {
                std::memset(mapped, 0, upload.size);
                for (size_t i = 0; i < info.description.instances.size(); ++i)
                {
                    const RHIAccelerationStructureInstance& instance = info.description.instances[i];
                    mapped[i]                                        = VulkanAccelerationStructure::EncodeInstance(
                        instance, static_cast<const VulkanAccelerationStructure*>(instance.pStructure)->GetDeviceAddress());
                }
                instances->Unmap();
                RecordResource(instances.Get());
            }
        }
        if (!HasRecordingError())
        {
            RecordResource(destination);
            RecordResource(info.pSource);
            RecordResource(info.pScratchBuffer);
            RecordResource(info.description.micromap.pDataBuffer);
            RecordResource(info.description.micromap.pTriangleBuffer);
            for (const RHIAccelerationStructureGeometry& geometry : info.description.geometries)
            {
                RecordResource(geometry.pVertexBuffer);
                RecordResource(geometry.pIndexBuffer);
                RecordResource(geometry.pOpacityIndexBuffer);
                RecordResource(geometry.pOpacityMicromap);
            }
            destination->RecordBuild(GetCommandBuffer()->GetVkHandle(), info, instances ? instances->GetDeviceAddress() : 0);
        }
    }
}
} // namespace zen
