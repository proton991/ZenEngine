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
    RENDER_GRAPH_QUEUE_GRAPHICS_BIT       = 1 << 0,
    RENDER_GRAPH_QUEUE_COMPUTE_BIT        = 1 << 1,
    RENDER_GRAPH_QUEUE_ASYNC_COMPUTE_BIT  = 1 << 2,
    RENDER_GRAPH_QUEUE_ASYNC_GRAPHICS_BIT = 1 << 3
};
using RDGQueueFlags = uint32_t;
} // namespace zen