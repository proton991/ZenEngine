#include "EditorWidgets.h"
#include <algorithm>

namespace zen::editor
{
float EditorControlWidth(float preferredWidth)
{
    return std::max(1.0f, std::min(preferredWidth * EditorScale(), ImGui::GetContentRegionAvail().x));
}

void SameLineIfFits(float nextItemWidth)
{
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;

    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + nextItemWidth <= right)
    {
        ImGui::SameLine();
    }
}

bool DrawSceneImage(ImTextureID texture, ImVec2 extent, bool focused)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##SceneImage", extent,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight
                               | ImGuiButtonFlags_MouseButtonMiddle);

    ImGui::GetWindowDrawList()->AddImage(texture, origin, ImVec2(origin.x + extent.x, origin.y + extent.y));

    const ImGuiIO& io = ImGui::GetIO();

    return focused && ImGui::IsWindowFocused() && !io.WantTextInput && (!ImGui::IsAnyItemActive() || ImGui::IsItemActive())
        && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
}

void DrawVector(const char* label, Vec3 value)
{
    const float scale  = EditorScale();

    const bool compact = ImGui::GetContentRegionAvail().x < 310 * scale;

    if (compact)
    {
        ImGui::TextDisabled("%s", label);
    }

    // Equal fixed weights. Proportional weights in a non-resizable table are recomputed
    // every frame from the previous content widths; full-width inputs feed those widths
    // back, so all but the last axis column collapse.
    if (ImGui::BeginTable(label, compact ? 3 : 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
    {
        if (!compact)
        {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 83 * scale);
        }

        for (const char* axis : {"X", "Y", "Z"})
        {
            ImGui::TableSetupColumn(axis, ImGuiTableColumnFlags_WidthStretch, 1.0f);
        }

        ImGui::TableNextRow();

        if (!compact)
        {
            ImGui::TableNextColumn();

            ImGui::AlignTextToFramePadding();

            ImGui::TextDisabled("%s", label);
        }

        for (int axis = 0; axis < 3; ++axis)
        {
            ImGui::TableNextColumn();

            ImGui::AlignTextToFramePadding();

            ImGui::TextColored(GetEditorPalette().axis[axis], "%c", 'X' + axis);

            ImGui::SameLine(0, 4 * scale);

            ImGui::SetNextItemWidth(EditorControlWidth(100));

            ImGui::PushID(axis);

            float number = value[axis];

            ImGui::InputFloat("##Value", &number, 0, 0, "%.3f", ImGuiInputTextFlags_ReadOnly);

            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}

void DrawMatrix(const char* label, const Mat4& value)
{
    if (ImGui::TreeNode(label))
    {
        for (int row = 0; row < 4; ++row)
        {
            ImGui::Text("%.3f  %.3f  %.3f  %.3f", value[0][row], value[1][row], value[2][row], value[3][row]);
        }

        ImGui::TreePop();
    }
}

const char* GetAssetKindName(SceneAssetKind kind, bool plural)
{
    const char* names[][2] = {
        {"Mesh", "Meshes"}, {"Material", "Materials"}, {"Texture", "Textures"}, {"Animation", "Animations"}};

    return names[uint32_t(kind)][plural ? 1 : 0];
}

EditorIcon GetAssetIcon(SceneAssetKind kind)
{
    const EditorIcon icons[] = {EditorIcon::Cube, EditorIcon::Sphere, EditorIcon::Image, EditorIcon::Play};

    return icons[uint32_t(kind)];
}

std::string GetFormatLabel(asset::Format format)
{
    std::string result;

    switch (format)
    {
        case asset::Format::R8_UNORM: result = "R8"; break;
        case asset::Format::R8G8_UNORM: result = "RG8"; break;
        case asset::Format::R8G8B8A8_UNORM: result = "RGBA8"; break;
        case asset::Format::R8G8B8A8_SRGB: result = "RGBA8 sRGB"; break;
        case asset::Format::R16G16B16A16_SFLOAT: result = "RGBA16F"; break;
        case asset::Format::R32G32B32A32_SFLOAT: result = "RGBA32F"; break;
        case asset::Format::BC1_RGB_UNORM_BLOCK:
        case asset::Format::BC1_RGBA_UNORM_BLOCK: result = "BC1"; break;
        case asset::Format::BC1_RGB_SRGB_BLOCK:
        case asset::Format::BC1_RGBA_SRGB_BLOCK: result = "BC1 sRGB"; break;
        case asset::Format::BC3_UNORM_BLOCK: result = "BC3"; break;
        case asset::Format::BC3_SRGB_BLOCK: result = "BC3 sRGB"; break;
        case asset::Format::BC4_UNORM_BLOCK: result = "BC4"; break;
        case asset::Format::BC5_UNORM_BLOCK: result = "BC5"; break;
        case asset::Format::BC6H_UFLOAT_BLOCK: result = "BC6H"; break;
        case asset::Format::BC7_UNORM_BLOCK: result = "BC7"; break;
        case asset::Format::BC7_SRGB_BLOCK: result = "BC7 sRGB"; break;
        case asset::Format::ASTC_4x4_UNORM_BLOCK: result = "ASTC 4x4"; break;
        case asset::Format::ASTC_4x4_SRGB_BLOCK: result = "ASTC 4x4 sRGB"; break;
        default: result = "Format " + std::to_string(uint32_t(format)); break;
    }

    return result;
}

namespace
{
std::string GetCompactCount(uint32_t count)
{
    return count >= 1000000 ? fmt::format("{:.1f}M", count / 1000000.0)
         : count >= 10000   ? fmt::format("{:.1f}k", count / 1000.0)
                            : std::to_string(count);
}
} // namespace

std::string GetAssetDetail(const SceneAssetItem& item)
{
    std::string result;

    switch (item.id.kind)
    {
        case SceneAssetKind::Mesh:
            result = fmt::format("{} tris | {} {}", GetCompactCount(item.triangles), item.primitives,
                                 item.primitives == 1 ? "primitive" : "primitives");
            break;

        case SceneAssetKind::Material: result = fmt::format("{} {}", item.users, item.users == 1 ? "node" : "nodes"); break;

        case SceneAssetKind::Texture:
            result = fmt::format("{}x{} {}", item.width, item.height, GetFormatLabel(item.format));
            break;

        case SceneAssetKind::Animation:
            result = fmt::format("{:.2f} s | {} {}", item.duration, item.channels, item.channels == 1 ? "channel" : "channels");
            break;
    }

    return result;
}

// Material factors are linear; the UI target stores SDR-encoded color.
ImU32 GetDisplayColor(Vec4 linear)
{
    const Vec3 encoded = glm::pow(glm::clamp(Vec3(linear), Vec3(0.0f), Vec3(1.0f)), Vec3(1.0f / 2.2f));

    return ImGui::ColorConvertFloat4ToU32(ImVec4(encoded.x, encoded.y, encoded.z, 1.0f));
}

PreviewImages::~PreviewImages()
{
    Clear();
}

void PreviewImages::Synchronize(EditorContext& context)
{
    if (m_generation != context.editor.GetScene().GetGeneration())
    {
        Clear();

        m_generation = context.editor.GetScene().GetGeneration();
    }

    m_renderer = &context.renderer.GetRenderer();
}

PreviewImage PreviewImages::Get(EditorContext& context, SceneAssetId id)
{
    Synchronize(context);

    RHITexture* preview = id.generation == m_generation ? context.editor.GetViewport().GetAssetPreview(id) : nullptr;

    PreviewImage result;

    if (preview != nullptr)
    {
        if (m_handles.count(id.index) == 0)
        {
            m_handles[id.index] = m_renderer->RegisterTexture(preview, context.editor.GetViewport().GetImageSampler());
        }

        result.texture = ui::ImGuiRenderer::GetTextureID(m_handles[id.index]);

        result.aspect  = float(preview->GetWidth()) / float(std::max(1u, preview->GetHeight()));
    }

    return result;
}

void PreviewImages::Clear()
{
    for (const std::pair<const uint32_t, ui::UITextureHandle>& entry : m_handles)
    {
        m_renderer->UnregisterTexture(entry.second);
    }

    m_handles.clear();
}

void DrawAssetThumbnail(EditorContext& context, PreviewImages& previews, const SceneAssetItem& item, ImVec2 min, float size)
{
    ImDrawList& draw          = *ImGui::GetWindowDrawList();

    const SceneAssetId source = item.id.kind == SceneAssetKind::Texture  ? item.id
                              : item.id.kind == SceneAssetKind::Material ? item.baseColorTexture
                                                                         : SceneAssetId{};

    const PreviewImage image  = source.generation != 0 ? previews.Get(context, source) : PreviewImage{};

    const ImVec2 center(min.x + size * 0.5f, min.y + size * 0.5f);

    const bool material = item.id.kind == SceneAssetKind::Material;

    const ImU32 tint    = material ? GetDisplayColor(item.baseColor) : IM_COL32_WHITE;

    if (image.texture != ImTextureID_Invalid)
    {
        const ImVec2 extent = image.aspect >= 1.0f ? ImVec2(size, size / image.aspect) : ImVec2(size * image.aspect, size);

        draw.AddImage(image.texture, ImVec2(center.x - extent.x * 0.5f, center.y - extent.y * 0.5f),
                      ImVec2(center.x + extent.x * 0.5f, center.y + extent.y * 0.5f), ImVec2(0, 0), ImVec2(1, 1), tint);
    }
    else if (material)
    {
        draw.AddCircleFilled(center, size * 0.42f, tint, 32);

        draw.AddCircleFilled(ImVec2(center.x - size * 0.14f, center.y - size * 0.16f), size * 0.1f,
                             GetEditorPalette().swatchHighlight);
    }
    else
    {
        const float icon = size * 0.5f;

        DrawEditorIcon(GetAssetIcon(item.id.kind), ImVec2(center.x - icon * 0.5f, center.y - icon * 0.5f), icon,
                       GetEditorPalette().icon);
    }
}
} // namespace zen::editor
