#pragma once
// The Inspector's material-free mesh preview. Private to the ImGui frontend.
#include "Editor/ImGui/EditorContext.h"

namespace zen::editor
{
// Shading options, the preview image and its orbit/pan/zoom input. The camera and
// options live in the controller, so they persist across selections.
class MeshPreviewWidget
{
public:
    MeshPreviewWidget()                                    = default;

    MeshPreviewWidget(const MeshPreviewWidget&)            = delete;

    MeshPreviewWidget& operator=(const MeshPreviewWidget&) = delete;

    ~MeshPreviewWidget();

    void Draw(EditorContext& context, SceneAssetId mesh);

private:
    void HandleInput(EditorContext& context);

    ui::UIRenderer*     m_renderer{nullptr};
    ui::UITextureHandle m_image;
    uint64_t            m_revision{0};
};
} // namespace zen::editor
