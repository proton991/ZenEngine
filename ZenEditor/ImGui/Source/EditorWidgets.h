#pragma once
// Drawing helpers shared by the ImGui panels. Private to the ImGui frontend.
#include "Editor/ImGui/EditorContext.h"
#include "Editor/ImGui/EditorTheme.h"
#include "Editor/Model/EditorText.h"
#include "imgui.h"

namespace zen::editor
{
// Caps a control's logical width and fits it to the current panel at the UI scale.
float EditorControlWidth(float preferredWidth);

// Keeps toolbar items together when their measured pixel width fits, otherwise wraps.
void SameLineIfFits(float nextItemWidth);

// The render service's state for the current UI frame. Every view shares one read.
const EditorRenderSnapshot& GetRenderSnapshot(EditorContext& context);

// "PBR" or "PBR + voxel GI".
const char* GetRenderAlgorithmName(rc::RenderAlgorithm algorithm);

// The display name of a node or asset; a node takes precedence when both are set.
std::string GetInspectionTargetName(const EditorScene& scene, InspectionTarget target);

// Reserves extent and draws one line of text vertically centered in it, clipped at its
// right edge. Hovering clipped text shows all of it, also in disabled sections.
void DrawClippedText(const char* text, ImVec2 extent, ImU32 color);

// Starts a property row: the label in a left column and the next widget filling the
// rest, or the label above the widget in narrow panels. Returns the widget's label,
// which hides the text and keeps the widget's ID stable across both layouts.
std::string PropertyLabel(const char* label);

// Explanatory text, muted so it does not compete with the controls around it.
void DrawHint(const char* text);

// Wrapped text in a status color from the palette, such as a warning or an error.
void DrawStatusText(const ImVec4& color, const char* text);

// "Open a scene with File > Open (Ctrl+O) <purpose>", using the registered shortcut.
std::string FormatOpenSceneHint(const EditorActions& registry, const char* purpose);

// One display line of a log entry; entries may span several lines.
struct LogLine
{
    uint32_t entry{0};
    uint32_t begin{0};
    uint32_t end{0};
};

// Splits entries at line breaks so every row has the same height, as list clipping
// requires. A trailing line break does not add an empty row.
HeapVector<LogLine> SplitLogLines(const HeapVector<EditorLogEntry>& entries);

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
