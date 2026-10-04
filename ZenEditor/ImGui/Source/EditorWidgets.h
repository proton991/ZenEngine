#pragma once
// Drawing helpers shared by the ImGui panels. Private to the ImGui frontend.
#include "Editor/ImGui/EditorContext.h"
#include "Editor/ImGui/EditorTheme.h"
#include "Editor/Model/EditorText.h"
#include "imgui.h"

namespace zen::editor
{
// Owns viewport drags without moving a floating panel; returns whether navigation may run.
bool DrawSceneImage(ImTextureID texture, ImVec2 extent, bool focused);

// Read-only XYZ row; narrow panels place the label above the values.
void DrawVector(const char* label, Vec3 value);

void DrawMatrix(const char* label, const Mat4& value);

const char* GetAssetKindName(SceneAssetKind kind, bool plural);

EditorIcon GetAssetIcon(SceneAssetKind kind);

std::string GetFormatLabel(asset::Format format);

// One-line summary such as "1024x1024 RGBA8 sRGB" or "2.00 s | 3 channels".
std::string GetAssetDetail(const SceneAssetItem& item);

// Material factors are linear; the UI target stores SDR-encoded color.
ImU32 GetDisplayColor(Vec4 linear);

struct PreviewImage
{
    ImTextureID texture{ImTextureID_Invalid};
    float       aspect{1.0f};
};

// UI registrations of the render service's texture previews for the current scene.
// Registrations are released when the scene changes and when the owner is destroyed.
class PreviewImages
{
public:
    PreviewImages()                                = default;

    PreviewImages(const PreviewImages&)            = delete;

    PreviewImages& operator=(const PreviewImages&) = delete;

    ~PreviewImages();

    void Synchronize(EditorContext& context);

    PreviewImage Get(EditorContext& context, SceneAssetId id);

private:
    void Clear();

    ui::UIRenderer*                        m_renderer{nullptr};
    uint64_t                               m_generation{0};
    HashMap<uint32_t, ui::UITextureHandle> m_handles;
};

// Draws a preview, material swatch or kind icon centered in the square [min, min + size].
void DrawAssetThumbnail(EditorContext& context, PreviewImages& previews, const SceneAssetItem& item, ImVec2 min, float size);
} // namespace zen::editor
