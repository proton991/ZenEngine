#include "Graphics/VulkanRHI/VulkanAccelerationStructure.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanResourceAllocator.h"

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

    explicit NativeBuild(const RHIAccelerationStructureBuildDesc& desc, bool addresses)
    {
        info.type  = static_cast<VkAccelerationStructureTypeKHR>(desc.type);
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                   | (desc.allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        if (desc.type == RHIAccelerationStructureType::eTopLevel)
        {
            VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geometry.geometryType             = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
            geometry.geometry.instances.data.deviceAddress =
                addresses ? desc.pInstanceBuffer->GetDeviceAddress() + desc.instanceOffset : 0;
            geometries.push_back(geometry);
            counts.push_back(desc.instanceCount);
            ranges.push_back({desc.instanceCount, 0, 0, 0});
        }
        else
        {
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
    bool                           valid        = capabilities.IsUsable();
    if (desc.type == RHIAccelerationStructureType::eTopLevel)
    {
        valid &= desc.geometries.empty() && desc.instanceCount <= capabilities.maxInstances;
        if (requireBuffers)
        {
            valid &= ValidInput(desc.pInstanceBuffer, desc.instanceOffset, uint64_t(desc.instanceCount) * 64, 16);
        }
    }
    else
    {
        valid &= desc.type == RHIAccelerationStructureType::eBottomLevel && !desc.geometries.empty()
              && desc.geometries.size() <= capabilities.maxGeometries && desc.instanceCount == 0;
        uint64_t primitives = 0;
        for (const RHIAccelerationStructureGeometry& geometry : desc.geometries)
        {
            valid &= geometry.vertexFormat == DataFormat::eR32G32B32SFloat && geometry.vertexCount > 0
                  && geometry.vertexStride >= 12 && geometry.vertexStride % 4 == 0 && geometry.indexCount % 3 == 0
                  && (geometry.indexFormat == DataFormat::eR16UInt || geometry.indexFormat == DataFormat::eR32UInt);
            primitives += geometry.indexCount / 3;
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
        vkGetAccelerationStructureBuildSizesKHR(GVulkanRHI->GetVkDevice(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                &build.info, build.counts.data(), &sizes);
        result = {sizes.accelerationStructureSize, sizes.buildScratchSize, sizes.updateScratchSize};
    }
    return result;
}

VulkanAccelerationStructure* VulkanAccelerationStructure::CreateObject(const RHIAccelerationStructureCreateInfo& info)
{
    VulkanAccelerationStructure* result = nullptr;
    bool                         valid =
        GVulkanRHI->QueryGPUInfo().rayQuery.IsUsable() && info.size > 0
        && (info.type == RHIAccelerationStructureType::eBottomLevel || info.type == RHIAccelerationStructureType::eTopLevel);
    for (RHIAccelerationStructure* dependency : info.referencedStructures)
    {
        valid &= info.type == RHIAccelerationStructureType::eTopLevel && dependency != nullptr
              && dependency->GetType() == RHIAccelerationStructureType::eBottomLevel && dependency->GetDeviceAddress() != 0;
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

void VulkanAccelerationStructure::Init()
{
    RHIBufferCreateInfo storage;
    storage.size         = m_size;
    storage.allocateType = RHIBufferAllocateType::eGPU;
    storage.usageFlags.SetFlags(RHIBufferUsageFlagBits::eAccelerationStructureStorage, RHIBufferUsageFlagBits::eDeviceAddress);
    storage.tag      = m_resourceTag;
    m_pStorageBuffer = GVulkanRHI->CreateBuffer(storage);
    if (m_pStorageBuffer != nullptr)
    {
        VkAccelerationStructureCreateInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        info.buffer           = static_cast<VulkanBuffer*>(m_pStorageBuffer)->GetVkBuffer();
        info.size             = m_size;
        info.type             = static_cast<VkAccelerationStructureTypeKHR>(GetType());
        const VkResult result = vkCreateAccelerationStructureKHR(GVulkanRHI->GetVkDevice(), &info, nullptr, &m_vkHandle);
        if (result == VK_SUCCESS)
        {
            VkAccelerationStructureDeviceAddressInfoKHR address{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
            address.accelerationStructure = m_vkHandle;
            m_deviceAddress               = vkGetAccelerationStructureDeviceAddressKHR(GVulkanRHI->GetVkDevice(), &address);
        }
        else
        {
            m_vkHandle = VK_NULL_HANDLE;
            ReportVulkanDeviceLoss(result, "vkCreateAccelerationStructureKHR");
            LOGE("vkCreateAccelerationStructureKHR failed: {}", GetResultString(result));
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
                   && m_instanceCount == desc.instanceCount && m_geometries.size() == desc.geometries.size();
    for (size_t i = 0; compatible && i < m_geometries.size(); ++i)
    {
        const RHIAccelerationStructureGeometry& prior = m_geometries[i];
        const RHIAccelerationStructureGeometry& next  = desc.geometries[i];
        compatible &= prior.vertexFormat == next.vertexFormat && prior.vertexCount == next.vertexCount
                   && prior.indexFormat == next.indexFormat && prior.indexCount == next.indexCount
                   && prior.opaque == next.opaque && prior.indexOffset == next.indexOffset && next.pIndexBuffer != nullptr
                   && m_indexIdentities[i] == next.pIndexBuffer->GetStableId();
    }
    return compatible;
}

bool VulkanAccelerationStructure::ValidateBuild(const RHIAccelerationStructureBuildInfo& info) const
{
    bool valid = info.pDestination == this && info.description.type == GetType() && ValidateDescription(info.description, true)
              && info.pScratchBuffer != nullptr
              && info.pScratchBuffer->GetUsageFlags().HasFlag(RHIBufferUsageFlagBits::eStorageBuffer);
    if (valid)
    {
        const RHIAccelerationStructureBuildSizes sizes = GetBuildSizes(info.description);
        const uint64_t scratchSize = info.pSource == nullptr ? sizes.buildScratchSize : sizes.updateScratchSize;
        const uint64_t address     = info.pScratchBuffer->GetDeviceAddress();
        valid                      = sizes.IsValid() && sizes.storageSize <= m_size && address != 0
             && info.scratchOffset <= info.pScratchBuffer->GetRequiredSize()
             && scratchSize <= info.pScratchBuffer->GetRequiredSize() - info.scratchOffset
             && (address + info.scratchOffset) % GVulkanRHI->QueryGPUInfo().rayQuery.scratchAlignment == 0;
        if (info.pSource != nullptr)
        {
            valid &= static_cast<const VulkanAccelerationStructure*>(info.pSource)->IsUpdateCompatible(info.description);
        }
    }
    return valid;
}

void VulkanAccelerationStructure::RecordBuild(VkCommandBuffer commandBuffer, const RHIAccelerationStructureBuildInfo& info)
{
    NativeBuild build(info.description, true);
    build.info.dstAccelerationStructure  = m_vkHandle;
    build.info.scratchData.deviceAddress = info.pScratchBuffer->GetDeviceAddress() + info.scratchOffset;
    if (info.pSource != nullptr)
    {
        build.info.mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        build.info.srcAccelerationStructure = static_cast<VulkanAccelerationStructure*>(info.pSource)->GetVkHandle();
    }
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = build.ranges.data();
    vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &build.info, &ranges);
    m_buildRecorded = true;
    m_allowUpdate   = info.description.allowUpdate;
    m_instanceCount = info.description.instanceCount;
    m_geometries.resize(info.description.geometries.size());
    std::ranges::copy(info.description.geometries, m_geometries.begin());
    m_indexIdentities.clear();
    for (const RHIAccelerationStructureGeometry& geometry : info.description.geometries)
    {
        m_indexIdentities.push_back(geometry.pIndexBuffer->GetStableId());
    }
}

void FVulkanCommandListContext::RHIBuildAccelerationStructure(const RHIAccelerationStructureBuildInfo& info)
{
    VulkanAccelerationStructure* destination = static_cast<VulkanAccelerationStructure*>(info.pDestination);
    if (destination == nullptr || GetContextType() != RHICommandContextType::eGraphics || !destination->ValidateBuild(info))
    {
        LatchError(MakeRHIError(RHIErrorCode::eInvalidArgument, "BuildAccelerationStructure", __FILE__, __LINE__));
    }
    else if (EnsureRecording())
    {
        RecordResource(destination);
        RecordResource(info.pSource);
        RecordResource(info.pScratchBuffer);
        RecordResource(info.description.pInstanceBuffer);
        for (const RHIAccelerationStructureGeometry& geometry : info.description.geometries)
        {
            RecordResource(geometry.pVertexBuffer);
            RecordResource(geometry.pIndexBuffer);
        }
        destination->RecordBuild(GetCommandBuffer()->GetVkHandle(), info);
    }
}
} // namespace zen
