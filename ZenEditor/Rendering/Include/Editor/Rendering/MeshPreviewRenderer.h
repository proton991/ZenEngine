#pragma once
#include "Editor/Model/MeshPreviewSettings.h"
#include "Editor/Rendering/EditorViewport.h"

namespace zen::editor
{
// One preview frame: a mesh of the open scene and how to view it.
struct MeshPreviewFrame
{
    const sg::Mesh*     mesh{nullptr};
    Mat4                viewProjection{1.0f};
    Vec3                eye{0.0f};
    MeshPreviewSettings settings;
};

// Draws one mesh of the open scene without materials into its own image, in the mesh's
// vertex space, like RenderDoc's mesh view. It reuses the viewport's scene vertex and
// index buffers, draws triangle primitives only, and renders only in frames that
// request it.
class MeshPreviewRenderer
{
public:
    MeshPreviewRenderer(rc::RenderDevice& device, const EditorViewport& viewport);

    bool Init();

    void Destroy();

    // Sizes the image, rounded up to eight pixels. Replaced images remain valid for
    // queued work and existing UI registrations.
    bool Resize(uint32_t width, uint32_t height);

    // Renders the frame in the next BuildGraph. The mesh must belong to the open scene.
    void Request(const MeshPreviewFrame& frame);

    // Drops a pending request, for example when the scene is replaced.
    void Cancel();

    void BuildGraph(rc::RenderGraph& graph);

    RHITexture* GetImage() const;

    uint32_t GetWidth() const;

    uint32_t GetHeight() const;

    // Advances whenever the image is replaced, so frontends register the new one.
    uint64_t GetImageRevision() const;

    RHISampler* GetSampler() const;

    bool SupportsWireframe() const;

private:
    rc::RenderDevice&     m_device;
    const EditorViewport& m_viewport;
    RHITexture*           m_color{nullptr};
    RHITexture*           m_depth{nullptr};
    uint32_t              m_width{0};
    uint32_t              m_height{0};
    uint64_t              m_revision{0};
    RHISampler*           m_sampler{nullptr};
    MeshPreviewFrame      m_frame;
    bool                  m_requested{false};
};
} // namespace zen::editor
