#include "Panels/EditorPanels.h"
#include "EditorWidgets.h"
#include "SceneViewOverlays.h"

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

    // Shortcuts such as F and Home are global actions; this handles camera input, picks
    // and the orientation sphere, which owns left clicks and drags that start on it.
    void Navigate(EditorContext& context, ImVec2 origin, ImVec2 extent, bool allowed, bool hovered, const ViewAxesHover& sphere)
    {
        const ImGuiIO& io = ImGui::GetIO();

        if (!allowed || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_navigation = 0;

            m_sphere.Cancel();
        }
        else
        {
            m_sphere.Update(context.editor.GetCamera(), sphere, hovered && !io.KeyAlt && m_navigation == 0);

            if (hovered && !m_sphere.IsActive())
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

            input.seconds   = context.seconds;

            input.moveSpeed = context.editor.GetPreferences().cameraMoveSpeed;

            input.fast      = io.KeyShift;

            input.dolly     = hovered ? io.MouseWheel : 0;

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

            if (hovered && !io.KeyAlt && !sphere.sphere && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                context.editor.Pick(Vec2((io.MousePos.x - origin.x) / extent.x, (io.MousePos.y - origin.y) / extent.y));
            }
        }
    }

    void DrawContents(EditorContext& context) override
    {
        if (ImGui::Button("Frame All"))
        {
            context.editor.GetActions().Execute(actions::FrameAll);
        }

        ImGui::SameLine();

        ImGui::Checkbox("Controls", &context.editor.GetPreferences().showSceneControls);

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Show mouse and keyboard controls over the scene");
        }

        if (context.editor.GetScene().Get() == nullptr || !context.editor.GetViewport().HasScene())
        {
            ImGui::TextWrapped(context.editor.GetScene().Get() == nullptr
                                   ? "Open a scene with File > Open (Ctrl+O) to start exploring."
                                   : "This scene contains no renderable geometry. Its nodes are available in the hierarchy.");

            m_navigation = 0;

            m_sphere.Cancel();
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

                // Read before the overlays, whose tooltips are separate windows.
                const bool hovered = ImGui::IsItemHovered();

                const ImVec2 end(origin.x + extent.x, origin.y + extent.y);

                // The sphere responds only while no other camera drag is running.
                const ViewAxesHover sphere =
                    DrawViewAxes(context.editor.GetCamera().GetCamera().GetViewMatrix(), origin, end,
                                 allowed && hovered && m_navigation == 0 && !m_sphere.IsActive(), m_sphere.IsActive());

                if (context.editor.GetPreferences().showSceneControls)
                {
                    DrawSceneControlsHint(context.editor.GetActions(), origin, end);
                }

                Navigate(context, origin, extent, allowed, hovered, sphere);
            }
        }
    }

    ui::UIRenderer*     m_renderer{nullptr};
    ui::UITextureHandle m_image;
    uint64_t            m_revision{0};
    int                 m_navigation{0};
    ViewSphereInput     m_sphere;
};
} // namespace

UniquePtr<EditorPanel> CreateScenePanel()
{
    return MakeUnique<ScenePanel>();
}
} // namespace zen::editor
