#include <iostream>
#include "Common/SharedPtr.h"
#include "Common/UniquePtr.h"
#include "Graphics/Val/ZenVal.h"
#include "Platform/GlfwWindow.h"
#include "Common/Helpers.h"
#include "Graphics/RenderCore/RenderDevice.h"
#include "Graphics/Val/VulkanStrings.h"

//using namespace zen::val;
//using namespace zen::platform;
using namespace zen;
int main(int argc, char** argv)
{
    platform::WindowConfig windowConfig;
    auto* window = new platform::GlfwWindowImpl(windowConfig);

    auto instanceExts = window->GetInstanceExtensions();
    auto deviceExts   = window->GetDeviceExtensions();

    val::Instance::CreateInfo instanceCI{};
    instanceCI.enabledExtensionCount   = util::ToU32(instanceExts.size());
    instanceCI.ppEnabledExtensionNames = instanceExts.data();

    auto valInstance = val::Instance::Create(instanceCI);

    VkSurfaceKHR surface = window->CreateSurface(valInstance->GetHandle());

    auto valPhysicalDevice = val::PhysicalDevice::CreateUnique(*valInstance);

    val::Device::CreateInfo deviceCI{};
    deviceCI.pPhysicalDevice         = valPhysicalDevice.Get();
    deviceCI.enabledExtensionCount   = util::ToU32(deviceExts.size());
    deviceCI.ppEnabledExtensionNames = deviceExts.data();

    auto valDevice = val::Device::Create(deviceCI);

    val::RuntimeArraySizes runtimeArraySizes{{"textures", 4}};
    val::RuntimeArraySizes runtimeArraySizes2{};

    val::ShaderModule testShader{*valDevice, VK_SHADER_STAGE_FRAGMENT_BIT, "gbuffer.frag.spv",
                                 runtimeArraySizes2};
    std::cout << "entry point: " << testShader.GetEntryPoint() << std::endl;
    auto& resources = testShader.GetResources();
    for (auto& resource : resources)
    {
        std::cout << resource.name << ":" << std::endl;
        std::cout << "\tset: " << resource.set << std::endl;
        std::cout << "\tbinding: " << resource.binding << std::endl;
        std::cout << "\tlocation: " << resource.location << std::endl;
        std::cout << "\tarraySize: " << resource.arraySize << std::endl;
        std::cout << "\tsize(in bytes): " << resource.size << std::endl;
        std::cout << "\tformat: " << val::VkToString(resource.format) << std::endl;
    }

    val::Swapchain swapChain{*valDevice, surface, {windowConfig.width, windowConfig.height}};

    RenderDevice renderDevice{*valDevice};
    val::ImageCreateInfo imageCI{};
    imageCI.extent3D = {windowConfig.width, windowConfig.height, 1};
    imageCI.usage    = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageCI.format   = VK_FORMAT_B8G8R8A8_SRGB;
    imageCI.vmaFlags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    auto dummyImage       = val::Image::Create(*valDevice, imageCI);
    auto dummyImageUnique = val::Image::CreateUnique(*valDevice, imageCI);

    val::BufferCreateInfo bufferCI{};
    bufferCI.vmaFlags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    bufferCI.usage    = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferCI.byteSize = 1024;

    auto dummyBuffer       = val::Buffer::Create(*valDevice, bufferCI);
    auto dummyBufferUnique = val::Buffer::CreateUnique(*valDevice, bufferCI);

    while (!window->ShouldClose())
    {
        window->Update();
    }
    return 0;
}
