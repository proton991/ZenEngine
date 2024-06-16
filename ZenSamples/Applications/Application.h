#pragma once
#include "Platform/GlfwWindow.h"
#include "Common/UniquePtr.h"
#include "Graphics/Val/ZenVal.h"

namespace zen
{
class Application
{
public:
    Application() {}

    virtual ~Application() = default;

    virtual void Prepare(const platform::WindowConfig& windowConfig)
    {
        m_window          = MakeUnique<platform::GlfwWindowImpl>(windowConfig);
        auto instanceExts = m_window->GetInstanceExtensions();
        auto deviceExts   = m_window->GetDeviceExtensions();

        val::Instance::CreateInfo instanceCI{};
        instanceCI.enabledExtensionCount   = util::ToU32(instanceExts.size());
        instanceCI.ppEnabledExtensionNames = instanceExts.data();

        m_instance = val::Instance::CreateUnique(instanceCI);

        m_physicalDevice = val::PhysicalDevice::CreateUnique(*m_instance);
        val::Device::CreateInfo deviceCI{};
        deviceCI.pPhysicalDevice         = m_physicalDevice.Get();
        deviceCI.enabledExtensionCount   = util::ToU32(deviceExts.size());
        deviceCI.ppEnabledExtensionNames = deviceExts.data();

        m_device = val::Device::CreateUnique(deviceCI);
    }

    virtual void Update(float deltaTime)
    {
        m_fps       = 1.0f / deltaTime;
        m_frameTime = deltaTime * 1000.0f;
    }

    virtual void Run() = 0;

    virtual void Finish() {}

    virtual void SetupRenderGraph() = 0;

protected:
    float m_fps{0.0f};
    float m_frameTime{0.0f}; // In ms
    // Window
    UniquePtr<platform::GlfwWindowImpl> m_window{nullptr};
    // Vulkan
    UniquePtr<val::Instance> m_instance;
    UniquePtr<val::PhysicalDevice> m_physicalDevice;
    UniquePtr<val::Device> m_device;
};
} // namespace zen