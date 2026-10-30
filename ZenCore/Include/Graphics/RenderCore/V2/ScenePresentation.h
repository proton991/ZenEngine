#pragma once
#include "Graphics/RenderCore/V2/RenderView.h"
#include <string>

namespace zen::rc
{
class RenderDevice;
class RenderGraph;

// Allocation is transactional; failed resize preserves the last complete view.
// Callers choose extent policy and retain the view while their scene is paused.
bool ResizeSceneView(RenderDevice& device, RenderView& view, uint32_t width, uint32_t height);
void DestroySceneView(RenderDevice& device, RenderView& view);
// Explicit diagnostic readback waits for GPU completion; writes scene-only PPM.
bool CaptureSceneFrame(RenderDevice& device, RHITexture* source, const std::string& path);

// One offscreen target can be borrowed by different presentation windows. Its
// revision follows allocation changes regardless of which window requested them.
// Destroy before the device; like SceneRendering, teardown is explicit.
class SceneRenderTarget
{
public:
    explicit SceneRenderTarget(RenderDevice& device) : m_device(device) {}
    bool              Resize(uint32_t width, uint32_t height);
    void              Destroy();
    const RenderView& GetView() const
    {
        return m_view;
    }
    uint64_t GetRevision() const
    {
        return m_revision;
    }
    bool HasCompletedImage() const
    {
        return m_completed;
    }
    void OnSubmitted(bool succeeded)
    {
        m_completed = m_completed || succeeded;
    }

private:
    RenderDevice& m_device;
    RenderView    m_view;
    uint64_t      m_revision{0};
    bool          m_completed{false};
};

// SDR scene-to-window presentation, independent of any UI library.
class SceneCompositor
{
public:
    bool Init(RenderDevice& device);
    bool BuildRenderGraph(RenderGraph& graph, const RenderView& destination, const RenderView& scene);

private:
    RHISampler* m_sampler{nullptr}; // Device sampler cache owns it.
};
} // namespace zen::rc
