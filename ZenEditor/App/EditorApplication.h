#pragma once
#include "Graphics/RenderCore/V2/Renderer/RenderOverlay.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Utils/UniquePtr.h"
#include <optional>
#include <string>

namespace zen::editor
{
struct EditorOptions
{
    std::string scene;
    std::string environment;
    std::string settingsDirectory;
    std::string capture;
    std::string sceneCapture;
    // Opens the Render Graph window and captures it on the final frame.
    std::string      graphCapture;
    std::string      graphSelect;
    rc::DebugOutput  debugOutput{rc::DebugOutput::eFinal};
    bool             debugSpecified{false};
    uint32_t         frames{0};
    float            scale{0};
    bool             threaded{true};
    bool             smokeTest{false};
    bool             hidden{false};
    bool             windowed{false};
    rc::RenderOption mode{rc::RenderOption::eVoxelGI}; // PBR + Global illumination.

    std::optional<bool> validation;
};

class EditorApplication
{
public:
    explicit EditorApplication(EditorOptions options);

    ~EditorApplication();

    int Run();

private:
    class State;

    UniquePtr<State> m_state;
};
} // namespace zen::editor
