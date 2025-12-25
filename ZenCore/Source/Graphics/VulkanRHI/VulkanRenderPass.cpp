#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanRenderPass.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"
#include "Graphics/VulkanRHI/VulkanViewport.h"

namespace zen
{
VkRenderPassCreateInfo VulkanRenderPassBuilder::BuildRenderPassCreateInfo(
    const RHIRenderPassLayout& renderPassLayout)
{
    const auto& colorRTs = renderPassLayout.GetColorRenderTargets();

    // attachment descriptions
    for (uint32_t i = 0; i < renderPassLayout.GetNumColorRenderTargets(); i++)
    {
        VkAttachmentDescription& description = m_attachmentDescriptions[i];
        const RHIRenderTarget& colorRT       = colorRTs[i];
        // set field values
        description.format         = ToVkFormat(colorRT.format);
        description.samples        = ToVkSampleCountFlagBits(colorRT.numSamples);
        description.loadOp         = ToVkAttachmentLoadOp(colorRT.loadOp);
        description.storeOp        = ToVkAttachmentStoreOp(colorRT.storeOp);
        description.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        description.initialLayout  = colorRT.loadOp == RHIRenderTargetLoadOp::eClear ?
             VK_IMAGE_LAYOUT_UNDEFINED :
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        description.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference& reference = m_colorAttachmentReference[m_numColorAttachmentRefs];
        reference.attachment             = i;
        reference.layout                 = description.finalLayout;

        m_numColorAttachmentRefs++;
        m_numAttachments++;
    }

    if (renderPassLayout.HasDepthStencilRenderTarget())
    {
        VkAttachmentDescription& description  = m_attachmentDescriptions[m_numAttachments];
        const RHIRenderTarget& depthStencilRT = renderPassLayout.GetDepthStencilRenderTarget();
        description.format  = ToVkFormat(renderPassLayout.GetDepthStencilRenderTarget().format);
        description.samples = ToVkSampleCountFlagBits(depthStencilRT.numSamples);
        description.loadOp  = ToVkAttachmentLoadOp(depthStencilRT.loadOp);
        description.storeOp = ToVkAttachmentStoreOp(depthStencilRT.storeOp);
        description.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        description.initialLayout  = depthStencilRT.loadOp == RHIRenderTargetLoadOp::eClear ?
             VK_IMAGE_LAYOUT_UNDEFINED :
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        description.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

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

VkRenderPassCreateInfo VulkanRenderPassBuilder::BuildRenderPassCreateInfo(
    const RHIRenderingLayout* pLayout)
{
    // attachment descriptions
    for (uint32_t i = 0; i < pLayout->numColorRenderTargets; i++)
    {
        VkAttachmentDescription& description = m_attachmentDescriptions[i];
        const RHIRenderTarget& colorRT       = pLayout->colorRenderTargets[i];
        // set field values
        description.format         = ToVkFormat(colorRT.format);
        description.samples        = ToVkSampleCountFlagBits(colorRT.numSamples);
        description.loadOp         = ToVkAttachmentLoadOp(colorRT.loadOp);
        description.storeOp        = ToVkAttachmentStoreOp(colorRT.storeOp);
        description.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        description.initialLayout  = colorRT.loadOp == RHIRenderTargetLoadOp::eClear ?
             VK_IMAGE_LAYOUT_UNDEFINED :
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        description.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference& reference = m_colorAttachmentReference[m_numColorAttachmentRefs];
        reference.attachment             = i;
        reference.layout                 = description.finalLayout;

        m_numColorAttachmentRefs++;
        m_numAttachments++;
    }

    if (pLayout->hasDepthStencilRT)
    {
        VkAttachmentDescription& description  = m_attachmentDescriptions[m_numAttachments];
        const RHIRenderTarget& depthStencilRT = pLayout->depthStencilRenderTarget;

        description.format         = ToVkFormat(depthStencilRT.format);
        description.samples        = ToVkSampleCountFlagBits(depthStencilRT.numSamples);
        description.loadOp         = ToVkAttachmentLoadOp(depthStencilRT.loadOp);
        description.storeOp        = ToVkAttachmentStoreOp(depthStencilRT.storeOp);
        description.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        description.initialLayout  = depthStencilRT.loadOp == RHIRenderTargetLoadOp::eClear ?
             VK_IMAGE_LAYOUT_UNDEFINED :
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        description.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        m_depthStencilReference.attachment = m_numAttachments;
        m_depthStencilReference.layout     = description.finalLayout;

        m_numAttachments++;
    }

    // only 1 subpass
    VkSubpassDescription& subpassDescription = m_subpasses[0];
    subpassDescription.pipelineBindPoint     = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpassDescription.colorAttachmentCount  = m_numColorAttachmentRefs;
    subpassDescription.pColorAttachments     = m_colorAttachmentReference;
    if (pLayout->hasDepthStencilRT)
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

VkRenderPass VulkanRHI::GetOrCreateRenderPass(const RHIRenderingLayout* pRenderingLayout)
{
    const uint32_t layoutHash = pRenderingLayout->GetHash32();
    if (!m_renderPassCache.contains(layoutHash))
    {
        VkRenderPass renderPass{VK_NULL_HANDLE};
        VulkanRenderPassBuilder builder;
        VkRenderPassCreateInfo renderPassCI = builder.BuildRenderPassCreateInfo(pRenderingLayout);
        VKCHECK(vkCreateRenderPass(GetVkDevice(), &renderPassCI, nullptr, &renderPass));
        m_renderPassCache[layoutHash] = renderPass;
    }
    return m_renderPassCache[layoutHash];
}


VkFramebuffer VulkanRHI::GetOrCreateFramebuffer(const RHIRenderingLayout* pRenderingLayout,
                                                VkRenderPass renderPass)
{
    VkFramebuffer framebuffer{VK_NULL_HANDLE};
    VulkanViewport* viewport = GVulkanRHI->GetCurrentViewport();
    const uint32_t fbWidth   = pRenderingLayout->renderArea.Width();
    const uint32_t fbHeight  = pRenderingLayout->renderArea.Height();

    if ((viewport != nullptr) &&
        (fbWidth == viewport->GetWidth() && fbHeight == viewport->GetHeight()))
    {
        framebuffer = viewport->GetCompatibleFramebufferForBackBuffer(renderPass);
    }
    else
    {
        const uint32_t layoutHash = pRenderingLayout->GetHash32();
        if (!m_framebufferCache.contains(layoutHash))
        {
            const uint32_t numAttachments = pRenderingLayout->GetTotalNumRenderTarges();
            std::vector<VkImageView> imageViews;
            imageViews.resize(numAttachments);
            std::vector<RHITexture*> pTextures;
            pTextures.resize(numAttachments);
            pRenderingLayout->GetRHITextureData(pTextures.data());

            for (uint32_t i = 0; i < numAttachments; i++)
            {
                imageViews[i] = TO_VK_TEXTURE(pTextures[i])->GetVkImageView();
            }

            VkFramebufferCreateInfo framebufferCI;
            InitVkStruct(framebufferCI, VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
            framebufferCI.renderPass      = renderPass;
            framebufferCI.width           = fbWidth;
            framebufferCI.height          = fbHeight;
            framebufferCI.layers          = 1;
            framebufferCI.attachmentCount = numAttachments;
            framebufferCI.pAttachments    = imageViews.data();
            VKCHECK(vkCreateFramebuffer(GVulkanRHI->GetVkDevice(), &framebufferCI, nullptr,
                                        &framebuffer));

            m_framebufferCache[layoutHash] = framebuffer;
        }
        else
        {
            framebuffer = m_framebufferCache[layoutHash];
        }
    }

    return framebuffer;
}


RenderPassHandle VulkanRHI::CreateRenderPass(const RHIRenderPassLayout& renderPassLayout)
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
                                     const RHIFramebufferInfo& fbInfo) :
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
        imageViews[i]          = texture->GetVkImageView();
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
                                               const RHIFramebufferInfo& fbInfo)
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

} // namespace zen