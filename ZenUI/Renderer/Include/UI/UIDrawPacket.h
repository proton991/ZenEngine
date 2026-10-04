#pragma once
#include "Templates/HeapVector.h"
#include <cstdint>

namespace zen::ui
{
// Logical positions and UVs; RGBA8 with red in the least significant byte.
// SDR encoded colors use straight-alpha blending. No toolkit vertex/index ABI.
struct UIVertex
{
    float    position[2];
    float    uv[2];
    uint32_t color;
};

// Opaque and local to the issuing UIRenderer; zero is invalid.
struct UITextureHandle
{
    uint64_t value{0};

    bool operator==(const UITextureHandle&) const = default;
};

struct UIDrawCommand
{
    UITextureHandle texture;
    uint32_t        minX{0};
    uint32_t        minY{0};
    uint32_t        maxX{0};
    uint32_t        maxY{0};
    uint32_t        count{0};
    uint32_t        firstIndex{0};
    uint32_t        vertexOffset{0};
};

// Owned geometry; clipping is in physical pixels. Projection maps logical positions to
// clip space as position * projection.xy + projection.zw.
struct UIDrawPacket
{
    HeapVector<UIVertex>      vertices;
    HeapVector<uint32_t>      indices;
    HeapVector<UIDrawCommand> commands;
    float                     projection[4]{};
};

bool ValidateUIDrawPacket(const UIDrawPacket& packet, uint32_t width, uint32_t height);
} // namespace zen::ui
