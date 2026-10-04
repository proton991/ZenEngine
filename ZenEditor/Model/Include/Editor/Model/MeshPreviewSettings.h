#pragma once
#include <cstdint>

namespace zen::editor
{
// Shading of the material-free mesh preview, as in RenderDoc's mesh view.
enum class MeshPreviewShading : uint32_t
{
    // Face normals from screen-space derivatives; shows the actual triangles.
    Flat,
    // Interpolated vertex normals.
    Smooth,
    // Vertex normals as colors, in the mesh's space.
    Normals
};

struct MeshPreviewSettings
{
    MeshPreviewShading shading{MeshPreviewShading::Flat};
    // Triangle edges drawn over the shading; requires non-solid fill support.
    bool wireframe{false};
};
} // namespace zen::editor
