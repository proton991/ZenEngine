#pragma once
#include "Runtime/RuntimeLogEntry.h"
#include "Graphics/RenderCore/V2/RenderGraph/RDGMetrics.h"
#include "Graphics/RHI/RHIGPUMemoryStats.h"

namespace zen::ui
{
enum class SessionDebugCommand
{
    None,
    Pause,
    Resume,
    Quit,
    ClearLogs
};
struct SessionDebugSnapshot
{
    bool              paused{false};
    bool              sharedScene{false};
    bool              preparingGI{false};
    uint64_t          frame{0};
    double            seconds{0};
    float             frameMilliseconds{0};
    float             sceneMilliseconds{0};
    double            graphCPUUs{0};
    uint32_t          passes{0};
    uint32_t          resources{0};
    uint32_t          width{0};
    uint32_t          height{0};
    RHIGPUMemoryStats memory;
    uint64_t          capacityBytes{0};
    uint64_t          droppedLogs{0};
    std::string       status;
    std::string       source;
    std::string       configuration;
    std::string       logPath;
};

struct SessionDebugUIState
{
    std::array<char, 128>  filter{};
    int                    severity{0};
    std::array<float, 120> history{};
    uint64_t               lastFrame{0};
    int                    nextSample{0};
};

// Diagnostics only. Scene authoring and renderer configuration live in the editor.
void DrawRuntimeMemoryStats(const RHIGPUMemoryStats& memory, uint64_t capacityBytes);

SessionDebugCommand DrawSessionDebugUI(const SessionDebugSnapshot&                snapshot,
                                       VectorView<const runtime::RuntimeLogEntry> logs,
                                       bool&                                      visible,
                                       SessionDebugUIState&                       state);
} // namespace zen::ui
