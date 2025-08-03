#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanRenderPass.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"

namespace zen::rhi
{
VkRenderPassCreateInfo VulkanRenderPassBuilder::BuildRenderPassCreateInfo(
    const RenderPassLayout& renderPassLayout)
{
    const auto& colorRTs = renderPassLayout.GetColorRenderTargets();

    // attachment descriptions
    for (uint32_t i = 0; i < renderPassLayout.GetNumColorRenderTargets(); i++)
    {
        VkAttachmentDescription& description = m_attachmentDescriptions[i];
        // set field values
        description.format  = ToVkFormat(colorRTs[i].format);
        description.samples = ToVkSampleCountFlagBits(renderPassLayout.GetNumSamples());
        description.loadOp  = ToVkAttachmentLoadOp(renderPassLayout.GetColorRenderTargetLoadOp());
        description.storeOp = ToVkAttachmentStoreOp(renderPassLayout.GetColorRenderTargetStoreOp());
        description.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        description.initialLayout =
            renderPassLayout.GetColorRenderTargetLoadOp() == RenderTargetLoadOp::eClear ?
            VK_IMAGE_LAYOUT_UNDEFINED :
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        description.finalLayout = ToVkImageLayout(TextureUsageToLayout(colorRTs[i].usage));

        VkAttachmentReference& reference = m_colorAttachmentReference[m_numColorAttachmentRefs];
        reference.attachment             = i;
        reference.layout                 = description.finalLayout;

        m_numColorAttachmentRefs++;
        m_numAttachments++;
    }

    if (renderPassLayout.HasDepthStencilRenderTarget())
    {
        VkAttachmentDescription& description = m_attachmentDescriptions[m_numAttachments];
        description.format  = ToVkFormat(renderPassLayout.GetDepthStencilRenderTarget().format);
        description.samples = ToVkSampleCountFlagBits(renderPassLayout.GetNumSamples());
        description.loadOp =
            ToVkAttachmentLoadOp(renderPassLayout.GetDepthStencilRenderTargetLoadOp());
        description.storeOp =
            ToVkAttachmentStoreOp(renderPassLayout.GetDepthStencilRenderTargetStoreOp());
        description.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        description.initialLayout =
            renderPassLayout.GetDepthStencilRenderTargetLoadOp() == RenderTargetLoadOp::eClear ?
            VK_IMAGE_LAYOUT_UNDEFINED :
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        description.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        m_depthStencilReference.attachment = m_numAttachments;
        m_depthStencilReference.layout     = description.finalLayout;

        m_numAttachments++;
    }

    // only 1 subpass
    VkSubpassDescription& subpassDescription = m_subpasses[0];
    subpassDescription.pipelineBindPoint     = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpassDescription.colorAttachmentCount  = m_numColorAttachmentRefs;
    subpassDescription.pColorAttachments     = m_colorAttachmentReference;
    if (renderPassLayout.HasDepthStencilRenderTarget())
    {
        subpassDescription.pDepthStencilAttachment = &m_depthStencilReference;
    }


    VkRenderPassCreateInfo renderPassCI;
    InitVkStruct(renderPassCI, VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    renderPassCI.attachmentCount = m_numAttachments;
    renderPassCI.pAttachments    = m_attachmentDescriptions;
    renderPassCI.subpassCount    = 1;
    renderPassCI.pSubpasses      = m_subpasses;
    renderPassCI.dependencyCount = m_numSubpassDeps;
    renderPassCI.pDependencies   = m_numSubpassDeps == 0 ? nullptr : m_subpassDeps;

    return renderPassCI;
}

RenderPassHandle VulkanRHI::CreateRenderPass(const RenderPassLayout& renderPassLayout)
{
    VulkanRenderPassBuilder builder;
    VkRenderPassCreateInfo renderPassCI = builder.BuildRenderPassCreateInfo(renderPassLayout);
    VkRenderPass renderPass{VK_NULL_HANDLE};
    VKCHECK(vkCreateRenderPass(GetVkDevice(), &renderPassCI, nullptr, &renderPass));

    return RenderPassHandle(renderPass);
}

void VulkanRHI::DestroyRenderPass(RenderPassHandle renderPassHandle)
{
    VkRenderPass renderPass = TO_VK_RENDER_PASS(renderPassHandle);
    vkDestroyRenderPass(GetVkDevice(), renderPass, nullptr);
}

VulkanFramebuffer::VulkanFramebuffer(VulkanRHI* vkRHI,
                                     VkRenderPass renderPass,
                                     const FramebufferInfo& fbInfo) :
    m_vkRHI(vkRHI),
    m_renderPass(renderPass),
    m_width(fbInfo.width),
    m_height(fbInfo.height),
    m_layers(fbInfo.depth)
{
    std::vector<VkImageView> imageViews;
    imageViews.resize(fbInfo.numRenderTarget);
    for (uint32_t i = 0; i < fbInfo.numRenderTarget; i++)
    {
        VulkanTexture* texture = TO_VK_TEXTURE(fbInfo.renderTargets[i]);
        imageViews[i]          = texture->imageView;
    }
    VkFramebufferCreateInfo framebufferCI;
    InitVkStruct(framebufferCI, VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
    framebufferCI.renderPass      = renderPass;
    framebufferCI.width           = fbInfo.width;
    framebufferCI.height          = fbInfo.height;
    framebufferCI.layers          = fbInfo.depth;
    framebufferCI.attachmentCount = imageViews.size();
    framebufferCI.pAttachments    = imageViews.data();
    VKCHECK(vkCreateFramebuffer(vkRHI->GetVkDevice(), &framebufferCI, nullptr, &m_framebuffer));
}

FramebufferHandle VulkanRHI::CreateFramebuffer(RenderPassHandle renderPassHandle,
                                               const FramebufferInfo& fbInfo)
{
    VulkanFramebuffer* framebuffer =
        new VulkanFramebuffer(this, TO_VK_RENDER_PASS(renderPassHandle), fbInfo);
    return FramebufferHandle(framebuffer);
}

void VulkanRHI::DestroyFramebuffer(FramebufferHandle framebufferHandle)
{
    VulkanFramebuffer* framebuffer = TO_VK_FRAMEBUFFER(framebufferHandle);
    vkDestroyFramebuffer(GetVkDevice(), framebuffer->GetVkHandle(), nullptr);
    delete framebuffer;
}

} // namespace zen::rhi