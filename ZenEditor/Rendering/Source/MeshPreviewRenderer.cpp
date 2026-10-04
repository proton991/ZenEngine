#include "Editor/Rendering/MeshPreviewRenderer.h"
#include "EditorShaders.h"
#include <algorithm>

namespace zen::editor
{
namespace
{
// Matches MeshPreviewConstants in Editor/mesh_preview.vert and .frag; plain arrays keep
// the size equal to the shader block's 84 bytes.
struct MeshPreviewConstants
{
    float    viewProjection[16];
    float    eye[4];
    uint32_t mode;
};

// The fragment shader's wireframe mode, after the MeshPreviewShading values.
constexpr uint32_t kWireframeMode = 3;

struct PreviewDraw
{
    uint32_t firstIndex{0};
    uint32_t indexCount{0};
};

void RecordPreview(rc::RDGPassCmdEncoder& encoder, const HeapVector<PreviewDraw>& draws, const MeshPreviewConstants& constants)
{
    encoder.SetPushConstants(constants);

    for (const PreviewDraw& draw : draws)
    {
        encoder.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
    }
}

void AddPreviewPass(rc::RenderGraph&               graph,
                    const rc::RenderScene&         scene,
                    const RHIGfxPipelineStates&    states,
                    RHITexture*                    color,
                    RHITexture*                    depth,
                    RHIRenderTargetLoadOp          depthLoad,
                    const HeapVector<PreviewDraw>& draws,
                    const MeshPreviewConstants&    constants)
{
    rc::RDGGraphicsPassDesc pass;

    pass.SetShaderProgramName("EditorMeshPreviewSP");

    pass.SetPassTag("EditorMeshPreview");

    pass.SetPipelineStates(states);

    pass.SetRenderArea(0, 0, color->GetWidth(), color->GetHeight());

    pass.AddColorOutput(color, RHIRenderTargetLoadOp::eLoad, RHIRenderTargetStoreOp::eStore);

    pass.AddDepthStencilOutput(depth, depthLoad, RHIRenderTargetStoreOp::eStore);

    pass.BindVertexBuffer(scene.GetVertexBuffer());

    pass.BindIndexBuffer(scene.GetIndexBuffer());

    graph.AddGraphicsPass(std::move(pass)).RecordPassCommands([draws, constants](rc::RDGPassCmdEncoder& encoder) {
        RecordPreview(encoder, draws, constants);
    });
}
} // namespace

MeshPreviewRenderer::MeshPreviewRenderer(rc::RenderDevice& device, const EditorViewport& viewport) :
    m_device(device), m_viewport(viewport)
{}

bool MeshPreviewRenderer::Init()
{
    RHISamplerCreateInfo sampler = RHISamplerCreateInfo::CreateLinearRepeat();

    sampler.repeatU = sampler.repeatV = RHISamplerRepeatMode::eClampToEdge;

    m_sampler                         = m_device.CreateSampler(sampler);

    return m_sampler != nullptr
        && RegisterEditorShader(m_device, "EditorMeshPreviewSP", "Editor/mesh_preview.vert.spv",
                                "Editor/mesh_preview.frag.spv");
}

void MeshPreviewRenderer::Destroy()
{
    Cancel();

    m_device.DestroyTexture(m_color);

    m_device.DestroyTexture(m_depth);

    m_color  = nullptr;

    m_depth  = nullptr;

    m_width  = 0;

    m_height = 0;
}

bool MeshPreviewRenderer::Resize(uint32_t width, uint32_t height)
{
    width      = std::min(4096u, (width + 7u) & ~7u);

    height     = std::min(4096u, (height + 7u) & ~7u);

    bool valid = width > 0 && height > 0;

    if (valid && (m_width != width || m_height != height))
    {
        rc::TextureFormat format;

        format.width      = width;

        format.height     = height;

        format.depth      = 1;

        format.format     = DataFormat::eR8G8B8A8UNORM;

        RHITexture* color = m_device.CreateTextureColorRT(format, {.copyUsage = true}, "EditorMeshPreviewColor");

        format.format     = DataFormat::eD32SFloat;

        RHITexture* depth = m_device.CreateTextureDepthStencilRT(format, {}, "EditorMeshPreviewDepth");

        valid             = color != nullptr && depth != nullptr;

        if (valid)
        {
            m_device.DestroyTexture(m_color);

            m_device.DestroyTexture(m_depth);

            m_color  = color;

            m_depth  = depth;

            m_width  = width;

            m_height = height;

            ++m_revision;
        }
        else
        {
            m_device.DestroyTexture(color);

            m_device.DestroyTexture(depth);
        }
    }

    return valid;
}

void MeshPreviewRenderer::Request(const MeshPreviewFrame& frame)
{
    m_frame     = frame;

    m_requested = frame.mesh != nullptr;
}

void MeshPreviewRenderer::Cancel()
{
    m_frame     = {};

    m_requested = false;
}

void MeshPreviewRenderer::BuildGraph(rc::RenderGraph& graph)
{
    const rc::RenderScene* scene = m_viewport.GetRenderScene();

    if (m_requested && scene != nullptr && m_color != nullptr)
    {
        HeapVector<PreviewDraw> draws;

        for (const sg::SubMesh* primitive : m_frame.mesh->GetSubMeshes())
        {
            if (primitive->topology == sg::MeshTopology::Triangles && primitive->GetIndexCount() > 0)
            {
                draws.push_back({primitive->GetFirstIndex(), primitive->GetIndexCount()});
            }
        }

        MeshPreviewConstants constants{};

        std::copy(&m_frame.viewProjection[0][0], &m_frame.viewProjection[0][0] + 16, constants.viewProjection);

        std::copy(&m_frame.eye[0], &m_frame.eye[0] + 3, constants.eye);

        constants.mode = uint32_t(m_frame.settings.shading);

        graph.AddTransferPass("EditorMeshPreviewClear").ClearTexture(m_color, Color(0.105f, 0.125f, 0.15f, 1.0f));

        const bool wireframe = m_frame.settings.wireframe && SupportsWireframe();

        RHIGfxPipelineStates states{};

        states.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;

        // Pushes filled triangles back so the wireframe pass draws over their edges.
        states.rasterizationState.enableDepthBias         = wireframe;

        states.rasterizationState.depthBiasConstantFactor = 1.0f;

        states.rasterizationState.depthBiasSlopeFactor    = 1.0f;

        states.depthStencilState = RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);

        states.colorBlendState.AddAttachments(1);

        AddPreviewPass(graph, *scene, states, m_color, m_depth, RHIRenderTargetLoadOp::eClear, draws, constants);

        if (wireframe)
        {
            states.rasterizationState.wireframe       = true;

            states.rasterizationState.enableDepthBias = false;

            states.depthStencilState =
                RHIGfxPipelineDepthStencilState::Create(true, false, RHIDepthCompareOperator::eLessOrEqual);

            constants.mode = kWireframeMode;

            AddPreviewPass(graph, *scene, states, m_color, m_depth, RHIRenderTargetLoadOp::eLoad, draws, constants);
        }
    }

    m_requested = false;
}

RHITexture* MeshPreviewRenderer::GetImage() const
{
    return m_color;
}

uint32_t MeshPreviewRenderer::GetWidth() const
{
    return m_width;
}

uint32_t MeshPreviewRenderer::GetHeight() const
{
    return m_height;
}

uint64_t MeshPreviewRenderer::GetImageRevision() const
{
    return m_revision;
}

RHISampler* MeshPreviewRenderer::GetSampler() const
{
    return m_sampler;
}

bool MeshPreviewRenderer::SupportsWireframe() const
{
    return m_device.GetGPUInfo().supportFillModeNonSolid;
}
} // namespace zen::editor
