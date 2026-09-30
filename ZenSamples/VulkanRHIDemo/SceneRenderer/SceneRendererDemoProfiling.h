#pragma once
#include "Templates/HeapVector.h"
#include "Utils/UniquePtr.h"
#include <string>

namespace zen
{
class RHIViewport;
namespace rc
{
class RenderDevice;
class RenderScene;
class RDGMetrics;
} // namespace rc

struct DemoProfilingOptions
{
    std::string prefix;
    HeapVector<std::string> arguments;
    uint32_t frames{0};
    uint32_t warmup{0};
    uint32_t giStartFrame{0};
    bool fixedStep{false};
    bool vsync{true};
};

// Owns bounded copies of deferred metrics. No file writes or GPU waits occur in RecordFrame.
class SceneRendererProfiling
{
public:
    SceneRendererProfiling(rc::RenderDevice& device, const DemoProfilingOptions& options);

    ~SceneRendererProfiling();

    void BeginFrame(rc::RenderDevice& device, uint32_t localFrame, bool warmup);

    void RecordFrame(rc::RenderDevice& device, RHIViewport& viewport, double cpuUs, bool succeeded);

    void Stop(rc::RenderDevice& device, const rc::RenderScene* scene, RHIViewport& viewport);

    // Called after the application's ordinary shutdown drain, before destroying resources.

    // Call after ordinary device shutdown has drained native GPU work.
    bool Export(rc::RDGMetrics& metrics, bool runSucceeded);

private:
    struct State;
    UniquePtr<State> m_state;
};
} // namespace zen
