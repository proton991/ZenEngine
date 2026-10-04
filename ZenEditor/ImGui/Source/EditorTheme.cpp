#include "Editor/ImGui/EditorTheme.h"
#include "Utils/Errors.h"
#include <filesystem>
#include <string>

namespace zen::editor
{
namespace
{
ImVec4 Shade(unsigned int hex, float alpha = 1.0f)
{
    return ImVec4(float((hex >> 16) & 255) / 255.0f, float((hex >> 8) & 255) / 255.0f, float(hex & 255) / 255.0f, alpha);
}
} // namespace

const EditorPalette& GetEditorPalette()
{
    static const EditorPalette palette = {.brand             = Shade(0x59abfa),
                                          .icon              = IM_COL32(126, 172, 213, 255),
                                          .iconStrong        = IM_COL32(121, 178, 224, 255),
                                          .cardBackground    = IM_COL32(24, 32, 40, 255),
                                          .swatchHighlight   = IM_COL32(255, 255, 255, 70),
                                          .treeSelection     = Shade(0x33597d),
                                          .windowButtonHover = IM_COL32(55, 70, 84, 255),
                                          .closeButtonHover  = IM_COL32(190, 50, 58, 255),
                                          .axis              = {Shade(0xe85957), Shade(0x70c763), Shade(0x59a1f0)},
                                          .axisLabel         = IM_COL32(16, 20, 26, 255),
                                          .previewBackground = IM_COL32(27, 32, 38, 255)};

    return palette;
}

void ApplyEditorTheme(float scale)
{
    ImGuiIO& io = ImGui::GetIO();

    std::error_code error;

    if (std::filesystem::is_regular_file(ZEN_EDITOR_FONT_PATH, error))
    {
        io.Fonts->Clear();

        ImFontConfig font;

        font.SizePixels        = 15.0f * scale;

        font.RasterizerDensity = io.DisplayFramebufferScale.x;

        io.FontDefault         = io.Fonts->AddFontFromFileTTF(ZEN_EDITOR_FONT_PATH, font.SizePixels, &font);
    }
    else
    {
        LOGW("Editor proportional font unavailable; using the adapter's fallback font");
    }

    ImGuiStyle& style = ImGui::GetStyle();

    style             = ImGuiStyle();

    ImGui::StyleColorsDark(&style);

    style.WindowPadding    = ImVec2(12, 10);

    style.FramePadding     = ImVec2(8, 6);

    style.ItemSpacing      = ImVec2(8, 7);

    style.ItemInnerSpacing = ImVec2(6, 4);

    style.CellPadding      = ImVec2(4, 5);

    style.IndentSpacing    = 18;

    style.ScrollbarSize    = 12;

    style.GrabMinSize      = 10;

    style.WindowBorderSize = style.ChildBorderSize = style.PopupBorderSize = 1;

    style.FrameBorderSize                                                  = 1;

    style.WindowRounding                                                   = 3;

    style.ChildRounding = style.FrameRounding = style.PopupRounding = style.TabRounding = 3;

    style.ScrollbarRounding                                                             = 6;

    style.TabBorderSize                                                                 = 0;

    style.TabBarBorderSize                                                              = 1;

    style.TabBarOverlineSize                                                            = 2;

    style.DockingSeparatorSize                                                          = 4;

    style.DisabledAlpha                                                                 = 0.48f;

    ImVec4* colors                                                                      = style.Colors;

    colors[ImGuiCol_Text]                                                               = Shade(0xe1e6ec);

    colors[ImGuiCol_TextDisabled]                                                       = Shade(0x89949f);

    colors[ImGuiCol_WindowBg]                                                           = Shade(0x202830);

    colors[ImGuiCol_ChildBg]                                                            = Shade(0x1c242b);

    colors[ImGuiCol_PopupBg]                                                            = Shade(0x242d36);

    colors[ImGuiCol_Border]                                                             = Shade(0x36434e);

    colors[ImGuiCol_BorderShadow]                                                       = Shade(0x000000, 0);

    colors[ImGuiCol_FrameBg]                                                            = Shade(0x171f26);

    colors[ImGuiCol_FrameBgHovered]                                                     = Shade(0x293947);

    colors[ImGuiCol_FrameBgActive]                                                      = Shade(0x30495e);

    colors[ImGuiCol_TitleBg] = colors[ImGuiCol_TitleBgCollapsed] = Shade(0x1b232b);

    colors[ImGuiCol_TitleBgActive]                               = Shade(0x252f39);

    colors[ImGuiCol_MenuBarBg]                                   = Shade(0x1b232b);

    colors[ImGuiCol_Button]                                      = Shade(0x26313b);

    colors[ImGuiCol_ButtonHovered]                               = Shade(0x344b60);

    colors[ImGuiCol_ButtonActive]                                = Shade(0x3e6485);

    colors[ImGuiCol_Header]                                      = Shade(0x293540);

    colors[ImGuiCol_HeaderHovered]                               = Shade(0x354d65);

    colors[ImGuiCol_HeaderActive]                                = Shade(0x3a6287);

    colors[ImGuiCol_CheckMark] = colors[ImGuiCol_SliderGrab] = Shade(0x63adf4);

    colors[ImGuiCol_SliderGrabActive]                        = Shade(0x88c1f8);

    colors[ImGuiCol_Separator]                               = Shade(0x36434e);

    colors[ImGuiCol_SeparatorHovered] = colors[ImGuiCol_SeparatorActive] = Shade(0x548fc6);

    colors[ImGuiCol_Tab] = colors[ImGuiCol_TabDimmed] = Shade(0x1a232b);

    colors[ImGuiCol_TabSelected] = colors[ImGuiCol_TabDimmedSelected] = Shade(0x293540);

    colors[ImGuiCol_TabHovered]                                       = Shade(0x33495e);

    colors[ImGuiCol_TabSelectedOverline] = colors[ImGuiCol_TabDimmedSelectedOverline] = Shade(0x528fc9);

    colors[ImGuiCol_DockingPreview]                                                   = Shade(0x528fc9, 0.55f);

    colors[ImGuiCol_DockingEmptyBg]                                                   = Shade(0x141c23);

    colors[ImGuiCol_ScrollbarBg]                                                      = Shade(0x1a2229);

    colors[ImGuiCol_ScrollbarGrab]                                                    = Shade(0x414f5d);

    colors[ImGuiCol_ScrollbarGrabHovered]                                             = Shade(0x566a7a);

    colors[ImGuiCol_ScrollbarGrabActive]                                              = Shade(0x65829a);

    colors[ImGuiCol_TableHeaderBg]                                                    = Shade(0x293540);

    colors[ImGuiCol_TableBorderStrong] = colors[ImGuiCol_TableBorderLight] = Shade(0x303d48);

    colors[ImGuiCol_TextSelectedBg]                                        = Shade(0x3a6287, 0.8f);

    colors[ImGuiCol_NavCursor]                                             = Shade(0x63adf4);

    style.ScaleAllSizes(scale);
}

float EditorScale()
{
    return ImGui::GetFontSize() / 15.0f;
}

void DrawEditorIcon(EditorIcon icon, ImVec2 origin, float size, ImU32 color)
{
    ImDrawList& draw   = *ImGui::GetWindowDrawList();

    const float x      = origin.x;

    const float y      = origin.y;

    const float s      = size;

    const float stroke = 1.4f * EditorScale();

    switch (icon)
    {
        case EditorIcon::Play:
            draw.AddTriangleFilled(ImVec2(x + s * 0.2f, y), ImVec2(x + s, y + s * 0.5f), ImVec2(x + s * 0.2f, y + s), color);
            break;

        case EditorIcon::Pause:
            draw.AddRectFilled(ImVec2(x + s * 0.15f, y), ImVec2(x + s * 0.4f, y + s), color);

            draw.AddRectFilled(ImVec2(x + s * 0.6f, y), ImVec2(x + s * 0.85f, y + s), color);
            break;

        case EditorIcon::Stop: draw.AddRectFilled(origin, ImVec2(x + s, y + s), color, s * 0.1f); break;

        case EditorIcon::Open:
        case EditorIcon::Folder:
            draw.AddRectFilled(ImVec2(x, y + s * 0.2f), ImVec2(x + s * 0.42f, y + s * 0.5f), color, s * 0.06f);

            draw.AddRectFilled(ImVec2(x, y + s * 0.35f), ImVec2(x + s, y + s * 0.92f), color, s * 0.08f);
            break;

        case EditorIcon::Save:
            draw.AddRect(origin, ImVec2(x + s, y + s), color, s * 0.05f, 0, stroke);

            draw.AddRect(ImVec2(x + s * 0.23f, y), ImVec2(x + s * 0.75f, y + s * 0.4f), color, 0, 0, stroke);

            draw.AddRect(ImVec2(x + s * 0.23f, y + s * 0.6f), ImVec2(x + s * 0.75f, y + s), color, 0, 0, stroke);
            break;

        case EditorIcon::Undo:
        case EditorIcon::Redo:
        case EditorIcon::Rotate:
        {
            const bool redo = icon == EditorIcon::Redo;

            draw.PathArcTo(ImVec2(x + s * 0.5f, y + s * 0.5f), s * 0.4f, redo ? 0.9f : -2.4f, redo ? 5.5f : 2.2f, 16);

            draw.PathStroke(color, 0, stroke);

            if (redo)
            {
                draw.AddTriangleFilled(ImVec2(x + s, y + s * 0.2f), ImVec2(x + s * 0.55f, y),
                                       ImVec2(x + s * 0.65f, y + s * 0.45f), color);
            }
            else
            {
                draw.AddTriangleFilled(ImVec2(x, y + s * 0.2f), ImVec2(x + s * 0.45f, y), ImVec2(x + s * 0.35f, y + s * 0.45f),
                                       color);
            }
        }
        break;

        case EditorIcon::Move:
            draw.AddLine(ImVec2(x, y + s * 0.5f), ImVec2(x + s, y + s * 0.5f), color, stroke);

            draw.AddLine(ImVec2(x + s * 0.5f, y), ImVec2(x + s * 0.5f, y + s), color, stroke);

            draw.AddTriangleFilled(ImVec2(x + s * 0.5f, y), ImVec2(x + s * 0.3f, y + s * 0.25f),
                                   ImVec2(x + s * 0.7f, y + s * 0.25f), color);

            draw.AddTriangleFilled(ImVec2(x + s, y + s * 0.5f), ImVec2(x + s * 0.75f, y + s * 0.3f),
                                   ImVec2(x + s * 0.75f, y + s * 0.7f), color);
            break;

        case EditorIcon::Scale:
            draw.AddRect(ImVec2(x, y + s * 0.6f), ImVec2(x + s * 0.4f, y + s), color, 0, 0, stroke);

            draw.AddLine(ImVec2(x + s * 0.25f, y + s * 0.75f), ImVec2(x + s, y), color, stroke);

            draw.AddLine(ImVec2(x + s * 0.5f, y), ImVec2(x + s, y), color, stroke);

            draw.AddLine(ImVec2(x + s, y), ImVec2(x + s, y + s * 0.5f), color, stroke);
            break;

        case EditorIcon::Cube:
        {
            const ImVec2 points[] = {{x + s * 0.5f, y},     {x + s, y + s * 0.25f}, {x + s, y + s * 0.75f},
                                     {x + s * 0.5f, y + s}, {x, y + s * 0.75f},     {x, y + s * 0.25f}};

            draw.AddPolyline(points, 6, color, ImDrawFlags_Closed, stroke);

            draw.AddLine(points[1], ImVec2(x + s * 0.5f, y + s * 0.5f), color, stroke);

            draw.AddLine(points[5], ImVec2(x + s * 0.5f, y + s * 0.5f), color, stroke);

            draw.AddLine(points[3], ImVec2(x + s * 0.5f, y + s * 0.5f), color, stroke);
        }
        break;

        case EditorIcon::Image:
            draw.AddRect(ImVec2(x, y + s * 0.1f), ImVec2(x + s, y + s * 0.9f), color, s * 0.06f, 0, stroke);

            draw.AddCircleFilled(ImVec2(x + s * 0.7f, y + s * 0.33f), s * 0.09f, color);

            draw.AddTriangleFilled(ImVec2(x + s * 0.1f, y + s * 0.8f), ImVec2(x + s * 0.38f, y + s * 0.42f),
                                   ImVec2(x + s * 0.66f, y + s * 0.8f), color);
            break;

        case EditorIcon::Sphere:
            draw.AddCircle(ImVec2(x + s * 0.5f, y + s * 0.5f), s * 0.48f, color, 24, stroke);

            draw.AddCircleFilled(ImVec2(x + s * 0.36f, y + s * 0.34f), s * 0.11f, color);
            break;
    }
}

bool EditorToolButton(const char* label, EditorIcon icon, bool enabled, const char* tooltip)
{
    ImGui::BeginDisabled(!enabled);

    const std::string caption = std::string("     ") + label;

    const bool clicked        = ImGui::Button(caption.c_str());

    const ImVec2 start        = ImGui::GetItemRectMin();

    const float size          = 14.0f * EditorScale();

    DrawEditorIcon(icon,
                   ImVec2(start.x + ImGui::GetStyle().FramePadding.x, start.y + (ImGui::GetItemRectSize().y - size) * 0.5f),
                   size, ImGui::GetColorU32(ImGuiCol_Text));

    ImGui::EndDisabled();

    if (tooltip != nullptr && tooltip[0] != 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", tooltip);
    }

    return clicked;
}

void EditorToolbarSeparator()
{
    ImGui::SameLine(0, 10 * EditorScale());

    const ImVec2 start = ImGui::GetCursorScreenPos();

    ImGui::GetWindowDrawList()->AddLine(start, ImVec2(start.x, start.y + ImGui::GetFrameHeight()),
                                        ImGui::GetColorU32(ImGuiCol_Border));

    ImGui::Dummy(ImVec2(1, ImGui::GetFrameHeight()));

    ImGui::SameLine(0, 10 * EditorScale());
}
} // namespace zen::editor
