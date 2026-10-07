#include "Editor/ImGui/EditorWorkspace.h"
#include "EditorShortcuts.h"
#include "SceneLoadingDialog.h"
#include "Editor/ImGui/EditorContext.h"
#include "Editor/ImGui/EditorTheme.h"
#include "Editor/Model/EditorText.h"
#include "imgui.h"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace zen::editor
{
namespace
{
constexpr const char* kLayoutFile = "imgui-layout-v2.ini";

bool DrawWindowButton(const char* label, EditorWindowAction action, bool maximized, ImVec2 position, ImVec2 size)
{
    ImGui::SetCursorScreenPos(position);

    const bool pressed = ImGui::InvisibleButton(label, size);

    ImDrawList& draw   = *ImGui::GetWindowDrawList();

    const ImVec2 end(position.x + size.x, position.y + size.y);

    if (ImGui::IsItemHovered())
    {
        const EditorPalette& palette = GetEditorPalette();

        draw.AddRectFilled(position, end,
                           action == EditorWindowAction::Close ? palette.closeButtonHover : palette.windowButtonHover);
    }

    const float scale = EditorScale();

    const ImVec2 center(position.x + size.x * 0.5f, position.y + size.y * 0.5f);

    const float half   = 5 * scale;

    const float stroke = std::max(1.0f, scale);

    const ImU32 color  = ImGui::GetColorU32(ImGuiCol_Text);

    if (action == EditorWindowAction::Minimize)
    {
        draw.AddLine(ImVec2(center.x - half, center.y), ImVec2(center.x + half, center.y), color, stroke);
    }
    else if (action == EditorWindowAction::Close)
    {
        draw.AddLine(ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half), color, stroke);

        draw.AddLine(ImVec2(center.x - half, center.y + half), ImVec2(center.x + half, center.y - half), color, stroke);
    }
    else
    {
        const float offset = maximized ? 2 * scale : 0;

        if (maximized)
        {
            draw.AddLine(ImVec2(center.x - half + offset, center.y - half), ImVec2(center.x + half, center.y - half), color,
                         stroke);

            draw.AddLine(ImVec2(center.x + half, center.y - half), ImVec2(center.x + half, center.y + half - offset), color,
                         stroke);
        }

        draw.AddRect(ImVec2(center.x - half, center.y - half + offset), ImVec2(center.x + half - offset, center.y + half),
                     color, 0, ImDrawFlags_None, stroke);
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    {
        ImGui::SetTooltip("%s", label);
    }

    return pressed;
}

EditorMenuItem MakeActionMenuItem(const EditorActions& registry, const char* id, bool blocked)
{
    EditorMenuItem item;

    item.id = id;

    if (const EditorAction* action = registry.Find(id))
    {
        item.label    = action->label;

        item.shortcut = action->shortcut;

        item.tooltip  = action->disabledReason;
    }

    item.enabled = !blocked && registry.IsEnabled(id);

    return item;
}

void DrawActionMenuItem(EditorActions& registry, const char* id)
{
    const EditorAction* action = registry.Find(id);

    if (action != nullptr)
    {
        const bool enabled         = registry.IsEnabled(id);

        const std::string shortcut = FormatShortcut(action->shortcut);

        if (ImGui::MenuItem(action->label.c_str(), shortcut.empty() ? nullptr : shortcut.c_str(), false, enabled))
        {
            registry.Execute(id);
        }

        if (!enabled && !action->disabledReason.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", action->disabledReason.c_str());
        }
    }
}

void DrawActionButton(EditorActions& registry, const char* id, EditorIcon icon)
{
    const EditorAction* action = registry.Find(id);

    if (action != nullptr)
    {
        const bool enabled         = registry.IsEnabled(id);

        const std::string shortcut = FormatShortcut(action->shortcut);

        // Toolbar captions drop the menu ellipsis.
        std::string label = action->label;

        if (label.ends_with("..."))
        {
            label.resize(label.size() - 3);
        }

        const std::string tooltip = !enabled         ? action->disabledReason
                                  : shortcut.empty() ? label
                                                     : label + " (" + shortcut + ")";

        if (EditorToolButton(label.c_str(), icon, enabled, tooltip.c_str()))
        {
            registry.Execute(id);
        }
    }
}
} // namespace

EditorWorkspace::EditorWorkspace(std::filesystem::path settingsDirectory) :
    m_panels(CreateEditorPanels()), m_settings(std::move(settingsDirectory))
{}

void EditorWorkspace::Initialize(EditorContext& context)
{
    const HashMap<std::string, bool>& saved = context.editor.GetPreferences().panels;

    for (const UniquePtr<EditorPanel>& panel : m_panels)
    {
        const HashMap<std::string, bool>::const_iterator found = saved.find(panel->GetDesc().id);

        panel->visible = found != saved.end() ? found->second : panel->GetDesc().visibleByDefault;
    }

    context.editor.GetActions().Register({actions::ResetLayout, "Reset Layout", {}, "", nullptr, [this]() { ResetLayout(); }});

    std::error_code error;

    const std::filesystem::path layout = m_settings / kLayoutFile;

    bool restored                      = false;

    if (std::filesystem::is_regular_file(layout, error) && std::filesystem::file_size(layout, error) < 1024 * 1024)
    {
        std::ifstream file(layout);

        std::ostringstream text;

        text << file.rdbuf();

        const std::string data = text.str();

        restored = data.find("[Docking][Data]") != std::string::npos && data.find("DockSpace") != std::string::npos;

        if (restored)
        {
            ImGui::LoadIniSettingsFromMemory(data.c_str(), data.size());
        }
    }

    m_resetLayout = !restored;

    m_menuBar.Initialize();

    UpdateNativeMenus(context);
}

void EditorWorkspace::Save(EditorContext& context)
{
    std::error_code error;

    std::filesystem::create_directories(m_settings, error);

    size_t size        = 0;

    const char* layout = ImGui::SaveIniSettingsToMemory(&size);

    std::ofstream layoutFile(m_settings / kLayoutFile, std::ios::binary);

    layoutFile.write(layout, static_cast<std::streamsize>(size));

    if (!layoutFile)
    {
        LOGW("Could not save the editor layout");
    }

    for (const UniquePtr<EditorPanel>& panel : m_panels)
    {
        context.editor.GetPreferences().panels[panel->GetDesc().id] = panel->visible;
    }
}

void EditorWorkspace::SetPanelVisible(const char* id, bool visible)
{
    for (const UniquePtr<EditorPanel>& panel : m_panels)
    {
        if (std::string(panel->GetDesc().id) == id)
        {
            panel->visible = visible;
        }
    }
}

void EditorWorkspace::ResetLayout()
{
    m_resetLayout = true;

    for (const UniquePtr<EditorPanel>& panel : m_panels)
    {
        panel->visible = panel->GetDesc().visibleByDefault;
    }
}

void EditorWorkspace::HandleShortcuts(EditorContext& context)
{
    // AppKit consumes the native menu's modified shortcuts during event polling.
    // Keep plain scene keys in ImGui, where text and widget focus can block them.
    HandleActionShortcuts(context.editor.GetActions(), EditorShortcutScope::Global, context.focused, m_menuBar.IsEnabled());
}

void EditorWorkspace::ProcessMenuCommands(EditorContext& context)
{
    std::string command;

    while (m_menuBar.TakeCommand(command))
    {
        if (command != actions::Exit && ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        {
            continue;
        }

        if (command.starts_with("panel:"))
        {
            for (const UniquePtr<EditorPanel>& panel : m_panels)
            {
                if (command.substr(6) == panel->GetDesc().id)
                {
                    panel->visible = !panel->visible;
                }
            }
        }
        else if (command.starts_with("recent:"))
        {
            if (context.editor.GetActions().IsEnabled(actions::Open))
            {
                context.editor.RequestLoad(command.substr(7));
            }
        }
        else if (command == "recent.clear")
        {
            context.editor.GetPreferences().recentFiles.Clear();
        }
        else
        {
            // Enabled state may have changed since AppKit displayed the item.
            context.editor.GetActions().Execute(command);
        }
    }

    UpdateNativeMenus(context);
}

void EditorWorkspace::UpdateNativeMenus(EditorContext& context)
{
    if (m_menuBar.IsEnabled())
    {
        EditorActions& registry = context.editor.GetActions();

        const bool blocked      = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

        EditorMenuItem separator;

        separator.separator = true;

        EditorMenuItem recent;

        recent.label = "Open Recent";

        recent.enabled =
            !blocked && registry.IsEnabled(actions::Open) && !context.editor.GetPreferences().recentFiles.Get().empty();

        for (const std::string& file : context.editor.GetPreferences().recentFiles.Get())
        {
            const std::filesystem::path path = std::filesystem::u8path(file);

            std::error_code error;

            const bool exists = std::filesystem::is_regular_file(path, error);

            EditorMenuItem item;

            item.id      = "recent:" + file;

            item.label   = PathToUtf8(path.filename()) + " — " + PathToUtf8(path.parent_path());

            item.tooltip = file + (exists ? "" : "\nFile not found");

            item.enabled = !blocked && exists && registry.IsEnabled(actions::Open);

            recent.children.push_back(std::move(item));
        }

        recent.children.push_back(separator);

        EditorMenuItem clear;

        clear.id      = "recent.clear";

        clear.label   = "Clear Recent";

        clear.enabled = !blocked;

        recent.children.push_back(std::move(clear));

        EditorMenuItem file;

        file.label    = "File";

        file.children = {MakeActionMenuItem(registry, actions::Open, blocked), std::move(recent), separator,
                         MakeActionMenuItem(registry, actions::Save, blocked)};

        EditorMenuItem edit;

        edit.label    = "Edit";

        edit.children = {MakeActionMenuItem(registry, actions::Undo, blocked),
                         MakeActionMenuItem(registry, actions::Redo, blocked)};

        EditorMenuItem panels;

        panels.label = "Panels";

        for (const UniquePtr<EditorPanel>& panel : m_panels)
        {
            EditorMenuItem item;

            item.id      = "panel:" + std::string(panel->GetDesc().id);

            item.label   = panel->GetDesc().title;

            item.checked = panel->visible;

            item.enabled = !blocked;

            panels.children.push_back(std::move(item));
        }

        EditorMenuItem view;

        view.label    = "View";

        view.children = {std::move(panels), MakeActionMenuItem(registry, actions::ResetLayout, blocked)};

        EditorMenuItem scene;

        scene.label    = "Scene";

        scene.children = {MakeActionMenuItem(registry, actions::FrameAll, blocked),
                          MakeActionMenuItem(registry, actions::FrameSelection, blocked)};

        EditorMenuItem help;

        help.label = "Help";

        for (const char* text : {"ZenEditor | Scene Viewer", "RMB + WASD/QE: fly | Alt+LMB: orbit | MMB: pan | Wheel: dolly",
                                 "F: frame selection | Home: frame all | Escape: release navigation"})
        {
            EditorMenuItem item;

            item.label   = text;

            item.enabled = false;

            help.children.push_back(std::move(item));
        }

        m_menuBar.Update({std::move(file), std::move(edit), std::move(view), std::move(scene), std::move(help)});
    }
}

void EditorWorkspace::DrawRecentFiles(EditorContext& context)
{
    RecentFiles& recent = context.editor.GetPreferences().recentFiles;

    // A copy: a successful load reorders the list, and Clear Recent empties it.
    const HeapVector<std::string> files = recent.Get();

    for (size_t index = 0; index < files.size(); ++index)
    {
        const std::filesystem::path path = std::filesystem::u8path(files[index]);

        std::error_code error;

        const bool exists        = std::filesystem::is_regular_file(path, error);

        const std::string name   = PathToUtf8(path.filename());

        const std::string folder = PathToUtf8(path.parent_path());

        const std::string hint   = folder.size() > 48 ? "..." + folder.substr(folder.size() - 45) : folder;

        ImGui::PushID(int(index));

        if (ImGui::MenuItem(name.c_str(), hint.c_str(), false, exists))
        {
            context.editor.RequestLoad(files[index]);
        }

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s%s", files[index].c_str(), exists ? "" : "\nFile not found");
        }

        ImGui::PopID();
    }

    ImGui::Separator();

    if (ImGui::MenuItem("Clear Recent"))
    {
        recent.Clear();
    }
}

void EditorWorkspace::DrawMenus(EditorContext& context)
{
    if (!m_menuBar.IsEnabled())
    {
        EditorActions& registry                       = context.editor.GetActions();

        const bool customTitleBar                     = context.windowChrome.IsEnabled();

        const platform::WindowTitleBarLayout titleBar = context.windowChrome.GetTitleBarLayout();

        const float scale                             = EditorScale();

        const ImVec2 padding                          = ImGui::GetStyle().FramePadding;

        // A native title-bar height keeps the platform's window buttons centered on the row.
        const float titlePadding =
            titleBar.height > 0 ? std::max(padding.y, 0.5f * (titleBar.height - ImGui::GetFontSize())) : 10 * scale;

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padding.x, customTitleBar ? titlePadding : padding.y));

        if (ImGui::BeginMainMenuBar())
        {
            // Keep clear of window buttons the platform draws at the leading edge.
            if (titleBar.leadingInset > ImGui::GetCursorPosX())
            {
                ImGui::SetCursorPosX(titleBar.leadingInset);
            }

            if (context.appIcon.value != 0)
            {
                const float size =
                    customTitleBar ? std::min(28 * scale, ImGui::GetWindowHeight() - 6 * scale) : ImGui::GetTextLineHeight();

                const float x = ImGui::GetCursorScreenPos().x;

                const float y = ImGui::GetWindowPos().y + (ImGui::GetWindowHeight() - size) * 0.5f;

                // The engine texture loader flips image rows; restore the artwork's orientation.
                ImGui::GetWindowDrawList()->AddImage(ui::ImGuiRenderer::GetTextureID(context.appIcon), ImVec2(x, y),
                                                     ImVec2(x + size, y + size), ImVec2(0, 1), ImVec2(1, 0));

                ImGui::Dummy(ImVec2(size, ImGui::GetTextLineHeight()));
            }

            if (ImGui::GetWindowWidth() > 640 * scale)
            {
                ImGui::TextColored(GetEditorPalette().brand, "ZenEditor");

                ImGui::TextDisabled("  |  ");
            }

            if (ImGui::BeginMenu("File"))
            {
                DrawActionMenuItem(registry, actions::Open);

                if (ImGui::BeginMenu("Open Recent", !context.editor.GetPreferences().recentFiles.Get().empty()))
                {
                    DrawRecentFiles(context);

                    ImGui::EndMenu();
                }

                ImGui::Separator();

                DrawActionMenuItem(registry, actions::Save);

                ImGui::Separator();

                DrawActionMenuItem(registry, actions::Exit);

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit"))
            {
                DrawActionMenuItem(registry, actions::Undo);

                DrawActionMenuItem(registry, actions::Redo);

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("View"))
            {
                if (ImGui::BeginMenu("Panels"))
                {
                    for (const UniquePtr<EditorPanel>& panel : m_panels)
                    {
                        ImGui::MenuItem(panel->GetDesc().title, nullptr, &panel->visible);
                    }

                    ImGui::EndMenu();
                }

                DrawActionMenuItem(registry, actions::ResetLayout);

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Scene"))
            {
                DrawActionMenuItem(registry, actions::FrameAll);

                DrawActionMenuItem(registry, actions::FrameSelection);

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Help"))
            {
                ImGui::TextUnformatted("ZenEditor | Scene Viewer");

                ImGui::TextUnformatted("RMB + WASD/QE: fly | Alt+LMB: orbit | MMB: pan | Wheel: dolly");

                ImGui::TextUnformatted("F: frame selection | Home: frame all | Escape: release navigation");

                ImGui::EndMenu();
            }

            const float menuEnd       = ImGui::GetCursorPosX() + 12 * scale;

            const float buttonWidth   = 46 * scale;

            const float controlsWidth = titleBar.drawsControls ? 3 * buttonWidth : 0;

            const LoadedScene* scene  = context.editor.GetScene().Get();

            if (scene != nullptr)
            {
                const std::string name = PathToUtf8(std::filesystem::u8path(scene->path).stem());

                const float position =
                    ImGui::GetWindowWidth() - controlsWidth - ImGui::CalcTextSize(name.c_str()).x - 24 * scale;

                if (position > menuEnd + 30 * scale)
                {
                    ImGui::SetCursorPosX(position);

                    ImGui::TextDisabled("%s", name.c_str());
                }
            }

            if (customTitleBar)
            {
                const float height = ImGui::GetWindowHeight();

                if (titleBar.drawsControls)
                {
                    const ImVec2 origin                 = ImGui::GetWindowPos();

                    const float start                   = origin.x + ImGui::GetWindowWidth() - controlsWidth;

                    const bool maximized                = context.windowChrome.IsMaximized();

                    const EditorWindowAction controls[] = {EditorWindowAction::Minimize, EditorWindowAction::ToggleMaximize,
                                                           EditorWindowAction::Close};

                    const char* labels[]                = {"Minimize", maximized ? "Restore" : "Maximize", "Close"};

                    for (int index = 0; index < 3; ++index)
                    {
                        if (DrawWindowButton(labels[index], controls[index], maximized,
                                             ImVec2(start + float(index) * buttonWidth, origin.y), ImVec2(buttonWidth, height)))
                        {
                            context.windowChrome.RequestAction(controls[index]);
                        }
                    }
                }

                const bool blocked = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)
                                  || ImGui::IsAnyItemActive();

                context.windowChrome.SetTitleBarRegion({menuEnd, height, controlsWidth, blocked});
            }

            ImGui::EndMainMenuBar();
        }

        ImGui::PopStyleVar();
    }
}

void EditorWorkspace::DrawToolbar(EditorContext& context)
{
    EditorActions& registry = context.editor.GetActions();

    DrawActionButton(registry, actions::Open, EditorIcon::Open);

    ImGui::SameLine();

    DrawActionButton(registry, actions::Save, EditorIcon::Save);

    EditorToolbarSeparator();

    DrawActionButton(registry, actions::Undo, EditorIcon::Undo);

    ImGui::SameLine();

    DrawActionButton(registry, actions::Redo, EditorIcon::Redo);

    if (ImGui::GetWindowWidth() - (ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x) > 280 * EditorScale())
    {
        EditorToolbarSeparator();

        DrawActionButton(registry, actions::Play, EditorIcon::Play);

        ImGui::SameLine();

        DrawActionButton(registry, actions::Pause, EditorIcon::Pause);

        ImGui::SameLine();

        DrawActionButton(registry, actions::Stop, EditorIcon::Stop);
    }

    const char* badge  = "SCENE VIEWER";

    const float badgeX = ImGui::GetWindowWidth() - ImGui::CalcTextSize(badge).x - 18 * EditorScale();

    if (badgeX > ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 35 * EditorScale())
    {
        ImGui::SameLine(badgeX);

        ImGui::AlignTextToFramePadding();

        ImGui::TextDisabled("%s", badge);
    }
}

void EditorWorkspace::DrawStatusBar(EditorContext& context)
{
    ImGui::TextDisabled("%s", context.editor.GetLoadState().IsActive() ? "Opening scene..." : "Ready");
}

void EditorWorkspace::DrawDialogs(EditorContext& context)
{
    // Fallback for window backends without a platform file picker.
    if (!context.nativeFileDialog && context.editor.TakeFileOpenRequest())
    {
        m_path[0] = 0;

        ImGui::OpenPopup("Open Scene");
    }

    if (ImGui::BeginPopupModal("Open Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Enter an absolute .gltf or .glb path:");

        ImGui::SetNextItemWidth(520 * ImGui::GetStyle().FontScaleMain);

        const bool enter = ImGui::InputText("Path", m_path, sizeof(m_path), ImGuiInputTextFlags_EnterReturnsTrue);

        if (ImGui::Button("Open") || enter)
        {
            context.editor.RequestLoad(m_path);

            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (DrawSceneLoadingDialog(context.editor.GetLoadState(), context.editor.GetError()))
    {
        context.editor.DismissLoadError();
    }
}

void EditorWorkspace::Draw(EditorContext& context)
{
    context.sceneVisible = false;

    HandleShortcuts(context);

    DrawMenus(context);

    DrawDialogs(context);

    ImGuiViewport* viewport   = ImGui::GetMainViewport();

    const float toolbarHeight = ImGui::GetFrameHeight() + 20.0f * EditorScale();

    const float statusHeight  = 32.0f * EditorScale();

    const ImGuiWindowFlags barFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;

    ImGui::SetNextWindowPos(viewport->WorkPos);

    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, toolbarHeight));

    if (ImGui::Begin("Toolbar###Toolbar", nullptr, barFlags))
    {
        DrawToolbar(context);
    }

    ImGui::End();

    const ImVec2 dockSize(viewport->WorkSize.x, std::max(1.0f, viewport->WorkSize.y - toolbarHeight - statusHeight));

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + toolbarHeight));

    ImGui::SetNextWindowSize(dockSize);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImGui::Begin("Workspace###Workspace", nullptr,
                 barFlags | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);

    ImGui::PopStyleVar();

    const ImGuiID dockspace = ImGui::GetID("ZenEditorDockspaceV2");

    if (m_resetLayout || !HasEditorLayout(dockspace))
    {
        BuildDefaultEditorLayout(dockspace, dockSize.x, dockSize.y, m_panels);

        m_resetLayout = false;
    }

    ImGui::DockSpace(dockspace);

    ImGui::End();

    for (const UniquePtr<EditorPanel>& panel : m_panels)
    {
        panel->Draw(context);
    }

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - statusHeight));

    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, statusHeight));

    if (ImGui::Begin("Status###Status", nullptr, barFlags))
    {
        DrawStatusBar(context);
    }

    ImGui::End();

    // Closing a dock panel or applying Reset Layout also updates native checks.
    UpdateNativeMenus(context);
}
} // namespace zen::editor
