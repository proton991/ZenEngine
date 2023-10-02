#pragma once
#include <cstdint>
#include <string>

namespace zen
{
using Index = uint32_t;
using Tag   = std::string;
enum ImageSizeType
{
    Absolute,
    SwapchainRelative,
};

enum RDGQueueFlagBits
{
    RDG_QUEUE_GRAPHICS_BIT       = 1 << 0,
    RDG_QUEUE_COMPUTE_BIT        = 1 << 1,
    RDG_QUEUE_ASYNC_COMPUTE_BIT  = 1 << 2,
    RDG_QUEUE_ASYNC_GRAPHICS_BIT = 1 << 3
};
using RDGQueueFlags = uint32_t;
} // namespace zen