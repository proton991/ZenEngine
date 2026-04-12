#include <iostream>
#include "Utils/SharedPtr.h"
#include "Utils/UniquePtr.h"
#include "Graphics/Val/ZenVal.h"
#include "Platform/GlfwWindow.h"
#include "Utils/Helpers.h"
#include "Graphics/RenderCore/RenderDevice.h"
#include "Graphics/Val/VulkanStrings.h"

using namespace zen;

int main(int argc, char** pArgv)
{
    platform::WindowConfig windowConfig;
    auto* pWindow = new platform::GlfwWindowImpl(windowConfig);

    auto instanceExts = pWindow->GetInstanceExtensions();
    auto deviceExts   = pWindow->GetDeviceExtensions();

    val::Instance::CreateInfo instanceCI{};
    instanceCI.enabledExtensionCount   = util::ToU32(instanceExts.size());
    instanceCI.pEnabledExtensionNames = instanceExts.data();

    auto valInstance = val::Instance::Create(instanceCI);

    VkSurfaceKHR surface = pWindow->CreateSurface(valInstance->GetHandle());

    auto valPhysicalDevice = val::PhysicalDevice::CreateUnique(*valInstance);

    val::Device::CreateInfo deviceCI{};
    deviceCI.pPhysicalDevice         = valPhysicalDevice.Get();
    deviceCI.enabledExtensionCount   = util::ToU32(deviceExts.size());
    deviceCI.pEnabledExtensionNames = deviceExts.data();

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
    imageCI.usage    = val::ImageUsage::Sampled;
    imageCI.format   = VK_FORMAT_B8G8R8A8_SRGB;
    imageCI.vmaFlags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    auto dummyImage       = val::Image::Create(*valDevice, imageCI);
    auto dummyImageUnique = val::Image::CreateUnique(*valDevice, imageCI);

    val::BufferCreateInfo bufferCI{};
    bufferCI.vmaFlags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    bufferCI.usage    = val::BufferUsage::VertexBuffer;
    bufferCI.byteSize = 1024;

    auto dummyBuffer       = val::Buffer::Create(*valDevice, bufferCI);
    auto dummyBufferUnique = val::Buffer::CreateUnique(*valDevice, bufferCI);

    while (!pWindow->ShouldClose())
    {
        pWindow->Update();
    }
    return 0;
}
