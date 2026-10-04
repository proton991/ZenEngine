#include "MeshPreviewWidget.h"
#include "Editor/ImGui/EditorTheme.h"
#include "imgui.h"
#include <algorithm>

namespace zen::editor
{
MeshPreviewWidget::~MeshPreviewWidget()
{
    if (m_renderer != nullptr)
    {
        m_renderer->UnregisterTexture(m_image);
    }
}

void MeshPreviewWidget::HandleInput(EditorContext& context)
{
    const ImGuiIO& io = ImGui::GetIO();

    // Wheel zooms the preview instead of scrolling the Inspector.
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);

    CameraInput input;

    input.seconds = context.seconds;

    input.dolly   = ImGui::IsItemHovered() ? io.MouseWheel : 0.0f;

    if (ImGui::IsItemActive())
    {
        const Vec2 delta(io.MouseDelta.x, io.MouseDelta.y);

        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle))
        {
            input.pan = delta;
        }
        else
        {
            input.orbit = delta;
        }
    }

    if (input.dolly != 0.0f || input.orbit != Vec2(0.0f) || input.pan != Vec2(0.0f))
    {
        context.editor.GetMeshPreviewCamera().Apply(input);
    }
}

void MeshPreviewWidget::Draw(EditorContext& context, SceneAssetId mesh)
{
    EditorController& editor      = context.editor;

    MeshPreviewSettings& settings = editor.GetMeshPreviewSettings();

    MeshPreviewRenderer& preview  = editor.GetMeshPreview();

    const float scale             = EditorScale();

    int shading                   = int(settings.shading);

    const char* shadings[]        = {"Flat", "Smooth", "Normals"};

    ImGui::SetNextItemWidth(110 * scale);

    if (ImGui::Combo("##PreviewShading", &shading, shadings, 3))
    {
        settings.shading = MeshPreviewShading(shading);
    }

    ImGui::SameLine();

    ImGui::BeginDisabled(!preview.SupportsWireframe());

    ImGui::Checkbox("Wireframe", &settings.wireframe);

    ImGui::EndDisabled();

    if (!preview.SupportsWireframe() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("This GPU does not support line fill mode.");
    }

    ImGui::SameLine();

    if (ImGui::Button("Frame"))
    {
        editor.ResetMeshPreview();
    }

    const ImVec2 pixels = ImGui::GetIO().DisplayFramebufferScale;

    const float width   = ImGui::GetContentRegionAvail().x;

    const float height  = std::clamp(width * 0.75f, 160 * scale, 480 * scale);

    if (width > 1 && editor.PreviewMesh(mesh, uint32_t(width * pixels.x), uint32_t(height * pixels.y)))
    {
        if (m_revision != preview.GetImageRevision())
        {
            m_renderer = &context.renderer.GetRenderer();

            m_renderer->UnregisterTexture(m_image);

            m_image    = m_renderer->RegisterTexture(preview.GetImage(), preview.GetSampler());

            m_revision = preview.GetImageRevision();
        }

        const ImVec2 start = ImGui::GetCursorScreenPos();

        const ImVec2 end(start.x + width, start.y + height);

        // A button owns the drag, so it neither moves a floating window nor reaches the scene.
        ImGui::InvisibleButton("##MeshPreview", ImVec2(width, height),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight
                                   | ImGuiButtonFlags_MouseButtonMiddle);

        HandleInput(context);

        // The image is rounded up to whole 8-pixel blocks; show it at its own aspect.
        const float imageWidth  = float(preview.GetWidth()) / pixels.x;

        const float imageHeight = float(preview.GetHeight()) / pixels.y;

        const float fit         = std::min(width / imageWidth, height / imageHeight);

        const ImVec2 extent(imageWidth * fit, imageHeight * fit);

        const ImVec2 origin(start.x + (width - extent.x) * 0.5f, start.y + (height - extent.y) * 0.5f);

        ImDrawList& draw = *ImGui::GetWindowDrawList();

        draw.AddRectFilled(start, end, GetEditorPalette().previewBackground);

        draw.AddImage(ui::ImGuiRenderer::GetTextureID(m_image), origin, ImVec2(origin.x + extent.x, origin.y + extent.y));

        draw.AddRect(start, end, ImGui::GetColorU32(ImGuiCol_Border));

        ImGui::TextDisabled("Drag: orbit | Middle drag: pan | Wheel: zoom");
    }
    else
    {
        ImGui::TextDisabled("This mesh has no GPU geometry to preview.");
    }
}
} // namespace zen::editor
