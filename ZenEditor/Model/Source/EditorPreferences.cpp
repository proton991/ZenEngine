#include "Editor/Model/EditorPreferences.h"
#include "Editor/Model/EditorText.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace zen::editor
{
namespace
{
constexpr const char* kPreferencesFile = "preferences-v3.txt";

constexpr const char* kSignature       = "ZenEditorPreferences3";

// Boolean options are stored as `option "name" 0|1`; builds without an option skip it.
constexpr const char* kSceneControlsOption   = "scene.controls_hint";

constexpr const char* kCameraMoveSpeedOption = "camera.move_speed";

// Versions 1 and 2 stored visibility as a bitmask in this panel order.
constexpr const char* kLegacyPanels[] = {"Hierarchy", "SceneViewport", "Inspector", "RenderSettings", "Assets", "Output"};

constexpr uint32_t kLegacyPanelCount  = 6;

void ApplyLegacyVisibility(uint32_t mask, EditorPreferences& preferences)
{
    for (uint32_t index = 0; index < kLegacyPanelCount; ++index)
    {
        preferences.panels[kLegacyPanels[index]] = (mask & (1u << index)) != 0;
    }
}

bool LoadCurrent(const std::filesystem::path& file, EditorPreferences& preferences)
{
    std::ifstream input(file);

    std::string signature;

    bool valid = std::getline(input, signature) && signature == kSignature;

    EditorPreferences loaded;

    HeapVector<std::string> recent;

    std::string line;

    while (valid && std::getline(input, line))
    {
        std::istringstream fields(line);

        fields.imbue(std::locale::classic());

        std::string key;

        std::string value;

        fields >> key >> std::quoted(value);

        if (key == "panel")
        {
            int visible          = 0;

            valid                = bool(fields >> visible) && !value.empty();

            loaded.panels[value] = visible != 0;
        }
        else if (key == "recent")
        {
            valid = bool(fields) && recent.size() < kMaxEditorRecentFiles;

            recent.push_back(value);
        }
        else if (key == "option")
        {
            int enabled = 0;

            valid       = bool(fields >> enabled) && !value.empty();

            if (value == kSceneControlsOption)
            {
                loaded.showSceneControls = enabled != 0;
            }
        }
        else if (key == "number" && value == kCameraMoveSpeedOption)
        {
            float speed = kDefaultEditorCameraMoveSpeed;

            // A malformed speed falls back without discarding other preferences.
            loaded.cameraMoveSpeed = fields >> speed ? ClampEditorCameraMoveSpeed(speed) : kDefaultEditorCameraMoveSpeed;
        }
        else
        {
            // Unknown keys come from newer builds and are skipped; blank lines are ignored.
            valid = true;
        }
    }

    if (valid)
    {
        loaded.recentFiles.Set(recent);

        preferences = std::move(loaded);
    }

    return valid;
}

bool LoadLegacy(const std::filesystem::path& directory, EditorPreferences& preferences)
{
    std::ifstream second(directory / "preferences-v2.txt");

    std::string signature;

    uint32_t mask  = 0;

    uint32_t count = 0;

    bool valid     = second >> signature >> mask >> count && signature == "ZenEditorPreferences2"
              && mask < (1u << kLegacyPanelCount) && count <= kMaxEditorRecentFiles;

    HeapVector<std::string> recent;

    for (uint32_t index = 0; valid && index < count; ++index)
    {
        std::string path;

        valid = bool(second >> std::quoted(path));

        recent.push_back(path);
    }

    if (!valid)
    {
        // Version 1 also stored an asset-catalog root, which the editor no longer uses.
        std::ifstream first(directory / "preferences-v1.txt");

        std::string root;

        valid = first >> signature >> mask >> std::quoted(root) && signature == "ZenEditorPreferences1"
             && mask < (1u << kLegacyPanelCount);

        recent.clear();
    }

    if (valid)
    {
        ApplyLegacyVisibility(mask, preferences);

        preferences.recentFiles.Set(recent);
    }

    return valid;
}
} // namespace

void RecentFiles::Add(const std::string& path)
{
    std::error_code error;

    const std::filesystem::path absolute = std::filesystem::absolute(std::filesystem::u8path(path), error);

    const std::string normalized         = error ? path : PathToUtf8(absolute.lexically_normal());

    HeapVector<std::string> files;

    files.push_back(normalized);

    for (const std::string& file : m_files)
    {
        if (file != normalized && files.size() < kMaxEditorRecentFiles)
        {
            files.push_back(file);
        }
    }

    m_files = std::move(files);
}

void RecentFiles::Set(const HeapVector<std::string>& files)
{
    m_files.clear();

    for (const std::string& file : files)
    {
        if (!file.empty() && m_files.size() < kMaxEditorRecentFiles
            && std::find(m_files.begin(), m_files.end(), file) == m_files.end())
        {
            m_files.push_back(file);
        }
    }
}

void RecentFiles::Clear()
{
    m_files.clear();
}

const HeapVector<std::string>& RecentFiles::Get() const
{
    return m_files;
}

std::filesystem::path GetDefaultEditorSettingsDirectory()
{
    const char* root = std::getenv("LOCALAPPDATA");

    if (root == nullptr)
    {
        root = std::getenv("XDG_CONFIG_HOME");
    }

    std::filesystem::path directory;

    if (root != nullptr)
    {
        directory = std::filesystem::u8path(root);
    }
    else
    {
        const char* home = std::getenv("HOME");

        directory        = home != nullptr ? std::filesystem::u8path(home) / ".config" : std::filesystem::temp_directory_path();
    }

    return directory / "ZenEngine" / "ZenEditor";
}

bool LoadEditorPreferences(const std::filesystem::path& directory, EditorPreferences& preferences)
{
    std::error_code error;

    const std::filesystem::path file = directory / kPreferencesFile;

    return std::filesystem::is_regular_file(file, error) ? LoadCurrent(file, preferences) : LoadLegacy(directory, preferences);
}

bool SaveEditorPreferences(const std::filesystem::path& directory, const EditorPreferences& preferences)
{
    std::error_code error;

    std::filesystem::create_directories(directory, error);

    const std::filesystem::path file      = directory / kPreferencesFile;

    const std::filesystem::path temporary = directory / (std::string(kPreferencesFile) + ".tmp");

    HeapVector<std::string> panels;

    for (const std::pair<const std::string, bool>& entry : preferences.panels)
    {
        panels.push_back(entry.first);
    }

    // Sorted output keeps the file stable between runs.
    std::sort(panels.begin(), panels.end());

    bool valid = false;

    {
        std::ofstream output(temporary, std::ios::trunc);

        output.imbue(std::locale::classic());

        output << kSignature << '\n';

        for (const std::string& panel : panels)
        {
            output << "panel " << std::quoted(panel) << ' ' << (preferences.panels.at(panel) ? 1 : 0) << '\n';
        }

        for (const std::string& path : preferences.recentFiles.Get())
        {
            output << "recent " << std::quoted(path) << '\n';
        }

        output << "option " << std::quoted(std::string(kSceneControlsOption)) << ' ' << (preferences.showSceneControls ? 1 : 0)
               << '\n';

        output << "number " << std::quoted(std::string(kCameraMoveSpeedOption)) << ' '
               << std::setprecision(std::numeric_limits<float>::max_digits10)
               << ClampEditorCameraMoveSpeed(preferences.cameraMoveSpeed) << '\n';

        output.flush();

        valid = bool(output);
    }

    if (valid)
    {
        std::filesystem::rename(temporary, file, error);

        valid = !error;
    }

    if (!valid)
    {
        std::filesystem::remove(temporary, error);
    }

    return valid;
}
} // namespace zen::editor
