#include "Graphics/VulkanRHI/VulkanRHI.h"

using namespace zen;


int main(int argc, char** argv)
{
    VulkanRHI vkRHI;
    vkRHI.Init();
    vkRHI.Destroy();
}