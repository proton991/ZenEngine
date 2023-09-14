#include <iostream>
#include "Common/SharedPtr.h"
#include "Common/UniquePtr.h"
#include "Graphics/Val/ZenVal.h"
#include "Platform/GlfwWindow.h"
#include "Common/Helpers.h"

//using namespace zen::val;
//using namespace zen::platform;
using namespace zen;
int main(int argc, char** argv)
{
    platform::WindowConfig windowConfig;
    auto*                  window = new platform::GlfwWindowImpl(windowConfig);

    auto instanceExts = window->GetInstanceExtensions();
    auto deviceExts   = window->GetDeviceExtensions();

    val::Instance::CreateInfo instanceCI{};
    instanceCI.enabledExtensionCount   = ToU32(instanceExts.size());
    instanceCI.ppEnabledExtensionNames = instanceExts.data();

    auto valInstance = val::Instance::Create(instanceCI);

    VkSurfaceKHR surface = window->CreateSurface(valInstance->GetHandle());

    auto valPhysicalDevice = val::PhysicalDevice::Create(*valInstance);

    val::Device::CreateInfo deviceCI{};
    deviceCI.pPhysicalDevice         = valPhysicalDevice.Get();
    deviceCI.enabledExtensionCount   = ToU32(deviceExts.size());
    deviceCI.ppEnabledExtensionNames = deviceExts.data();

    auto valDevice = val::Device::Create(deviceCI);

    val::RuntimeArraySizes runtimeArraySizes{{"textures", 4}};

    //    val::ShaderModule testShader{
    //        *valDevice, VK_SHADER_STAGE_FRAGMENT_BIT, "gbuffer.frag.spv", runtimeArraySizes};
    //    std::cout << "entry point: " << testShader.GetEntryPoint() << std::endl;
    //    auto& resources = testShader.GetResources();
    //    for (auto& resource : resources)
    //    {
    //        std::cout << resource.name << ":" << std::endl;
    //        std::cout << "\tset: " << resource.set << std::endl;
    //        std::cout << "\tbinding: " << resource.binding << std::endl;
    //        std::cout << "\tlocation: " << resource.location << std::endl;
    //        std::cout << "\tarraySize: " << resource.arraySize << std::endl;
    //        std::cout << "\tsize: " << resource.size << std::endl;
    //    }

    val::Swapchain swapChain{*valDevice, surface, {windowConfig.width, windowConfig.height}};

    while (!window->ShouldClose())
    {
        window->Update();
    }
    return 0;
}
