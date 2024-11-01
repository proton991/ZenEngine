#pragma once
#include <vulkan/vulkan.h>
#include "Common/ObjectBase.h"
#include <vector>
#include <string>

namespace zen::platform
{

struct WindowConfig
{
    std::string title{"ZenEngine"};
    bool resizable{false};
    uint32_t width{1280};
    uint32_t height{720};
    float aspect{0.0f};
};

class NativeWindow
{
public:
    NativeWindow() = default;
    ZEN_NO_COPY_MOVE(NativeWindow)
    virtual ~NativeWindow() = default;

    virtual VkSurfaceKHR CreateSurface(VkInstance instance) const = 0;

    virtual std::vector<const char*> GetInstanceExtensions() = 0;
    virtual std::vector<const char*> GetDeviceExtensions()
    {
        return {"VK_KHR_swapchain"};
    }

    virtual float GetAspect() = 0;
};
} // namespace zen::platform