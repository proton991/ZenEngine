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
        AddShaderStage(RHIShaderStage::eVertex, "UI/ui.vert.spv");

        AddShaderStage(RHIShaderStage::eFragment, "UI/ui.frag.spv");
    }
};

// Texture and sampler array size in UI/ui.frag. One pass draws while the distinct
// images it samples fit in these slots.
constexpr uint32_t kUITextureSlots = 16;

// Matches UIDrawConstants in UI/ui.vert and UI/ui.frag.
struct UIDrawConstants
{
    float    projection[4];
    uint32_t textureSlot;
};

struct UIBatchDraw
{
    UIDrawCommand command;
    uint32_t      slot{0};
};

void RecordDraws(rc::RDGPassCmdEncoder&         encoder,
                 const HeapVector<UIBatchDraw>& draws,
                 const UIDrawConstants&         base,
                 uint32_t                       width,
                 uint32_t                       height)
{
    encoder.SetViewport(0, 0, width, height);

    for (const UIBatchDraw& draw : draws)
    {
        UIDrawConstants constants = base;

        constants.textureSlot     = draw.slot;

        encoder.SetPushConstants(constants);

        encoder.SetScissor(draw.command.minX, draw.command.minY, draw.command.maxX, draw.command.maxY);

        encoder.DrawIndexed(draw.command.count, 1, draw.command.firstIndex, draw.command.vertexOffset, 0);
    }
}

uint32_t FindSlot(const HeapVector<UITextureHandle>& slots, UITextureHandle handle)
{
    uint32_t result = uint32_t(slots.size());

    for (uint32_t index = 0; index < slots.size(); ++index)
    {
        result = result == slots.size() && slots[index] == handle ? index : result;
    }

    return result;
}
} // namespace

UIRenderer::UIRenderer(rc::RenderDevice& device) : m_device(device) {}

bool UIRenderer::Init()
{
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

    if (valid)
    {
        m_frames.resize(GRenderFrameState.GetNumFramesInFlight());
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

    for (uint32_t index = 0; index < m_textures.size(); ++index)
    {
        UnregisterTexture({(uint64_t(m_textures[index].generation) << 32) | (uint64_t(index) + 1)});
    }
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

UITextureHandle UIRenderer::RegisterTexture(RHITexture* texture, RHISampler* sampler)
{
    UITextureHandle handle;

    if (texture != nullptr && sampler != nullptr)
    {
        uint32_t index = 0;

        while (index < m_textures.size() && (m_textures[index].texture || m_textures[index].generation == UINT32_MAX))
        {
            ++index;
        }

        if (index == m_textures.size())
        {
            m_textures.emplace_back();
        }

        TextureSlot& slot = m_textures[index];

        slot.texture      = texture;

        slot.sampler      = sampler;

        handle.value      = (uint64_t(slot.generation) << 32) | (uint64_t(index) + 1);
    }

    return handle;
}

bool UIRenderer::IsTextureValid(UITextureHandle handle) const
{
    const uint32_t index = uint32_t(handle.value) - 1;

    return index < m_textures.size() && m_textures[index].texture
        && m_textures[index].generation == uint32_t(handle.value >> 32);
}

void UIRenderer::UnregisterTexture(UITextureHandle handle)
{
    if (IsTextureValid(handle))
    {
        TextureSlot& slot = m_textures[uint32_t(handle.value) - 1];

        m_device.DeferReleaseResource(slot.texture.Detach());

        m_device.DeferReleaseResource(slot.sampler.Detach());

        ++slot.generation;
    }
}

bool UIRenderer::BuildRenderGraph(rc::RenderGraph& graph, const UIRenderTarget& target, const UIDrawPacket& packet)
{
    const uint32_t width  = target.width;

    const uint32_t height = target.height;

    bool valid            = target.color != nullptr && !m_frames.empty() && ValidateUIDrawPacket(packet, width, height);

    for (const UIDrawCommand& command : packet.commands)
    {
        valid = valid && IsTextureValid(command.texture);
    }

    if (valid && !packet.commands.empty())
    {
        FrameBuffers& frame        = m_frames[rc::ToIndex(GRenderFrameState.GetFrameSlot())];

        const uint32_t vertexBytes = static_cast<uint32_t>(packet.vertices.size() * sizeof(UIVertex));

        const uint32_t indexBytes  = static_cast<uint32_t>(packet.indices.size() * sizeof(uint32_t));

        valid = GrowBuffer(frame.vertices, frame.vertexCapacity, vertexBytes, RHIBufferUsageFlagBits::eVertexBuffer)
             && GrowBuffer(frame.indices, frame.indexCapacity, indexBytes, RHIBufferUsageFlagBits::eIndexBuffer);

        if (valid)
        {
            m_device.UpdateBuffer(frame.vertices, vertexBytes, reinterpret_cast<const uint8_t*>(packet.vertices.data()));

            m_device.UpdateBuffer(frame.indices, indexBytes, reinterpret_cast<const uint8_t*>(packet.indices.data()));

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

            UIDrawConstants constants{};

            std::copy(std::begin(packet.projection), std::end(packet.projection), constants.projection);

            // Commands keep their order. A pass takes consecutive commands until a new image
            // would exceed the texture slots; every sampled image is declared to the graph.
            size_t first = 0;

            while (first < packet.commands.size())
            {
                HeapVector<UITextureHandle> slots;

                HeapVector<UIBatchDraw> draws;

                bool full = false;

                while (first < packet.commands.size() && !full)
                {
                    const UIDrawCommand& command = packet.commands[first];

                    const uint32_t slot          = FindSlot(slots, command.texture);

                    full                         = slot == slots.size() && slots.size() == kUITextureSlots;

                    if (!full)
                    {
                        if (slot == slots.size())
                        {
                            slots.push_back(command.texture);
                        }

                        draws.push_back({command, slot});

                        ++first;
                    }
                }

                rc::RDGGraphicsPassDesc pass;

                pass.SetShaderProgramName("UIRenderSP");

                pass.SetPassTag("UI");

                pass.SetPipelineStates(states);

                pass.SetRenderArea(0, 0, width, height);

                pass.AddColorOutput(target.color, RHIRenderTargetLoadOp::eLoad, RHIRenderTargetStoreOp::eStore);

                pass.BindVertexBuffer(frame.vertices);

                pass.BindIndexBuffer(frame.indices, DataFormat::eR32UInt);

                HeapVector<RHITextureView*> views;

                for (uint32_t slot = 0; slot < slots.size(); ++slot)
                {
                    const TextureSlot& texture = m_textures[uint32_t(slots[slot].value) - 1];

                    views.push_back(texture.texture->GetDefaultView());

                    pass.BindSampler("uSamplers", texture.sampler.Get(), slot);
                }

                pass.BindSeparateTexture("uTextures", views);

                graph.AddGraphicsPass(std::move(pass))
                    .RecordPassCommands([draws = std::move(draws), constants, width, height](rc::RDGPassCmdEncoder& encoder) {
                        RecordDraws(encoder, draws, constants, width, height);
                    });
            }
        }
    }

    if (!valid)
    {
        LOGE("UI draw packet rejected: stale texture, invalid geometry or allocation failure");
    }

    return valid;
}
} // namespace zen::ui
