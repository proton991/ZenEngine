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

  UniquePtr<Device> device       = MakeUnique<Device>(context);
  auto graphicsQFIndex           = device->GetQueueFamliyIndex(QueueIndices::QUEUE_INDEX_GRAPHICS);
  UniquePtr<CommandPool> cmdPool = MakeUnique<CommandPool>(*device, graphicsQFIndex);

  cmdPool->SetDebugName("Graphics Command Pool");
  SwapChain::Desc swapChainDesc{windowConfig.width, windowConfig.height, 2, true};
  SwapChain swapChain{context, surface, swapChainDesc};

  Buffer stageBuffer(*device, 1024, vk::BufferUsageFlagBits::eTransferSrc,
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT, "test staging buffer");
  while (!window->ShouldClose()) {
    window->Update();
  }
}