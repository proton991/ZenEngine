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

    void RecordBuild(VkCommandBuffer commandBuffer, const RHIAccelerationStructureBuildInfo& info);

    uint64_t GetDeviceAddress() const override
    {
        return m_deviceAddress;
    }

    RHIBuffer* GetStorageBuffer() const override
    {
        return m_pStorageBuffer;
    }

    VkAccelerationStructureKHR GetVkHandle() const
    {
        return m_vkHandle;
    }

protected:
    void Init() override;

    void Destroy() override;

private:
    explicit VulkanAccelerationStructure(const RHIAccelerationStructureCreateInfo& info) :
        RHIAccelerationStructure(info), m_size(info.size)
    {}

    bool IsUpdateCompatible(const RHIAccelerationStructureBuildDesc& desc) const;

    VkAccelerationStructureKHR m_vkHandle{VK_NULL_HANDLE};
    RHIBuffer*                 m_pStorageBuffer{nullptr};
    uint64_t                   m_size{0};
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
