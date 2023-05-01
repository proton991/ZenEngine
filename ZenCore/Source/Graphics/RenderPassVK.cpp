#include "Graphics/Vulkan/RenderPassVK.h"
#include "Graphics/Vulkan/DeviceVK.h"

namespace zen::vulkan {
SubpassInfo::SubpassInfo(const std::vector<uint32_t>& colors, const std::vector<uint32_t>& inputs,
                         uint32_t depthStencil) {
  for (const auto color : colors) {
    colorRefs.emplace_back(
        vk::AttachmentReference(color, vk::ImageLayout::eColorAttachmentOptimal));
  }
  for (const auto input : inputs) {
    colorRefs.emplace_back(vk::AttachmentReference(input, vk::ImageLayout::eShaderReadOnlyOptimal));
  }
  depthStencilRef =
      vk::AttachmentReference(depthStencil, vk::ImageLayout::eDepthStencilAttachmentOptimal);
}

RenderPassBuilder& RenderPassBuilder::AddPresentAttachment(vk::Format format) {
  auto att = vk::AttachmentDescription()
                 .setFormat(format)
                 .setSamples(vk::SampleCountFlagBits::e1)
                 .setLoadOp(vk::AttachmentLoadOp::eClear)
                 .setStoreOp(vk::AttachmentStoreOp::eStore)
                 .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
                 .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
                 .setInitialLayout(vk::ImageLayout::eUndefined)
                 .setFinalLayout(vk::ImageLayout::ePresentSrcKHR);
  m_attachments.emplace_back(att);
  return *this;
}

RenderPassBuilder& RenderPassBuilder::AddColorAttachment(vk::Format format, bool clear) {
  auto att = vk::AttachmentDescription()
                 .setFormat(format)
                 .setSamples(vk::SampleCountFlagBits::e1)
                 .setLoadOp(clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eDontCare)
                 .setStoreOp(vk::AttachmentStoreOp::eDontCare)
                 .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
                 .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
                 .setInitialLayout(vk::ImageLayout::eUndefined)
                 .setFinalLayout(vk::ImageLayout::eColorAttachmentOptimal);
  m_attachments.emplace_back(att);
  return *this;
}

RenderPassBuilder& RenderPassBuilder::AddDepthStencilAttatchment(vk::Format format) {
  auto att = vk::AttachmentDescription()
                 .setFormat(format)
                 .setSamples(vk::SampleCountFlagBits::e1)
                 .setLoadOp(vk::AttachmentLoadOp::eClear)
                 .setStoreOp(vk::AttachmentStoreOp::eDontCare)
                 .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
                 .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
                 .setInitialLayout(vk::ImageLayout::eUndefined)
                 .setFinalLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
  m_attachments.emplace_back(att);
  return *this;
}

RenderPassBuilder& RenderPassBuilder::AddSubpass(const std::vector<uint32_t>& colorRefs,
                                                 const std::vector<uint32_t>& inputRefs,
                                                 uint32_t depthStencilRef) {
  m_subpassInfos.emplace_back(colorRefs, inputRefs, depthStencilRef);
  return *this;
}

RenderPassBuilder& RenderPassBuilder::SetSubpassDeps(const SubpassDepInfo& info) {
  auto subpassCount    = static_cast<uint32_t>(m_subpassInfos.size());
  uint32_t lastSubpass = subpassCount - 1;
  // external dependencies
  vk::SubpassDependency init = {};
  std::vector<vk::SubpassDependency> deps(subpassCount + 1, init);
  if (info.externDepthDep) {
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask |= vk::PipelineStageFlagBits::eLateFragmentTests;
    deps[0].dstStageMask |= vk::PipelineStageFlagBits::eEarlyFragmentTests |
                            vk::PipelineStageFlagBits::eLateFragmentTests;
    deps[0].srcAccessMask |= vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    deps[0].dstAccessMask |= vk::AccessFlagBits::eDepthStencilAttachmentWrite |
                             vk::AccessFlagBits::eDepthStencilAttachmentRead;
    deps[0].dependencyFlags = vk::DependencyFlagBits::eByRegion;

    deps[subpassCount].srcSubpass = lastSubpass;
    deps[subpassCount].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[subpassCount].srcStageMask |= vk::PipelineStageFlagBits::eEarlyFragmentTests |
                                       vk::PipelineStageFlagBits::eLateFragmentTests;
    deps[subpassCount].dstStageMask |= vk::PipelineStageFlagBits::eLateFragmentTests;
    deps[subpassCount].srcAccessMask |= vk::AccessFlagBits::eDepthStencilAttachmentWrite |
                                        vk::AccessFlagBits::eDepthStencilAttachmentRead;
    deps[subpassCount].dstAccessMask |= vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    ;
    deps[subpassCount].dependencyFlags = vk::DependencyFlagBits::eByRegion;
  }
  if (info.externColorDep) {
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask |= vk::PipelineStageFlagBits::eBottomOfPipe;
    deps[0].dstStageMask |= vk::PipelineStageFlagBits::eColorAttachmentOutput;
    deps[0].srcAccessMask |= vk::AccessFlagBits::eMemoryRead;
    deps[0].dstAccessMask |=
        vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite;
    deps[0].dependencyFlags = vk::DependencyFlagBits::eByRegion;

    deps[subpassCount].srcSubpass = lastSubpass;
    deps[subpassCount].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[subpassCount].srcStageMask |= vk::PipelineStageFlagBits::eColorAttachmentOutput;
    deps[subpassCount].dstStageMask |= vk::PipelineStageFlagBits::eBottomOfPipe;
    deps[subpassCount].srcAccessMask |=
        vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite;
    deps[subpassCount].dstAccessMask |= vk::AccessFlagBits::eMemoryRead;
    deps[subpassCount].dependencyFlags = vk::DependencyFlagBits::eByRegion;
  }
  for (uint32_t subpass = 1; subpass < subpassCount; subpass++) {
    deps[subpass].srcSubpass = subpass - 1;
    deps[subpass].dstSubpass = subpass;
    if (info.colorAttReadWrite & (1u << (subpass - 1))) {
      deps[subpass].srcStageMask |= vk::PipelineStageFlagBits::eColorAttachmentOutput;
      deps[subpass].srcAccessMask |= vk::AccessFlagBits::eColorAttachmentWrite;
    }

    if (info.depthAttWrite & (1u << (subpass - 1))) {
      deps[subpass].srcStageMask |= vk::PipelineStageFlagBits::eEarlyFragmentTests |
                                    vk::PipelineStageFlagBits::eLateFragmentTests;
      deps[subpass].srcAccessMask |= vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    }

    if (info.colorAttReadWrite & (1u << subpass)) {
      deps[subpass].dstStageMask |= vk::PipelineStageFlagBits::eColorAttachmentOutput;
      deps[subpass].dstAccessMask |=
          vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eColorAttachmentRead;
    }

    if (info.depthAttRead & (1u << subpass)) {
      deps[subpass].dstStageMask |= vk::PipelineStageFlagBits::eEarlyFragmentTests |
                                    vk::PipelineStageFlagBits::eLateFragmentTests;
      deps[subpass].dstAccessMask |= vk::AccessFlagBits::eDepthStencilAttachmentRead;
    }

    if (info.depthAttWrite & (1u << subpass)) {
      deps[subpass].dstStageMask |= vk::PipelineStageFlagBits::eEarlyFragmentTests |
                                    vk::PipelineStageFlagBits::eLateFragmentTests;
      deps[subpass].dstAccessMask |= vk::AccessFlagBits::eDepthStencilAttachmentWrite |
                                     vk::AccessFlagBits::eDepthStencilAttachmentRead;
    }

    if (info.inputAttRead & (1u << subpass)) {
      deps[subpass].dstStageMask |= vk::PipelineStageFlagBits::eFragmentShader;
      deps[subpass].dstAccessMask |= vk::AccessFlagBits::eInputAttachmentRead;
    }
  }
  m_subpassDeps = std::move(deps);
  return *this;
}

UniquePtr<RenderPass> RenderPassBuilder::Build() {
  return MakeUnique<RenderPass>(m_device, m_attachments, m_subpassInfos, m_subpassDeps);
}

RenderPass::RenderPass(const Device& device,
                       const std::vector<vk::AttachmentDescription>& attachments,
                       const std::vector<SubpassInfo>& subpassInfos,
                       const std::vector<vk::SubpassDependency>& subpassDeps)
    : DeviceResource(device, nullptr) {
  std::vector<vk::SubpassDescription> subpassDescriptions;
  m_colorOutputCount.resize(subpassInfos.size());
  for (auto i = 0u; i < subpassInfos.size(); i++) {
    const auto& subpassInfo = subpassInfos[i];
    m_colorOutputCount[i++] = static_cast<uint32_t>(subpassInfo.colorRefs.size());
    auto subpass            = vk::SubpassDescription()
                       .setPipelineBindPoint(vk::PipelineBindPoint::eGraphics)
                       .setInputAttachmentCount(static_cast<uint32_t>(subpassInfo.inputRefs.size()))
                       .setPInputAttachments(
                           subpassInfo.inputRefs.empty() ? nullptr : subpassInfo.inputRefs.data())
                       .setColorAttachmentCount(static_cast<uint32_t>(subpassInfo.colorRefs.size()))
                       .setPColorAttachments(
                           subpassInfo.colorRefs.empty() ? nullptr : subpassInfo.colorRefs.data())
                       .setPDepthStencilAttachment(&subpassInfo.depthStencilRef);
    subpassDescriptions.emplace_back(subpass);
  }
  auto renderPassCI =
      vk::RenderPassCreateInfo()
          .setAttachmentCount(static_cast<uint32_t>(attachments.size()))
          .setPAttachments(attachments.empty() ? nullptr : attachments.data())
          .setSubpassCount(static_cast<uint32_t>(subpassDescriptions.size()))
          .setPSubpasses(subpassDescriptions.empty() ? nullptr : subpassDescriptions.data())
          .setDependencyCount(static_cast<uint32_t>(subpassDeps.size()))
          .setPDependencies(subpassDeps.empty() ? nullptr : subpassDeps.data());
  SetHanlde(GetDeviceHandle().createRenderPass(renderPassCI));
}

}  // namespace zen::vulkan