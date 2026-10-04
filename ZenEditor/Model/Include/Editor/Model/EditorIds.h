#pragma once
#include <cstdint>

namespace zen::editor
{
// Session-local identities; scene replacement advances the generation.
struct NodeId
{
    uint64_t generation{0};
    uint32_t index{UINT32_MAX};

    bool operator==(const NodeId&) const = default;
};

enum class SceneAssetKind : uint32_t
{
    Mesh,
    Material,
    Texture,
    Animation
};

struct SceneAssetId
{
    uint64_t       generation{0};
    SceneAssetKind kind{SceneAssetKind::Mesh};
    uint32_t       index{UINT32_MAX};

    bool operator==(const SceneAssetId&) const = default;
};
} // namespace zen::editor
