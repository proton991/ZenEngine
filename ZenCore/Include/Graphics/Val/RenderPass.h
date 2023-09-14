#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include "DeviceObject.h"

namespace zen::val
{
class RenderPassBuilder;

struct SubpassInfo
{
    SubpassInfo(const std::vector<uint32_t>& colors, const std::vector<uint32_t>& inputs, const uint32_t depthStencil = UINT32_MAX);
    std::vector<VkAttachmentReference> colorRefs;
    std::vector<VkAttachmentReference> inputRefs;
    bool                               hasDepthStencilRef{false};
    VkAttachmentReference              depthStencilRef;
};

class RenderPass : public DeviceObject<VkRenderPass, VK_OBJECT_TYPE_RENDER_PASS>
{
public:
    ~RenderPass();

private:
    RenderPass(Device& device, const std::vector<VkAttachmentDescription>& attachments, const std::vector<SubpassInfo>& subpassInfos, const std::vector<VkSubpassDependency>& subpassDeps);
    friend class RenderPassBuilder;
};


class RenderPassBuilder
{
public:
    struct SubpassDepInfo
    {
        uint32_t inputAttRead{0};
        uint32_t colorAttReadWrite{0};
        uint32_t depthAttWrite{0};
        uint32_t depthAttRead{0};
        bool     externDepthDep{false};
        bool     externColorDep{true};
    };
    explicit RenderPassBuilder(Device& device) :
        m_device(device) {}

    RenderPassBuilder& AddPresentAttachment(VkFormat format);
    RenderPassBuilder& AddColorAttachment(VkFormat format, bool clear);
    RenderPassBuilder& AddDepthStencilAttachment(VkFormat format);

    RenderPassBuilder& AddSubpass(const std::vector<uint32_t>& colorRefs,
                                  const std::vector<uint32_t>& inputRefs,
                                  uint32_t                     depthStencilRef);

    RenderPassBuilder& SetSubpassDeps(const SubpassDepInfo& info);

    RenderPass* Build();

private:
    Device& m_device;

    std::vector<VkAttachmentDescription> m_attachments;
    std::vector<SubpassInfo>             m_subpassInfos;
    std::vector<VkSubpassDependency>     m_subpassDeps;
};
} // namespace zen::val