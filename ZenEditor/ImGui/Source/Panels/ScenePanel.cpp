#include "Panels/EditorPanels.h"
#include "EditorWidgets.h"
#include "SceneViewOverlays.h"
#include "LightMoveWidget.h"

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
    void CancelLightMove(EditorContext& context)
    {
        rc::RenderingSettings draft = context.editor.GetRenderingState().GetDraft();
        if (m_lightMove.Cancel(draft.lights, context.editor.GetScene().GetGeneration()))
        {
            context.editor.StageRenderingSettings(draft);
        }
    }

    void Synchronize(EditorContext& context) override
    {
        if (!visible || !context.focused || context.editor.GetLoadState().IsActive()
            || ImGui::GetFrameCount() > m_lastSceneFrame + 1)
        {
            CancelLightMove(context);
        }
    }

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
        else if (!m_lightMove.OwnsMouse())
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

        SameLineIfFits(ImGui::CalcTextSize("Move lights").x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);

        ImGui::Checkbox("Move lights", &m_moveLights);

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Show draggable handles for point and spot lights, including disabled lights.\n"
                              "Drag across the view; orbit to change the movement plane. Esc cancels.");
        }

        const RHIGPUMemoryStats& memory = GetRenderSnapshot(context).memory;

        const std::string usage =
            memory.available ? fmt::format("GPU {:.0f} MiB", memory.deviceLocalBytes / (1024.0 * 1024.0)) : "GPU memory n/a";

        const std::string profile = fmt::format("{:.1f} FPS  |  {:.2f} ms  |  {}  |  Vulkan",
                                                context.frameMs > 0 ? 1000.0f / context.frameMs : 0, context.frameMs, usage);

        // Reserve a stable field: changing digit counts must never reflow the toolbar
        // and resize the render target (or cancel a drag through a new camera aspect).
        // Rare longer readings are clipped, with the full text in a tooltip.
        const float profileWidth = ImGui::CalcTextSize("0000.0 FPS  |  000.00 ms  |  GPU 00000 MiB  |  Vulkan").x;

        const float profileX     = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - profileWidth;

        // Keep the readout beside the controls when it fits; narrow panels wrap it below.
        if (profileX >= ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetStyle().ItemSpacing.x)
        {
            ImGui::SameLine(profileX);
        }

        DrawClippedText(profile.c_str(), ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x), ImGui::GetFrameHeight()),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled));

        bool drewScene = false;

        if (context.editor.GetScene().Get() == nullptr || !context.editor.GetViewport().HasScene())
        {
            if (context.editor.GetScene().Get() == nullptr)
            {
                DrawHint(FormatOpenSceneHint(context.editor.GetActions(), "to start exploring.").c_str());
            }
            else
            {
                DrawHint("This scene contains no renderable geometry. Its nodes are available in the hierarchy.");
            }

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

                drewScene                  = true;

                m_lastSceneFrame           = ImGui::GetFrameCount();

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

                const bool allowed = DrawSceneImage(ui::ImGuiRenderer::GetTextureID(m_image), extent, context.focused)
                                  && !context.editor.GetLoadState().IsActive();

                // Read before the overlays, whose tooltips are separate windows.
                const bool hovered = ImGui::IsItemHovered();

                const ImVec2 end(origin.x + extent.x, origin.y + extent.y);

                // The sphere responds only while no other camera drag is running.
                const ViewAxesHover sphere =
                    DrawViewAxes(context.editor.GetCamera().GetCamera().GetViewMatrix(), origin, end,
                                 allowed && hovered && m_navigation == 0 && !m_sphere.IsActive() && !m_lightMove.IsActive(),
                                 m_sphere.IsActive());

                if (context.editor.GetPreferences().showSceneControls)
                {
                    DrawSceneControlsHint(context.editor.GetActions(), origin, end);
                }

                rc::RenderingSettings draft = context.editor.GetRenderingState().GetDraft();
                if (m_lightMove.Draw(draft.lights, context.editor.GetCamera(), context.editor.GetScene().GetGeneration(),
                                     origin, extent, m_moveLights, allowed,
                                     hovered && m_navigation == 0 && !m_sphere.IsActive() && !sphere.sphere))
                {
                    context.editor.StageRenderingSettings(draft);
                }

                Navigate(context, origin, extent, allowed, hovered, sphere);
            }
        }

        if (!drewScene)
        {
            CancelLightMove(context);
        }
    }

    ui::UIRenderer*     m_renderer{nullptr};
    ui::UITextureHandle m_image;
    uint64_t            m_revision{0};
    int                 m_navigation{0};
    ViewSphereInput     m_sphere;
    LightMoveWidget     m_lightMove;
    bool                m_moveLights{false};
    int                 m_lastSceneFrame{0};
};
} // namespace

UniquePtr<EditorPanel> CreateScenePanel()
{
    return MakeUnique<ScenePanel>();
}
} // namespace zen::editor
