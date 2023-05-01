#pragma once
#include <vector>
#include <vulkan/vulkan.hpp>
#include "Common/UniquePtr.h"
#include "DeviceResource.h"

namespace zen::vulkan {
class Device;
class RenderPass;
struct SubpassDepInfo {
  uint32_t inputAttRead{0};
  uint32_t colorAttReadWrite{0};
  uint32_t depthAttWrite{0};
  uint32_t depthAttRead{0};
  bool externDepthDep{false};
  bool externColorDep{true};
};

struct SubpassInfo {
  SubpassInfo(const std::vector<uint32_t>& colors, const std::vector<uint32_t>& inputs,
              uint32_t depthStencil);
  std::vector<vk::AttachmentReference> colorRefs;
  std::vector<vk::AttachmentReference> inputRefs;
  vk::AttachmentReference depthStencilRef{};
};

class RenderPassBuilder {
public:
  explicit RenderPassBuilder(const Device& device) : m_device(device) {}
  ~RenderPassBuilder() = default;

  RenderPassBuilder& AddPresentAttachment(vk::Format format);
  RenderPassBuilder& AddColorAttachment(vk::Format format, bool clear);
  RenderPassBuilder& AddDepthStencilAttatchment(vk::Format format);

  RenderPassBuilder& AddSubpass(const std::vector<uint32_t>& colorRefs,
                                const std::vector<uint32_t>& inputRefs, uint32_t depthStencilRef);

  RenderPassBuilder& SetSubpassDeps(const SubpassDepInfo& info);

  UniquePtr<RenderPass> Build();

private:
  const Device& m_device;
  std::vector<vk::AttachmentDescription> m_attachments;
  std::vector<SubpassInfo> m_subpassInfos;
  std::vector<vk::SubpassDependency> m_subpassDeps;
};

class RenderPass : public DeviceResource<vk::RenderPass> {
public:
  RenderPass(const Device& device, const std::vector<vk::AttachmentDescription>& attachments,
             const std::vector<SubpassInfo>& subpassInfos,
             const std::vector<vk::SubpassDependency>& subpassDeps);

private:
  std::vector<uint32_t> m_colorOutputCount;  // num of color attachments for each subpass
};
}  // namespace zen::vulkan