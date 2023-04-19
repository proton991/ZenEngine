#include <iostream>
#include "Graphics/Vulkan/ZenVulkan.h"

using namespace zen::vulkan;
int main(int argc, char** argv) {
  std::cout << "ZenCore.lib linked!" << std::endl;
  Context context{};
  context.SetupInstance(nullptr, 0, nullptr, 0);
  context.SetupDevice(nullptr, 0, nullptr);
}