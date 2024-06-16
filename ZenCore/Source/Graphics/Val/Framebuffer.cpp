#include "Graphics/Val/Framebuffer.h"
#include "Common/Errors.h"

namespace zen::val
{
Framebuffer::Framebuffer(const Device& device,
                         VkRenderPass renderPassHandle,
                         const std::vector<VkImageView>& attachments,
                         VkExtent3D extent3D) :
    DeviceObject(device), m_extent(extent3D)
{
    VkFramebufferCreateInfo fbCI{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbCI.renderPass      = renderPassHandle;
    fbCI.pAttachments    = attachments.data();
    fbCI.attachmentCount = static_cast<uint32_t>(attachments.size());
    fbCI.width           = m_extent.width;
    fbCI.height          = m_extent.height;
    fbCI.layers          = m_extent.depth;

    CHECK_VK_ERROR_AND_THROW(vkCreateFramebuffer(m_device.GetHandle(), &fbCI, nullptr, &m_handle),
                             "Failed to create framebuffer");
}

Framebuffer::~Framebuffer()
{
    if (m_handle != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(m_device.GetHandle(), m_handle, nullptr);
    }
}

Framebuffer::Framebuffer(Framebuffer&& other) noexcept :
    DeviceObject(std::move(other)), m_extent(other.m_extent)
{}

} // namespace zen::val