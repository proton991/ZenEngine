#pragma once
#include "Graphics/RenderCore/V2/Renderer/RenderOverlay.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Utils/UniquePtr.h"
#include <string>

namespace zen::editor
{
struct EditorOptions
{
    std::string      scene;
    std::string      environment;
    std::string      settingsDirectory;
    std::string      capture;
    std::string      sceneCapture;
    rc::DebugOutput  debugOutput{rc::DebugOutput::eFinal};
    bool             debugSpecified{false};
    uint32_t         frames{0};
    float            scale{0};
    bool             threaded{true};
    bool             smokeTest{false};
    bool             hidden{false};
    bool             windowed{false};
    rc::RenderOption mode{rc::RenderOption::ePBR};
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
