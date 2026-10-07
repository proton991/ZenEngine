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
    static const EditorPalette palette = {.brand             = Shade(0x8bb8ff),
                                          .icon              = IM_COL32(153, 173, 204, 255),
                                          .iconStrong        = IM_COL32(139, 184, 255, 255),
                                          .cardBackground    = IM_COL32(34, 39, 50, 255),
                                          .swatchHighlight   = IM_COL32(255, 255, 255, 70),
                                          .treeSelection     = Shade(0x334867),
                                          .windowButtonHover = IM_COL32(49, 58, 74, 255),
                                          .closeButtonHover  = IM_COL32(190, 50, 58, 255),
                                          .axis              = {Shade(0xe85957), Shade(0x70c763), Shade(0x59a1f0)},
                                          .axisLabel         = IM_COL32(16, 20, 26, 255),
                                          .previewBackground = IM_COL32(27, 32, 38, 255),
                                          .warning           = Shade(0xe8b85c),
                                          .error             = Shade(0xf07178)};

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

        font.SizePixels        = 16.0f * scale;

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

    // Compact enough that docked panels show several properties at 150-200% scale.
    style.WindowPadding    = ImVec2(12, 10);

    style.FramePadding     = ImVec2(8, 4);

    style.ItemSpacing      = ImVec2(8, 6);

    style.ItemInnerSpacing = ImVec2(6, 4);

    style.CellPadding      = ImVec2(6, 4);

    style.IndentSpacing    = 18;

    style.ScrollbarSize    = 12;

    style.GrabMinSize      = 10;

    style.WindowBorderSize = style.ChildBorderSize = style.PopupBorderSize = 1;

    style.FrameBorderSize                                                  = 1;

    style.WindowRounding                                                   = 8;

    style.ChildRounding = style.PopupRounding = 8;

    style.FrameRounding = style.GrabRounding = 5;

    style.TabRounding                        = 5;

    style.ScrollbarRounding                  = 6;

    style.TabBorderSize                      = 0;

    style.TabBarBorderSize                   = 1;

    style.TabBarOverlineSize                 = 2;

    style.DockingSeparatorSize               = 4;

    // Each tab has its own close button; a second one per dock node closes every tab.
    style.DockingNodeHasCloseButton = false;

    style.DisabledAlpha             = 0.5f;

    style.SeparatorTextBorderSize   = 1;

    style.SeparatorTextPadding      = ImVec2(0, 8);

    ImVec4* colors                  = style.Colors;

    colors[ImGuiCol_Text]           = Shade(0xe8edf5);

    colors[ImGuiCol_TextDisabled]   = Shade(0x9ba8bb);

    colors[ImGuiCol_WindowBg]       = Shade(0x1b1f28);

    colors[ImGuiCol_ChildBg]        = Shade(0x222732);

    colors[ImGuiCol_PopupBg]        = Shade(0x252b37);

    colors[ImGuiCol_Border]         = Shade(0x3a4557, 0.65f);

    colors[ImGuiCol_BorderShadow]   = Shade(0x000000, 0);

    colors[ImGuiCol_FrameBg]        = Shade(0x131720);

    colors[ImGuiCol_FrameBgHovered] = Shade(0x29364a);

    colors[ImGuiCol_FrameBgActive]  = Shade(0x31445e);

    colors[ImGuiCol_TitleBg] = colors[ImGuiCol_TitleBgCollapsed] = Shade(0x151922);

    colors[ImGuiCol_TitleBgActive]                               = Shade(0x2a3446);

    colors[ImGuiCol_MenuBarBg]                                   = Shade(0x151922);

    colors[ImGuiCol_Button]                                      = Shade(0x2b3444);

    colors[ImGuiCol_ButtonHovered]                               = Shade(0x3c536f);

    colors[ImGuiCol_ButtonActive]                                = Shade(0x476895);

    colors[ImGuiCol_Header]                                      = Shade(0x2c3545);

    colors[ImGuiCol_HeaderHovered]                               = Shade(0x354b68);

    colors[ImGuiCol_HeaderActive]                                = Shade(0x3b5880);

    colors[ImGuiCol_CheckMark] = colors[ImGuiCol_SliderGrab] = Shade(0x8bb8ff);

    colors[ImGuiCol_SliderGrabActive]                        = Shade(0xb0d0ff);

    colors[ImGuiCol_Separator]                               = Shade(0x3a4557, 0.65f);

    colors[ImGuiCol_SeparatorHovered] = colors[ImGuiCol_SeparatorActive] = Shade(0x739fdf);

    colors[ImGuiCol_Tab] = colors[ImGuiCol_TabDimmed] = Shade(0x151922);

    colors[ImGuiCol_TabSelected]                      = Shade(0x2c3545);

    colors[ImGuiCol_TabDimmedSelected]                = Shade(0x232a37);

    colors[ImGuiCol_TabHovered]                       = Shade(0x354762);

    // Only the focused panel's tab carries the accent; shortcuts and navigation follow focus.
    colors[ImGuiCol_TabSelectedOverline]       = Shade(0x739fdf);

    colors[ImGuiCol_TabDimmedSelectedOverline] = Shade(0x3a4557, 0.65f);

    colors[ImGuiCol_DockingPreview]            = Shade(0x739fdf, 0.55f);

    colors[ImGuiCol_DockingEmptyBg]            = Shade(0x11141b);

    colors[ImGuiCol_ScrollbarBg]               = Shade(0x171b23);

    colors[ImGuiCol_ScrollbarGrab]             = Shade(0x3b4556);

    colors[ImGuiCol_ScrollbarGrabHovered]      = Shade(0x53627a);

    colors[ImGuiCol_ScrollbarGrabActive]       = Shade(0x6b82a3);

    colors[ImGuiCol_TableHeaderBg]             = Shade(0x2c3545);

    colors[ImGuiCol_TableBorderStrong] = colors[ImGuiCol_TableBorderLight] = Shade(0x343d4d);

    colors[ImGuiCol_TextSelectedBg]                                        = Shade(0x3b5880, 0.8f);

    colors[ImGuiCol_TextLink] = colors[ImGuiCol_NavCursor] = colors[ImGuiCol_DragDropTarget] = Shade(0x8bb8ff);

    colors[ImGuiCol_PlotHistogram]                                                           = Shade(0x8bb8ff);

    colors[ImGuiCol_PlotHistogramHovered]                                                    = Shade(0xb0d0ff);

    colors[ImGuiCol_ResizeGrip]                                                              = Shade(0x739fdf, 0.2f);

    colors[ImGuiCol_ResizeGripHovered]                                                       = Shade(0x739fdf, 0.6f);

    colors[ImGuiCol_ResizeGripActive]                                                        = Shade(0x8bb8ff, 0.9f);

    colors[ImGuiCol_TreeLines]                                                               = Shade(0x3a4557);

    colors[ImGuiCol_ModalWindowDimBg]                                                        = Shade(0x0b0d12, 0.6f);

    style.ScaleAllSizes(scale);
}

float EditorScale()
{
    return ImGui::GetFontSize() / 16.0f;
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

namespace
{
// Sizes the button from the measured icon and label instead of padding the caption
// with spaces, so the icon never overlaps the text at any font size.
bool DrawIconButton(const char* id, const char* label, EditorIcon icon, bool enabled, const char* tooltip)
{
    const ImGuiStyle& style = ImGui::GetStyle();

    const float iconSize    = 14.0f * EditorScale();

    const float height      = ImGui::GetFrameHeight();

    const float labelWidth  = label != nullptr ? style.ItemInnerSpacing.x + ImGui::CalcTextSize(label).x : 0.0f;

    const float width       = label != nullptr ? 2 * style.FramePadding.x + iconSize + labelWidth : height;

    ImGui::BeginDisabled(!enabled);

    const bool clicked = ImGui::Button(id, ImVec2(width, height));

    const ImVec2 start = ImGui::GetItemRectMin();

    const ImVec2 iconMin(label != nullptr ? start.x + style.FramePadding.x : start.x + (width - iconSize) * 0.5f,
                         start.y + (height - iconSize) * 0.5f);

    // Colors are read before EndDisabled so disabled buttons are dimmed.
    DrawEditorIcon(icon, iconMin, iconSize,
                   enabled ? ImGui::GetColorU32(GetEditorPalette().brand) : ImGui::GetColorU32(ImGuiCol_Text));

    if (label != nullptr)
    {
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(iconMin.x + iconSize + style.ItemInnerSpacing.x, start.y + style.FramePadding.y),
            ImGui::GetColorU32(ImGuiCol_Text), label);
    }

    ImGui::EndDisabled();

    if (tooltip != nullptr && tooltip[0] != 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", tooltip);
    }

    return clicked;
}
} // namespace

bool EditorToolButton(const char* label, EditorIcon icon, bool enabled, const char* tooltip)
{
    const std::string id = std::string("##") + label;

    return DrawIconButton(id.c_str(), label, icon, enabled, tooltip);
}

bool EditorIconButton(const char* id, EditorIcon icon, bool enabled, const char* tooltip)
{
    const std::string hidden = std::string("##") + id;

    return DrawIconButton(hidden.c_str(), nullptr, icon, enabled, tooltip);
}

void EditorToolbarSeparator()
{
    const float gap = 8 * EditorScale();

    ImGui::SameLine(0, gap);

    // Inset so the rule reads as a divider rather than a border.
    const ImVec2 start = ImGui::GetCursorScreenPos();

    const float inset  = ImGui::GetStyle().FramePadding.y;

    ImGui::GetWindowDrawList()->AddLine(ImVec2(start.x, start.y + inset),
                                        ImVec2(start.x, start.y + ImGui::GetFrameHeight() - inset),
                                        ImGui::GetColorU32(ImGuiCol_Border));

    ImGui::Dummy(ImVec2(1, ImGui::GetFrameHeight()));

    ImGui::SameLine(0, gap);
}
} // namespace zen::editor
