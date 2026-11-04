#pragma once
#include "Graphics/RHI/RHIAccelerationStructure.h"
#include "VulkanHeaders.h"

namespace zen
{
class VulkanAccelerationStructure : public RHIAccelerationStructure
{
public:
    static VulkanAccelerationStructure* CreateObject(const RHIAccelerationStructureCreateInfo& info);

    static RHIAccelerationStructureBuildSizes GetBuildSizes(const RHIAccelerationStructureBuildDesc& desc);

    static bool ValidateDescription(const RHIAccelerationStructureBuildDesc& desc, bool requireBuffers);

    bool ValidateBuild(const RHIAccelerationStructureBuildInfo& info) const;

    static VkAccelerationStructureInstanceKHR EncodeInstance(const RHIAccelerationStructureInstance& instance,
                                                             uint64_t                                address);

    void RecordBuild(VkCommandBuffer commandBuffer, const RHIAccelerationStructureBuildInfo& info, uint64_t instanceAddress);

    uint64_t GetDeviceAddress() const
    {
        return m_deviceAddress;
    }

    VkAccelerationStructureKHR GetVkHandle() const
    {
        return m_vkHandle;
    }

    bool IsAsyncComputeAccessible() const override
    {
        return m_pStorageBuffer != nullptr && m_pStorageBuffer->IsAsyncComputeAccessible();
    }

protected:
    void Init() override;

    void Destroy() override;

private:
    explicit VulkanAccelerationStructure(const RHIAccelerationStructureCreateInfo& info) : RHIAccelerationStructure(info) {}

    bool IsUpdateCompatible(const RHIAccelerationStructureBuildDesc& desc) const;

    VkAccelerationStructureKHR m_vkHandle{VK_NULL_HANDLE};
    RHIBuffer*                 m_pStorageBuffer{nullptr};
    uint64_t                   m_deviceAddress{0};
    // Recording metadata only. Submission success and generation publication belong
    // to the caller; discarded builds must never be reused as update sources.
    bool                                         m_buildRecorded{false};
    bool                                         m_allowUpdate{false};
    uint32_t                                     m_instanceCount{0};
    HeapVector<RHIAccelerationStructureGeometry> m_geometries;
    HeapVector<uint64_t>                         m_indexIdentities;
};
} // namespace zen
