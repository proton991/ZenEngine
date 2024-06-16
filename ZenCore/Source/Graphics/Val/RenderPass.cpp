#include "Graphics/Val/RenderPass.h"
#include "Common/Errors.h"
#include "Common/Helpers.h"

namespace zen::val
{
SubpassInfo::SubpassInfo(const std::vector<VkAttachmentReference>& colors,
                         const std::vector<VkAttachmentReference>& inputs,
                         const uint32_t depthStencil) :
    colorRefs(colors), inputRefs(inputs)
{
    if (depthStencil != UINT32_MAX)
    {
        hasDepthStencilRef = true;
        depthStencilRef    = {depthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    }
}

RenderPass::RenderPass(const Device& device,
                       const std::vector<VkAttachmentDescription>& attachments,
                       const SubpassInfo& subpassInfo) :
    DeviceObject(device)
{
    VkSubpassDescription subpassDescription{};
    subpassDescription.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpassDescription.colorAttachmentCount = util::ToU32(subpassInfo.colorRefs.size());
    subpassDescription.pColorAttachments    = subpassInfo.colorRefs.data();
    subpassDescription.pDepthStencilAttachment =
        subpassInfo.hasDepthStencilRef ? &subpassInfo.depthStencilRef : nullptr;

    //    std::vector<VkSubpassDependency> subpassDeps(2);
    //    subpassDeps[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
    //    subpassDeps[0].dstSubpass      = 0;
    //    subpassDeps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    //    subpassDeps[0].srcAccessMask   = VK_ACCESS_MEMORY_READ_BIT;
    //    subpassDeps[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    //    subpassDeps[0].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    //    subpassDeps[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    //
    //    subpassDeps[1].srcSubpass      = 0;
    //    subpassDeps[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
    //    subpassDeps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    //    subpassDeps[1].srcAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    //    subpassDeps[1].dstAccessMask   = VK_ACCESS_MEMORY_READ_BIT;
    //    subpassDeps[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    //    subpassDeps[1].dstStageMask    = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    VkRenderPassCreateInfo renderPassCI{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassCI.attachmentCount = util::ToU32(attachments.size());
    renderPassCI.pAttachments    = attachments.data();
    //    renderPassCI.dependencyCount = util::ToU32(subpassDeps.size());
    //    renderPassCI.pDependencies   = subpassDeps.data();
    renderPassCI.subpassCount = 1;
    renderPassCI.pSubpasses   = &subpassDescription;

    CHECK_VK_ERROR_AND_THROW(
        vkCreateRenderPass(m_device.GetHandle(), &renderPassCI, nullptr, &m_handle),
        "Failed to create RenderPass");
}

RenderPass::RenderPass(RenderPass&& other) noexcept : DeviceObject(std::move(other)) {}

RenderPass::~RenderPass()
{
    if (m_handle != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_device.GetHandle(), m_handle, nullptr);
    }
}

RenderPass::RenderPass(const Device& device,
                       const std::vector<VkAttachmentDescription>& attachments,
                       const std::vector<SubpassInfo>& subpassInfos,
                       const std::vector<VkSubpassDependency>& subpassDeps) :
    DeviceObject(device)
{
    std::vector<VkSubpassDescription> subpassDescriptions(subpassInfos.size());
    for (auto i = 0u; i < subpassInfos.size(); i++)
    {
        const auto& subpassInfo = subpassInfos[i];
        // create subpass description
        VkSubpassDescription& subpassDesc = subpassDescriptions[i];
        subpassDesc.pipelineBindPoint     = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassDesc.inputAttachmentCount  = static_cast<uint32_t>(subpassInfo.inputRefs.size());
        subpassDesc.pInputAttachments     = subpassInfo.inputRefs.data();
        subpassDesc.colorAttachmentCount  = static_cast<uint32_t>(subpassInfo.colorRefs.size());
        subpassDesc.pColorAttachments     = subpassInfo.colorRefs.data();
        subpassDesc.pDepthStencilAttachment =
            subpassInfo.hasDepthStencilRef ? &subpassInfo.depthStencilRef : nullptr;
    }
    VkRenderPassCreateInfo renderPassCI{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassCI.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassCI.pAttachments    = attachments.data();
    renderPassCI.subpassCount    = static_cast<uint32_t>(subpassDescriptions.size());
    renderPassCI.pSubpasses      = subpassDescriptions.data();
    renderPassCI.dependencyCount = static_cast<uint32_t>(subpassDeps.size());
    renderPassCI.pDependencies   = subpassDeps.data();
    CHECK_VK_ERROR_AND_THROW(
        vkCreateRenderPass(m_device.GetHandle(), &renderPassCI, nullptr, &m_handle),
        "Failed to create render pass");
}

//RenderPassBuilder& RenderPassBuilder::AddPresentAttachment(VkFormat format)
//{
//    VkAttachmentDescription att{};
//    att.format         = format;
//    att.samples        = VK_SAMPLE_COUNT_1_BIT;
//    att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
//    att.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
//    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
//    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
//    att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
//    att.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
//    m_attachments.emplace_back(att);
//    return *this;
//}
//
//RenderPassBuilder& RenderPassBuilder::AddColorAttachment(VkFormat format, bool clear)
//{
//    VkAttachmentDescription att{};
//    att.format         = format;
//    att.samples        = VK_SAMPLE_COUNT_1_BIT;
//    att.loadOp         = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
//    att.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
//    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
//    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
//    att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
//    att.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
//    m_attachments.emplace_back(att);
//    return *this;
//}
//
//RenderPassBuilder& RenderPassBuilder::AddDepthStencilAttachment(VkFormat format)
//{
//    VkAttachmentDescription att{};
//    att.format         = format;
//    att.samples        = VK_SAMPLE_COUNT_1_BIT;
//    att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
//    att.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
//    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
//    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
//    att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
//    att.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
//    m_attachments.emplace_back(att);
//    return *this;
//}

//RenderPassBuilder& RenderPassBuilder::AddSubpass(const std::vector<uint32_t>& colorRefs,
//                                                 const std::vector<uint32_t>& inputRefs,
//                                                 uint32_t                     depthStencilRef)
//{
//    m_subpassInfos.emplace_back(colorRefs, inputRefs, depthStencilRef);
//    return *this;
//}
//
//RenderPassBuilder& RenderPassBuilder::SetSubpassDeps(const SubpassDepInfo& info)
//{
//    auto     subpassCount = static_cast<uint32_t>(m_subpassInfos.size());
//    uint32_t lastSubpass  = subpassCount - 1;
//    // external dependencies
//    VkSubpassDependency              init = {};
//    std::vector<VkSubpassDependency> deps(subpassCount + 1, init);
//    if (info.externDepthDep)
//    {
//        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
//        deps[0].dstSubpass = 0;
//
//        deps[0].srcStageMask |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
//        deps[0].dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
//            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
//        deps[0].srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
//        deps[0].dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
//            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
//        deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
//
//        deps[subpassCount].srcSubpass = lastSubpass;
//        deps[subpassCount].dstSubpass = VK_SUBPASS_EXTERNAL;
//        deps[subpassCount].srcStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
//            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
//        deps[subpassCount].dstStageMask |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
//        deps[subpassCount].srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
//            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
//        deps[subpassCount].dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
//        ;
//        deps[subpassCount].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
//    }
//    if (info.externColorDep)
//    {
//        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
//        deps[0].dstSubpass = 0;
//        deps[0].srcStageMask |= VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
//        deps[0].dstStageMask |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
//        deps[0].srcAccessMask |= VK_ACCESS_MEMORY_READ_BIT;
//        deps[0].dstAccessMask |=
//            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
//        deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
//
//        deps[subpassCount].srcSubpass = lastSubpass;
//        deps[subpassCount].dstSubpass = VK_SUBPASS_EXTERNAL;
//        deps[subpassCount].srcStageMask |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
//        deps[subpassCount].dstStageMask |= VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
//        deps[subpassCount].srcAccessMask |=
//            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
//        deps[subpassCount].dstAccessMask |= VK_ACCESS_MEMORY_READ_BIT;
//        deps[subpassCount].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
//    }
//    for (uint32_t subpass = 1; subpass < subpassCount; subpass++)
//    {
//        deps[subpass].srcSubpass = subpass - 1;
//        deps[subpass].dstSubpass = subpass;
//        if (info.colorAttReadWrite & (1u << (subpass - 1)))
//        {
//            deps[subpass].srcStageMask |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
//            deps[subpass].srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
//        }
//
//        if (info.depthAttWrite & (1u << (subpass - 1)))
//        {
//            deps[subpass].srcStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
//                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
//            deps[subpass].srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
//        }
//
//        if (info.colorAttReadWrite & (1u << subpass))
//        {
//            deps[subpass].dstStageMask |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
//            deps[subpass].dstAccessMask |=
//                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
//        }
//
//        if (info.depthAttRead & (1u << subpass))
//        {
//            deps[subpass].dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
//                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
//            deps[subpass].dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
//        }
//
//        if (info.depthAttWrite & (1u << subpass))
//        {
//            deps[subpass].dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
//                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
//            deps[subpass].dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
//                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
//        }
//
//        if (info.inputAttRead & (1u << subpass))
//        {
//            deps[subpass].dstStageMask |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
//            deps[subpass].dstAccessMask |= VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
//        }
//    }
//    m_subpassDeps = std::move(deps);
//    return *this;
//}
//
//RenderPass* RenderPassBuilder::Build()
//{
//    return new RenderPass(m_device, m_attachments, m_subpassInfos, m_subpassDeps);
//}
} // namespace zen::val