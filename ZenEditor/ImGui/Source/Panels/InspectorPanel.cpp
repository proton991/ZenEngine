#include "Panels/EditorPanels.h"
#include "EditorWidgets.h"
#include "InspectorNavigationWidget.h"
#include "MeshPreviewWidget.h"

namespace zen::editor
{
namespace
{
// Clickable reference rows; long lists stop after a fixed number of entries.
constexpr size_t kInspectorLinkLimit = 64;

void DrawInspectionLink(EditorContext& context, const char* label, InspectionTarget target)
{
    if (ImGui::Selectable(label))
    {
        const ImGuiIO& io = ImGui::GetIO();

        context.editor.GetInspector().Open(target, io.KeyCtrl || io.KeySuper);
    }

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Inspect reference. Ctrl/Cmd-click to open in another tab.");
    }
}

void DrawAssetLinks(EditorContext& context, const char* label, const HeapVector<SceneAssetId>& assets)
{
    if (!assets.empty() && ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen, "%s (%zu)", label, assets.size()))
    {
        for (size_t index = 0; index < std::min(assets.size(), kInspectorLinkLimit); ++index)
        {
            const SceneAssetItem item = context.editor.GetScene().GetAssets().Describe(assets[index]);

            ImGui::PushID(int(index));

            DrawInspectionLink(context, item.name.c_str(), {{}, item.id});

            ImGui::PopID();
        }

        if (assets.size() > kInspectorLinkLimit)
        {
            ImGui::TextDisabled("... %zu more", assets.size() - kInspectorLinkLimit);
        }

        ImGui::TreePop();
    }
}

void DrawNodeLinks(EditorContext& context, const char* label, const HeapVector<NodeId>& nodes)
{
    if (!nodes.empty() && ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen, "%s (%zu)", label, nodes.size()))
    {
        for (size_t index = 0; index < std::min(nodes.size(), kInspectorLinkLimit); ++index)
        {
            const std::string name = context.editor.GetScene().GetNodeDisplayName(nodes[index]);

            ImGui::PushID(int(index));

            DrawInspectionLink(context, name.c_str(), {nodes[index], {}});

            ImGui::PopID();
        }

        if (nodes.size() > kInspectorLinkLimit)
        {
            ImGui::TextDisabled("... %zu more", nodes.size() - kInspectorLinkLimit);
        }

        ImGui::TreePop();
    }
}

void DrawAssetInspection(EditorContext&              context,
                         PreviewImages&              previews,
                         MeshPreviewWidget&          meshPreview,
                         const SceneAssetInspection& data)
{
    const SceneAssetItem& item = data.item;

    const float scale          = EditorScale();

    const ImVec2 origin        = ImGui::GetCursorScreenPos();

    DrawEditorIcon(GetAssetIcon(item.id.kind), ImVec2(origin.x, origin.y + 2 * scale), 18 * scale,
                   GetEditorPalette().iconStrong);

    ImGui::Dummy(ImVec2(24 * scale, 22 * scale));

    ImGui::SameLine();

    ImGui::TextWrapped("%s", item.name.c_str());

    ImGui::TextDisabled("%s %u | Read only | %s", GetAssetKindName(item.id.kind, false), item.id.index,
                        GetAssetDetail(item).c_str());

    ImGui::Spacing();

    ImGui::Separator();

    ImGui::Spacing();

    if (item.id.kind == SceneAssetKind::Texture && ImGui::CollapsingHeader("Texture", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const float size = std::min(ImGui::GetContentRegionAvail().x, 256 * scale);

        if (previews.Get(context, item.id).texture != ImTextureID_Invalid)
        {
            const ImVec2 start = ImGui::GetCursorScreenPos();

            ImGui::Dummy(ImVec2(size, size));

            DrawAssetThumbnail(context, previews, item, start, size);
        }
        else
        {
            ImGui::TextDisabled("No preview for this format.");
        }

        ImGui::TextWrapped("Size: %u x %u | Format: %s", item.width, item.height, GetFormatLabel(item.format).c_str());

        ImGui::TextWrapped("Mip levels: %u | Source data: %.1f KiB", data.mipLevels, data.bytes / 1024.0);

        if (data.sampler >= 0)
        {
            ImGui::TextWrapped("Sampler: %d", data.sampler);
        }
        else
        {
            ImGui::TextWrapped("Sampler: default");
        }

        DrawAssetLinks(context, "Used by materials", data.usedBy);
    }

    if (item.id.kind == SceneAssetKind::Material && ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();

        const float size   = 64 * scale;

        ImGui::Dummy(ImVec2(size, size));

        DrawAssetThumbnail(context, previews, item, start, size);

        ImGui::TextWrapped("Base color: %.3f %.3f %.3f %.3f", item.baseColor.x, item.baseColor.y, item.baseColor.z,
                           item.baseColor.w);

        ImGui::TextWrapped("Metallic: %.3f | Roughness: %.3f", data.metallic, data.roughness);

        const char* alpha = data.alphaMode == sg::AlphaMode::Opaque ? "Opaque"
                          : data.alphaMode == sg::AlphaMode::Mask   ? "Mask"
                                                                    : "Blend";

        if (data.alphaMode == sg::AlphaMode::Mask)
        {
            ImGui::TextWrapped("Alpha: %s (cutoff %.3f)", alpha, data.alphaCutoff);
        }
        else
        {
            ImGui::TextWrapped("Alpha: %s", alpha);
        }

        ImGui::TextWrapped("Double sided: %s | Unlit: %s", data.doubleSided ? "Yes" : "No", data.unlit ? "Yes" : "No");

        ImGui::TextWrapped("Emissive: %.3f %.3f %.3f", data.emissive.x, data.emissive.y, data.emissive.z);

        if (!data.textures.empty()
            && ImGui::TreeNodeEx("Textures", ImGuiTreeNodeFlags_DefaultOpen, "Textures (%zu)", data.textures.size()))
        {
            for (size_t index = 0; index < data.textures.size(); ++index)
            {
                const SceneAssetItem texture = context.editor.GetScene().GetAssets().Describe(data.textures[index].texture);

                ImGui::PushID(int(index));

                ImGui::TextDisabled("%s", data.textures[index].slot);

                ImGui::SameLine(150 * scale);

                DrawInspectionLink(context, texture.name.c_str(), {{}, texture.id});

                ImGui::PopID();
            }

            ImGui::TreePop();
        }

        DrawAssetLinks(context, "Used by meshes", data.usedBy);

        DrawNodeLinks(context, "Used by nodes", data.nodes);
    }

    if (item.id.kind == SceneAssetKind::Mesh && ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen))
    {
        meshPreview.Draw(context, item.id);

        ImGui::TextWrapped("Primitives: %u | Triangles: %u", item.primitives, item.triangles);

        DrawVector("Bounds min", data.bounds.GetMin());

        DrawVector("Bounds max", data.bounds.GetMax());

        DrawAssetLinks(context, "Materials", data.materials);

        DrawNodeLinks(context, "Instances", data.nodes);
    }

    if (item.id.kind == SceneAssetKind::Animation && ImGui::CollapsingHeader("Animation", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("Duration: %.3f s | Channels: %u", item.duration, item.channels);

        ImGui::TextWrapped("Playback is frozen in the viewer.");

        DrawNodeLinks(context, "Targets", data.nodes);
    }
}

class InspectorPanel final : public EditorPanel
{
public:
    InspectorPanel() : EditorPanel({"Inspector", "Inspector", EditorDockArea::Right}) {}

private:
    void Synchronize(EditorContext& context) override
    {
        m_previews.Synchronize(context);
    }

    void DrawContents(EditorContext& context) override
    {
        InspectorNavigation& navigation = context.editor.GetInspector();

        m_navigation.Draw(navigation, context.editor.GetActions(), context.editor.GetScene(), context.focused);

        const InspectionTarget target = navigation.GetActiveTab().GetTarget();

        // Keep scrolling and expanded sections separate for each page of each tab.
        const std::string page = std::to_string(navigation.GetActiveTab().id) + ":" + std::to_string(target.node.generation)
                               + ":" + std::to_string(target.node.index) + ":" + std::to_string(target.asset.generation) + ":"
                               + std::to_string(uint32_t(target.asset.kind)) + ":" + std::to_string(target.asset.index);

        if (ImGui::BeginChild(page.c_str(), ImVec2(0, 0)))
        {
            DrawPage(context, target);
        }

        ImGui::EndChild();
    }

    void DrawPage(EditorContext& context, InspectionTarget target)
    {
        const NodeInspection data        = context.editor.GetScene().Inspect(target.node);

        const SceneAssetInspection asset = context.editor.GetScene().GetAssets().Inspect(target.asset);

        if (asset.valid)
        {
            DrawAssetInspection(context, m_previews, m_meshPreview, asset);
        }
        else if (!data.valid)
        {
            ImGui::TextWrapped("Select a node in the hierarchy, a surface in the Scene view, or an item in Assets.");
        }
        else
        {
            const ImVec2 origin = ImGui::GetCursorScreenPos();

            DrawEditorIcon(EditorIcon::Cube, ImVec2(origin.x, origin.y + 2 * EditorScale()), 18 * EditorScale(),
                           GetEditorPalette().iconStrong);

            ImGui::Dummy(ImVec2(24 * EditorScale(), 22 * EditorScale()));

            ImGui::SameLine();

            ImGui::TextWrapped("%s", context.editor.GetScene().GetNodeDisplayName(data.id).c_str());

            ImGui::TextDisabled("Node %u | Read only | %s", data.id.index, data.visible ? "Visible" : "Hidden");

            ImGui::Spacing();

            ImGui::Separator();

            ImGui::Spacing();

            if (data.hasTransform && ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
            {
                DrawVector("Position", data.translation);

                DrawVector("Rotation", data.rotationDegrees);

                DrawVector("Scale", data.scale);

                DrawMatrix("Authored Base Matrix", data.baseMatrix);

                DrawMatrix("World Matrix", data.worldMatrix);
            }

            if (data.hasMesh && ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (data.meshAsset.generation != 0)
                {
                    m_meshPreview.Draw(context, data.meshAsset);

                    const SceneAssetItem mesh = context.editor.GetScene().GetAssets().Describe(data.meshAsset);

                    ImGui::AlignTextToFramePadding();

                    ImGui::TextDisabled("Mesh asset");

                    ImGui::SameLine();

                    DrawInspectionLink(context, mesh.name.c_str(), {{}, mesh.id});
                }

                ImGui::TextWrapped("Primitives: %u | Indices: %u", data.primitives, data.indices);

                ImGui::TextWrapped("Skinned: %s", data.skinned ? "Yes (rest pose)" : "No");

                const SceneAssetInspection mesh = context.editor.GetScene().GetAssets().Inspect(data.meshAsset);

                DrawAssetLinks(context, "Materials", mesh.materials);
            }

            if (data.hasLight && ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::PushID("LightProperties");

                ImGui::TextWrapped("Type: %s", data.lightType == sg::Point  ? "Point"
                                               : data.lightType == sg::Spot ? "Spot"
                                                                            : "Directional");

                DrawVector("Color", Vec3(data.light.color));

                ImGui::TextWrapped("Intensity: %.3f | Range: %.3f", data.light.intensity, data.light.range);

                DrawVector("Position", data.light.position);

                DrawVector("Direction", Vec3(data.light.direction));

                ImGui::TextWrapped("Cone angles: %.1f / %.1f degrees", glm::degrees(data.light.innerConeAngle),
                                   glm::degrees(data.light.outerConeAngle));

                ImGui::PopID();
            }

            if (data.hasCamera && ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::TextUnformatted(data.orthographic ? "Orthographic" : "Perspective");

                ImGui::TextWrapped("FOV: %.1f degrees | Near: %.4f | Far: %.3f", data.fov, data.nearPlane, data.farPlane);

                ImGui::TextWrapped("The Scene view uses an independent editor camera.");
            }
        }
    }

    InspectorNavigationWidget m_navigation;
    PreviewImages             m_previews;
    MeshPreviewWidget         m_meshPreview;
};
} // namespace

UniquePtr<EditorPanel> CreateInspectorPanel()
{
    return MakeUnique<InspectorPanel>();
}
} // namespace zen::editor
