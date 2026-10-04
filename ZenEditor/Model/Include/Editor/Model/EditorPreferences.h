#pragma once
#include "Editor/Model/EditorCamera.h"
#include "Templates/HashMap.h"
#include "Templates/HeapVector.h"
#include <filesystem>
#include <string>

namespace zen::editor
{
constexpr uint32_t kMaxEditorRecentFiles = 10;

// Most recent first, without duplicates or empty entries.
class RecentFiles
{
public:
    // Stores an absolute, normalized UTF-8 path at the front.
    void Add(const std::string& path);

    void Set(const HeapVector<std::string>& files);

    void Clear();

    const HeapVector<std::string>& Get() const;

private:
    HeapVector<std::string> m_files;
};

// Toolkit-independent user preferences. The toolkit's layout file is stored separately,
// so replacing the UI toolkit may reset docking without losing these values.
struct EditorPreferences
{
    // Keyed by stable panel ID; panels without an entry use their default visibility.
    HashMap<std::string, bool> panels;
    RecentFiles                recentFiles;
    // The Scene view's mouse and keyboard hint.
    bool showSceneControls{true};
    // Normalized scene units per second; shared by all imported models.
    float cameraMoveSpeed{kDefaultEditorCameraMoveSpeed};
};

// %LOCALAPPDATA%/ZenEngine/ZenEditor, else $XDG_CONFIG_HOME or ~/.config, else the temp directory.
std::filesystem::path GetDefaultEditorSettingsDirectory();

// Reads preferences-v3.txt, or migrates earlier versions. A missing or invalid file
// leaves the preferences unchanged and returns false.
bool LoadEditorPreferences(const std::filesystem::path& directory, EditorPreferences& preferences);

// Replaces preferences-v3.txt through a temporary file.
bool SaveEditorPreferences(const std::filesystem::path& directory, const EditorPreferences& preferences);
} // namespace zen::editor
