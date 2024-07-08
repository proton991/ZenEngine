#pragma once
#include <utility>
#include <vulkan/vulkan.hpp>
#include "Common/ObjectBase.h"
#include "Common/Errors.h"
#include "Device.h"

namespace zen::val
{
template <class VkHandleType, VkObjectType objectType> class DeviceObject
{
public:
    explicit DeviceObject(const Device& device) : m_device(device), m_handle(VK_NULL_HANDLE) {}

    DeviceObject(const DeviceObject& other) :
        m_device(other.m_device), m_handle(other.m_handle), m_debugName(other.m_debugName)
    {}

    DeviceObject(DeviceObject&& other) noexcept :
        m_device(std::move(other.m_device)),
        m_handle(std::exchange(other.m_handle, {})),
        m_debugName(std::move(other.m_debugName))
    {
        SetObjectDebugName(m_debugName);
    }

    VkHandleType GetHandle() const
    {
        return m_handle;
    }

    void SetObjectDebugName(std::string name)
    {
        m_debugName = std::move(name);
        VkDebugUtilsObjectNameInfoEXT ObjectNameInfo{};
        ObjectNameInfo.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        ObjectNameInfo.pNext        = nullptr;
        ObjectNameInfo.objectType   = objectType;
        ObjectNameInfo.objectHandle = reinterpret_cast<uint64_t>(m_handle);
        ObjectNameInfo.pObjectName  = m_debugName.data();

        CHECK_VK_ERROR(vkSetDebugUtilsObjectNameEXT(m_device.GetHandle(), &ObjectNameInfo),
                       "Failed to set debug object name");
    }

    const Device& m_device;
    VkHandleType m_handle;
    std::string m_debugName{};
};
} // namespace zen::val