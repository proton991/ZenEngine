#include "UI/UIRenderer.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include <algorithm>
#include <cstddef>

namespace zen::ui
{
namespace
{
class UIShaderProgram : public rc::ShaderProgram
{
public:
    explicit UIShaderProgram(rc::RenderDevice& device) : ShaderProgram(&device, "UIRenderSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "UI/imgui.vert.spv");

        AddShaderStage(RHIShaderStage::eFragment, "UI/imgui.frag.spv");
    }
};

struct UIProjection
{
    float values[4];
};

void RecordDraws(rc::RDGPassCmdEncoder&           encoder,
                 const HeapVector<UIDrawCommand>& commands,
                 const UIProjection&              projection,
                 uint32_t                         width,
                 uint32_t                         height)
{
    encoder.SetViewport(0, 0, width, height);

    encoder.SetPushConstants(projection);

    for (const UIDrawCommand& command : commands)
    {
        encoder.SetScissor(command.minX, command.minY, command.maxX, command.maxY);

        encoder.DrawIndexed(command.count, 1, command.firstIndex, command.vertexOffset, 0);
    }
}
} // namespace

UIRenderer::UIRenderer(rc::RenderDevice& device) : m_device(device) {}

bool UIRenderer::Init(ImFontAtlas& fonts)
{
    static_assert(sizeof(ImDrawVert) == 20 && offsetof(ImDrawVert, pos) == 0 && offsetof(ImDrawVert, uv) == 8
                  && offsetof(ImDrawVert, col) == 16);

    static_assert(IM_COL32_R_SHIFT == 0 && IM_COL32_G_SHIFT == 8 && IM_COL32_B_SHIFT == 16 && IM_COL32_A_SHIFT == 24);

    rc::ShaderProgramManager& programs = rc::ShaderProgramManager::GetInstance();

    bool valid                         = true;

    if (programs.RequestShaderProgram("UIRenderSP") == nullptr)
    {
        UIShaderProgram* program = ZEN_NEW() UIShaderProgram(m_device);

        valid                    = program->Init();

        if (valid)
        {
            programs.StoreProgram(program);
        }
        else
        {
            ZEN_DELETE(program);
        }
    }

    if (valid && m_fontTexture == nullptr)
    {
        unsigned char* pixels = nullptr;

        int width             = 0;

        int height            = 0;

        fonts.GetTexDataAsRGBA32(&pixels, &width, &height);

        valid = pixels != nullptr && width > 0 && height > 0 && uint64_t(width) * uint64_t(height) * 4 <= UINT32_MAX;

        if (valid)
        {
            rc::TextureFormat format;

            format.width                 = static_cast<uint32_t>(width);

            format.height                = static_cast<uint32_t>(height);

            format.depth                 = 1;

            format.format                = DataFormat::eR8G8B8A8UNORM;

            m_fontTexture                = m_device.CreateTextureSampled(format, {.copyUsage = true}, "UIFont");

            RHISamplerCreateInfo sampler = RHISamplerCreateInfo::CreateLinearRepeat();

            sampler.repeatU = sampler.repeatV = sampler.repeatW = RHISamplerRepeatMode::eClampToEdge;

            m_sampler                                           = m_device.CreateSampler(sampler);

            valid                                               = m_fontTexture != nullptr && m_sampler != nullptr;
        }

        if (valid)
        {
            RHIBufferTextureCopyRegion region{};

            region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

            region.textureSubresources.layerCount = 1;

            region.textureSize                    = {width, height, 1};

            m_device.UpdateTexture(m_fontTexture, MakeVecView(&region, 1), static_cast<uint32_t>(width * height * 4), pixels);

            m_fontId = static_cast<ImTextureID>(m_fontTexture->GetStableId());

            fonts.SetTexID(m_fontId);

            m_frames.resize(GRenderFrameState.GetNumFramesInFlight());
        }
        else
        {
            Destroy();
        }
    }

    return valid;
}

void UIRenderer::Destroy()
{
    for (FrameBuffers& frame : m_frames)
    {
        m_device.DestroyBuffer(frame.vertices);

        m_device.DestroyBuffer(frame.indices);
    }

    m_frames.clear();

    m_device.DestroyTexture(m_fontTexture);

    m_fontTexture = nullptr;

    // Samplers belong to RenderDevice's sampler cache.
    m_sampler = nullptr;

    m_fontId  = ImTextureID_Invalid;
}

bool UIRenderer::GrowBuffer(RHIBuffer*& buffer, uint32_t& capacity, uint32_t required, RHIBufferUsageFlagBits usage)
{
    bool valid = true;

    if (required > capacity)
    {
        const uint64_t grown       = std::max(uint64_t(required), std::max(uint64_t(capacity) * 2, uint64_t(65536)));

        const uint32_t newCapacity = static_cast<uint32_t>(std::min(grown, uint64_t(UINT32_MAX & ~3u)));

        RHIBufferCreateInfo info;

        info.size         = newCapacity;

        info.allocateType = RHIBufferAllocateType::eGPU;

        info.usageFlags.SetFlags(usage, RHIBufferUsageFlagBits::eTransferDstBuffer);

        info.tag               = "UIFrameGeometry";

        RHIBuffer* replacement = m_device.CreateBuffer(info);

        valid                  = replacement != nullptr && newCapacity >= required;

        if (valid)
        {
            // RDG currently tracks whole-buffer initialization. Define spare capacity
            // once at growth so later partial geometry updates preserve known contents.
            HeapVector<uint8_t> initial(newCapacity);

            std::memset(initial.data(), 0, initial.size());

            m_device.UpdateBuffer(replacement, newCapacity, initial.data());

            m_device.DestroyBuffer(buffer);

            buffer   = replacement;

            capacity = newCapacity;
        }
        else
        {
            m_device.DestroyBuffer(replacement);
        }
    }

    return valid;
}

bool UIRenderer::BuildRenderGraph(rc::RenderGraph& graph, RHIViewport& viewport, const ImDrawData& data)
{
    UIDrawPacket packet;

    const uint32_t width  = viewport.GetWidth();

    const uint32_t height = viewport.GetHeight();

    bool valid            = m_fontTexture != nullptr && BuildUIDrawPacket(data, m_fontId, width, height, packet);

    if (valid && !packet.commands.empty())
    {
        FrameBuffers& frame = m_frames[rc::ToIndex(GRenderFrameState.GetFrameSlot())];

        valid               = GrowBuffer(frame.vertices, frame.vertexCapacity, static_cast<uint32_t>(packet.vertices.size()),
                                         RHIBufferUsageFlagBits::eVertexBuffer)
             && GrowBuffer(frame.indices, frame.indexCapacity, static_cast<uint32_t>(packet.indices.size()),
                           RHIBufferUsageFlagBits::eIndexBuffer);

        if (valid)
        {
            m_device.UpdateBuffer(frame.vertices, static_cast<uint32_t>(packet.vertices.size()), packet.vertices.data());

            m_device.UpdateBuffer(frame.indices, static_cast<uint32_t>(packet.indices.size()), packet.indices.data());

            RHIGfxPipelineStates states{};

            states.primitiveType               = RHIDrawPrimitiveType::eTriangleList;

            states.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;

            states.depthStencilState = RHIGfxPipelineDepthStencilState::Create(false, false, RHIDepthCompareOperator::eAlways);

            states.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

            RHIGfxPipelineColorBlendState::Attachment blend;

            blend.enableBlend         = true;

            blend.srcColorBlendFactor = RHIBlendFactor::eSrcAlpha;

            blend.dstColorBlendFactor = RHIBlendFactor::eOneMinusSrcAlpha;

            blend.srcAlphaBlendFactor = RHIBlendFactor::eOne;

            blend.dstAlphaBlendFactor = RHIBlendFactor::eOneMinusSrcAlpha;

            blend.colorWriteMask.SetFlags(RHIColorComponent::eRed, RHIColorComponent::eGreen, RHIColorComponent::eBlue,
                                          RHIColorComponent::eAlpha);

            states.colorBlendState.AddAttachment(blend);

            rc::RDGGraphicsPassDesc pass;

            pass.SetShaderProgramName("UIRenderSP");

            pass.SetPassTag("RuntimeUI");

            pass.SetPipelineStates(states);

            pass.SetRenderArea(0, 0, width, height);

            pass.AddColorOutput(viewport.GetColorBackBuffer(), RHIRenderTargetLoadOp::eLoad, RHIRenderTargetStoreOp::eStore);

            pass.BindVertexBuffer(frame.vertices);

            pass.BindIndexBuffer(frame.indices, sizeof(ImDrawIdx) == 2 ? DataFormat::eR16UInt : DataFormat::eR32UInt);

            pass.BindSampledTexture("uFont", m_sampler, m_fontTexture->GetDefaultView());

            UIProjection projection;

            std::copy(std::begin(packet.projection), std::end(packet.projection), projection.values);

            graph.AddGraphicsPass(std::move(pass))
                .RecordPassCommands(
                    [commands = std::move(packet.commands), projection, width, height](rc::RDGPassCmdEncoder& encoder) {
                        RecordDraws(encoder, commands, projection, width, height);
                    });
        }
    }

    if (!valid)
    {
        LOGE("UI draw packet rejected: unsupported texture/callback, invalid data, or allocation failure");
    }

    return valid;
}
} // namespace zen::ui
