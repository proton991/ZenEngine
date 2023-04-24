#include <iostream>
#include "Common/SharedPtr.h"
#include "Common/UniquePtr.h"
#include "Graphics/Vulkan/ZenVulkan.h"
#include "Platform/GlfwWindow.h"

using namespace zen::vulkan;
using namespace zen::platform;
int main(int argc, char** argv) {
  WindowConfig windowConfig;
  GlfwWindowImpl* window = new GlfwWindowImpl(windowConfig);
  {
    UniquePtr<int> a = UniquePtr<int>(new int(2));
    auto b           = UniquePtr<int>(std::move(a));

    SharedPtr<int> c = MakeShared<int>(4);
    auto d           = SharedPtr(std::move(c));

    int f = 1;
  }

  Context context{};
  auto instanceExts = window->GetInstanceExtensions();
  auto deviceExts   = window->GetDeviceExtensions();
  context.SetupInstance(instanceExts.data(), instanceExts.size(), nullptr, 0);
  vk::SurfaceKHR surface = window->CreateSurface(context.GetInstance());
  context.SetupDevice(deviceExts.data(), deviceExts.size(), surface);
  SwapChain::Desc swapChainDesc{windowConfig.width, windowConfig.height, 2, true};
  SwapChain swapChain{context, surface, swapChainDesc};
  while (!window->ShouldClose()) {
    window->Update();
  }
}