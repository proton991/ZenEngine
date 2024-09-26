#pragma once
#include "VulkanCommon.h"
#include "Graphics/RHI/RHICommon.h"

namespace zen::rhi
{
class VulkanRHI;

class VulkanRenderPassBuilder
{
public:
    VulkanRenderPassBuilder() = default;

    VkRenderPassCreateInfo BuildRenderPassCreateInfo(const RenderPassLayout& renderPassLayout);

private:
    // TODO: Support resolve attachments
    VkAttachmentDescription m_attachmentDescriptions[MAX_COLOR_ATTACHMENT_COUNT + 1]{};
    VkAttachmentReference m_colorAttachmentReference[MAX_COLOR_ATTACHMENT_COUNT]{};
    VkAttachmentReference m_depthStencilReference{};
    VkSubpassDescription m_subpasses[MAX_SUBPASS_COUNT]{};
    // only add dependency between inner subpasses, no dependency from outer or to outer
    VkSubpassDependency m_subpassDeps[MAX_SUBPASS_COUNT - 1]{};

    // counters
    uint32_t m_numAttachments{0};
    uint32_t m_numSubpassDeps{0};
    uint32_t m_numColorAttachmentRefs{0};
};

class VulkanFramebuffer
{
public:
    VulkanFramebuffer(VulkanRHI* vkRHI, VkRenderPass renderPass, const FramebufferInfo& fbInfo);

    VkFramebuffer GetVkHandle() const
    {
        return m_framebuffer;
    }

    VkRect2D GetRenderArea() const
    {
        return m_renderArea;
    }

private:
    VkFramebuffer m_framebuffer{VK_NULL_HANDLE};
    VkRect2D m_renderArea{};
};
} // namespace zen::rhi