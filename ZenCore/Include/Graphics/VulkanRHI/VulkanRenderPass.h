#pragma once
#include "VulkanCommon.h"
#include "Graphics/RHI/RHICommon.h"

namespace zen
{
class VulkanRHI;

class VulkanRenderPassBuilder
{
public:
    VulkanRenderPassBuilder() = default;

    VkRenderPassCreateInfo BuildRenderPassCreateInfo(const RHIRenderPassLayout& renderPassLayout);

    VkRenderPassCreateInfo BuildRenderPassCreateInfo(const RHIRenderingLayout* pLayout);

private:
    // TODO: Support resolve attachments
    VkAttachmentDescription m_attachmentDescriptions[MAX_NUM_COLOR_ATTACHMENTS + 1]{};
    VkAttachmentReference m_colorAttachmentReference[MAX_NUM_COLOR_ATTACHMENTS]{};
    VkAttachmentReference m_depthStencilReference{};
    VkSubpassDescription m_subpasses[MAX_NUM_SUBPASSES]{};
    // only add dependency between inner subpasses, no dependency from outer or to outer
    VkSubpassDependency m_subpassDeps[MAX_NUM_SUBPASSES - 1]{};

    // counters
    uint32_t m_numAttachments{0};
    uint32_t m_numSubpassDeps{0};
    uint32_t m_numColorAttachmentRefs{0};
};

class VulkanFramebuffer
{
public:
    VulkanFramebuffer(VulkanRHI* vkRHI, VkRenderPass renderPass, const RHIFramebufferInfo& fbInfo);

    VkFramebuffer GetVkHandle() const
    {
        return m_framebuffer;
    }

    VkRenderPass GetVkRenderPass() const
    {
        return m_renderPass;
    }

    auto GetWidth() const
    {
        return m_width;
    }

    auto GetHeight() const
    {
        return m_height;
    }

private:
    VulkanRHI* m_vkRHI;
    VkFramebuffer m_framebuffer{VK_NULL_HANDLE};
    VkRenderPass m_renderPass{VK_NULL_HANDLE};
    uint32_t m_width{0};
    uint32_t m_height{0};
    uint32_t m_layers{1};
};
} // namespace zen