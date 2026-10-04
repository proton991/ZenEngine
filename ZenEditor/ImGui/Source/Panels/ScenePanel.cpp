#include "Panels/EditorPanels.h"
#include "EditorWidgets.h"

namespace zen::editor
{
namespace
{
class ScenePanel final : public EditorPanel
{
public:
    ScenePanel() : EditorPanel({"SceneViewport", "Scene", EditorDockArea::Center}) {}

    ~ScenePanel() override
    {
        if (m_renderer != nullptr)
        {
            m_renderer->UnregisterTexture(m_image);
        }
    }

private:
    // A narrow frame leaves the scene image as large as possible.
    int PushWindowStyle() override
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5 * EditorScale(), 5 * EditorScale()));

        return 1;
    }

    // Shortcuts such as F and Home are global actions; this handles camera input and picks.
    void Navigate(EditorContext& context, ImVec2 origin, ImVec2 extent, bool allowed)
    {
        const ImGuiIO& io  = ImGui::GetIO();

        const bool hovered = ImGui::IsItemHovered();

        if (!allowed || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_navigation = 0;
        }
        else
        {
            if (hovered)
            {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                {
                    m_navigation = 1;
                }

                if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    m_navigation = 2;
                }

                if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
                {
                    m_navigation = 3;
                }
            }

            if ((m_navigation == 1 && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
                || (m_navigation == 2 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
                || (m_navigation == 3 && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)))
            {
                m_navigation = 0;
            }

            CameraInput input;

            input.seconds = context.seconds;

            input.fast    = io.KeyShift;

            input.dolly   = hovered ? io.MouseWheel : 0;

            const Vec2 delta(io.MouseDelta.x, io.MouseDelta.y);

            if (m_navigation == 1)
            {
                input.look   = delta;

                input.move.x = float(ImGui::IsKeyDown(ImGuiKey_D)) - float(ImGui::IsKeyDown(ImGuiKey_A));

                input.move.y = float(ImGui::IsKeyDown(ImGuiKey_E)) - float(ImGui::IsKeyDown(ImGuiKey_Q));

                input.move.z = float(ImGui::IsKeyDown(ImGuiKey_W)) - float(ImGui::IsKeyDown(ImGuiKey_S));
            }
            else if (m_navigation == 2)
            {
                input.orbit = delta;
            }
            else if (m_navigation == 3)
            {
                input.pan = delta;
            }

            context.editor.GetCamera().Apply(input);

            if (hovered && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                context.editor.Pick(Vec2((io.MousePos.x - origin.x) / extent.x, (io.MousePos.y - origin.y) / extent.y));
            }
        }
    }

    void DrawContents(EditorContext& context) override
    {
        const float scaleFactor   = EditorScale();

        int projection            = context.editor.GetCamera().IsOrthographic() ? 1 : 0;

        const char* projections[] = {"Perspective", "Orthographic"};

        ImGui::SetNextItemWidth(132 * scaleFactor);

        if (ImGui::Combo("##Projection", &projection, projections, 2))
        {
            context.editor.GetCamera().SetOrthographic(projection == 1);
        }

        ImGui::SameLine();

        int mode            = int(context.editor.GetViewport().GetSnapshot().requestedMode);

        const char* modes[] = {"Voxels", "PBR", "Voxel GI"};

        ImGui::SetNextItemWidth(112 * scaleFactor);

        if (ImGui::Combo("##RenderMode", &mode, modes, 3))
        {
            context.editor.GetViewport().SetRenderMode(static_cast<rc::RenderOption>(mode));
        }

        ImGui::SameLine();

        if (ImGui::Button("Frame All"))
        {
            context.editor.GetActions().Execute(actions::FrameAll);
        }

        if (context.editor.GetScene().Get() == nullptr || !context.editor.GetViewport().HasScene())
        {
            ImGui::TextWrapped(context.editor.GetScene().Get() == nullptr
                                   ? "Open a scene with File > Open (Ctrl+O) to start exploring."
                                   : "This scene contains no renderable geometry. Its nodes are available in the hierarchy.");

            m_navigation = 0;
        }
        else
        {
            const ImVec2 available = ImGui::GetContentRegionAvail();

            const ImVec2 scale     = ImGui::GetIO().DisplayFramebufferScale;

            if (available.x > 1 && available.y > 1
                && context.editor.ResizeViewport(uint32_t(available.x * scale.x), uint32_t(available.y * scale.y)))
            {
                context.sceneVisible       = true;

                const rc::RenderView& view = context.editor.GetViewport().GetRenderView();

                if (m_revision != context.editor.GetViewport().GetTargetRevision())
                {
                    m_renderer = &context.renderer.GetRenderer();

                    m_renderer->UnregisterTexture(m_image);

                    m_image    = m_renderer->RegisterTexture(view.color, context.editor.GetViewport().GetImageSampler());

                    m_revision = context.editor.GetViewport().GetTargetRevision();
                }

                const float fit = std::min(available.x / float(view.width), available.y / float(view.height));

                const ImVec2 extent(view.width * fit, view.height * fit);

                const ImVec2 start = ImGui::GetCursorScreenPos();

                const ImVec2 origin(start.x + (available.x - extent.x) * 0.5f, start.y + (available.y - extent.y) * 0.5f);

                ImGui::SetCursorScreenPos(origin);

                const bool allowed = DrawSceneImage(ui::ImGuiRenderer::GetTextureID(m_image), extent, context.focused);

                Navigate(context, origin, extent, allowed);
            }
        }
    }

    ui::UIRenderer*     m_renderer{nullptr};
    ui::UITextureHandle m_image;
    uint64_t            m_revision{0};
    int                 m_navigation{0};
};
} // namespace

UniquePtr<EditorPanel> CreateScenePanel()
{
    return MakeUnique<ScenePanel>();
}
} // namespace zen::editor
