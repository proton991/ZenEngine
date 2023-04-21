#pragma once
#include <vector>
#include <vulkan/vulkan.hpp>
namespace zen::vulkan {
class Device;
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
  explicit RenderPassBuilder(vk::Device logicalDevice) : m_logicalDevice(logicalDevice) {}
  ~RenderPassBuilder() = default;

  RenderPassBuilder& AddPresentAttachment(vk::Format format);
  RenderPassBuilder& AddColorAttachment(vk::Format format, bool clear);
  RenderPassBuilder& AddDepthStencilAttatchment(vk::Format format);

  RenderPassBuilder& AddSubpass(const std::vector<uint32_t>& colorRefs,
                                const std::vector<uint32_t>& inputRefs, uint32_t depthStencilRef);

  RenderPassBuilder& SetSubpassDeps(const SubpassDepInfo& info);

  vk::UniqueRenderPass Build();

private:
  vk::Device m_logicalDevice;
  std::vector<vk::AttachmentDescription> m_attachments;
  std::vector<SubpassInfo> m_subpassInfos;
  std::vector<vk::SubpassDependency> m_subpassDeps;
};
}  // namespace zen::vulkan