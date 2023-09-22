#include "Graphics/Rendering/RenderGraph.h"
#include "Common/Errors.h"
#include "Common/Helpers.h"
#include <queue>

namespace zen
{
RDGPass::RDGPass(RenderGraph& graph, Index index, std::string tag, RDGQueueFlags queueFlags) :
    m_graph(graph), m_index(index), m_tag(std::move(tag)), m_queueFlags(queueFlags)
{
}

void RDGPass::WriteToColorImage(const std::string& tag, const RDGImage::Info& info)
{
    auto* res = m_graph.GetImageResource(tag);
    res->AddImageUsage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    res->WriteInPass(m_index);
    res->SetInfo(info);
    m_outImageResources.push_back(res);
}

void RDGPass::ReadFromAttachment(const std::string& tag)
{
    auto* res = m_graph.GetImageResource(tag);
    res->AddImageUsage(VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
    res->ReadInPass(m_index);
    m_inImagesResources.push_back(res);
}

void RDGPass::WriteToStorageBuffer(const std::string& tag, const RDGBuffer::Info& info)
{
    auto* res = m_graph.GetBufferResource(tag);
    res->AddBufferUsage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    res->WriteInPass(m_index);
    res->SetInfo(info);
    m_outBufferResources.push_back(res);
}

void RDGPass::WriteToTransferDstBuffer(const Tag& tag, const RDGBuffer::Info& info)
{
    auto* res = m_graph.GetBufferResource(tag);
    res->AddBufferUsage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    res->WriteInPass(m_index);
    res->SetInfo(info);
    m_outBufferResources.push_back(res);
}

void RDGPass::ReadFromStorageBuffer(const std::string& tag)
{
    auto* res = m_graph.GetBufferResource(tag);
    res->AddBufferUsage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    res->ReadInPass(m_index);
    m_inBuffersResources.push_back(res);
}

void RDGPass::WriteToDepthStencilImage(const std::string& tag, const RDGImage::Info& info)
{
    auto* res = m_graph.GetImageResource(tag);
    res->AddImageUsage(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    res->WriteInPass(m_index);
    res->SetInfo(info);
    m_outImageResources.push_back(res);
}

void RDGPass::ReadFromDepthStencilImage(const std::string& tag)
{
    auto* res = m_graph.GetImageResource(tag);
    res->AddImageUsage(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    res->ReadInPass(m_index);
    m_inImagesResources.push_back(res);
}

void RDGPass::ReadFromGenericTexture(const Tag& tag)
{
    auto* res = m_graph.GetImageResource(tag);
    res->AddImageUsage(VK_IMAGE_USAGE_SAMPLED_BIT);
    res->ReadInPass(m_index);

    RDGAccessedTexture accTexture;
    accTexture.registry = res;
    m_inTextures.push_back(accTexture);
}

RDGImage* RenderGraph::GetImageResource(const std::string& tag)
{
    auto iter = m_resourceToIndex.find(tag);
    if (iter != m_resourceToIndex.end())
    {
        ASSERT(m_resources[iter->second]->GetType() == RDGResource::Type::Image);
        return m_resources[iter->second]->As<RDGImage>();
    }
    else
    {
        Index index = m_resources.size();
        m_resources.emplace_back(new RDGImage(index, tag));
        m_resourceToIndex[tag] = index;
        return m_resources.back()->As<RDGImage>();
    }
}

RDGBuffer* RenderGraph::GetBufferResource(const std::string& tag)
{
    auto iter = m_resourceToIndex.find(tag);
    if (iter != m_resourceToIndex.end())
    {
        ASSERT(m_resources[iter->second]->GetType() == RDGResource::Type::Buffer);
        return m_resources[iter->second]->As<RDGBuffer>();
    }
    else
    {
        Index index = m_resources.size();
        m_resources.emplace_back(new RDGImage(index, tag));
        m_resourceToIndex[tag] = index;
        return m_resources.back()->As<RDGBuffer>();
    }
}

void RenderGraph::SetBackBufferSize(uint32_t width, uint32_t height)
{
    m_backBufferExtent.width  = width;
    m_backBufferExtent.height = height;
}

RDGPass& RenderGraph::AddPass(const std::string& tag, RDGQueueFlags queueFlags)
{
    auto iter = m_passToIndex.find(tag);
    if (iter != m_passToIndex.end())
    {
        return *m_passes[iter->second];
    }
    else
    {
        Index index = m_passes.size();
        m_passes.emplace_back(new RDGPass(*this, index, tag, queueFlags));
        m_passToIndex[tag] = index;
        return *m_passes.back();
    }
}

void RenderGraph::SetBackBufferTag(const Tag& tag)
{
    m_backBufferTag = tag;
}

void RenderGraph::Compile()
{
    SortRenderPasses();
}

void RenderGraph::SortRenderPasses()
{
    if (m_resourceToIndex.find(m_backBufferTag) == m_resourceToIndex.end())
    {
        LOG_ERROR_AND_THROW("Back buffer resource does not exist!");
    }

    auto& backBuffer = m_resources[m_resourceToIndex[m_backBufferTag]];
    if (backBuffer->GetWrittenInPasses().empty())
    {
        LOG_ERROR_AND_THROW("No pass exists which writes to back buffer");
    }

    for (auto& passIndex : backBuffer->GetWrittenInPasses())
    {
        m_sortedPassIndices.push_back(passIndex);
    }
    // use a copy here as TraversePassDepsRecursive() will change the content of m_sortedPassIndices
    auto tmp = m_sortedPassIndices;
    for (auto& passIndex : tmp)
    {
        TraversePassDepsRecursive(passIndex, 0);
    }

    std::reverse(m_sortedPassIndices.begin(), m_sortedPassIndices.end());
    RemoveDuplicates(m_sortedPassIndices);
}

void RenderGraph::TraversePassDepsRecursive(Index passIndex, uint32_t level)
{
    auto& pass = *m_passes[passIndex];
    if (level > m_passes.size())
    {
        LOG_ERROR_AND_THROW("Cycle detected in render graph!");
    }
    for (auto& input : pass.GetInImageResources())
    {
        for (auto& dep : input->GetWrittenInPasses())
        {
            m_passDeps[pass.GetIndex()].insert(dep);
        }
    }
    for (auto& input : pass.GetInBufferResources())
    {
        for (auto& dep : input->GetWrittenInPasses())
        {
            m_passDeps[pass.GetIndex()].insert(dep);
        }
    }
    level++;
    for (auto& index : m_passDeps[pass.GetIndex()])
    {
        m_sortedPassIndices.push_back(index);
        TraversePassDepsRecursive(index, level);
    }
}

void RenderGraph::RemoveDuplicates(std::vector<Index>& list)
{
    std::unordered_set<Index> visited;
    auto                      tail = list.begin();
    for (auto it = list.begin(); it != list.end(); ++it)
    {
        if (!visited.count(*it))
        {
            *tail = *it;
            visited.insert(*it);
            tail++;
        }
    }
    list.erase(tail, list.end());
}

void RenderGraph::ResolveResourceState()
{
    std::unordered_map<Tag, VkBufferUsageFlags> lastBufferUsages;
    std::unordered_map<Tag, VkImageUsageFlags>  lastImageUsages;
    for (const auto& pass : m_passes)
    {
        auto& bufferTransitions = m_resourceState.perPassBufferState[pass->GetTag()];
        auto& imageTransitions  = m_resourceState.perPassImageState[pass->GetTag()];
        for (auto& input : pass->GetInBufferResources())
        {
            if (lastBufferUsages.find(input->GetTag()) == lastBufferUsages.end())
            {
                m_resourceState.bufferFirstUsePass[input->GetTag()] = pass->GetTag();
                // set at the end
                lastBufferUsages[input->GetTag()] = VK_BUFFER_USAGE_FLAG_BITS_MAX_ENUM;
            }
            bufferTransitions[input->GetTag()].srcUsage = lastBufferUsages[input->GetTag()];
            bufferTransitions[input->GetTag()].dstUsage = input->GetUsage();
            m_resourceState.totalUsages[input->GetTag()] |= input->GetUsage();
            lastBufferUsages[input->GetTag()] = input->GetUsage();

            m_resourceState.bufferLastUsePass[input->GetTag()] = pass->GetTag();
        }
        for (auto& output : pass->GetOutBufferResources())
        {
            if (lastBufferUsages.find(output->GetTag()) == lastBufferUsages.end())
            {
                m_resourceState.bufferFirstUsePass[output->GetTag()] = pass->GetTag();
                // set at the end
                lastBufferUsages[output->GetTag()] = VK_BUFFER_USAGE_FLAG_BITS_MAX_ENUM;
            }
            bufferTransitions[output->GetTag()].srcUsage = lastBufferUsages[output->GetTag()];
            bufferTransitions[output->GetTag()].dstUsage = output->GetUsage();
            m_resourceState.totalUsages[output->GetTag()] |= output->GetUsage();
            lastBufferUsages[output->GetTag()] = output->GetUsage();

            m_resourceState.bufferLastUsePass[output->GetTag()] = pass->GetTag();
        }
        for (auto& input : pass->GetInImageResources())
        {
            if (lastImageUsages.find(input->GetTag()) == lastImageUsages.end())
            {
                m_resourceState.imageFirstUsePass[input->GetTag()] = pass->GetTag();
                // set at the end
                lastImageUsages[input->GetTag()] = VK_IMAGE_USAGE_FLAG_BITS_MAX_ENUM;
            }
            imageTransitions[input->GetTag()].srcUsage = lastImageUsages[input->GetTag()];
            imageTransitions[input->GetTag()].dstUsage = input->GetUsage();
            m_resourceState.totalUsages[input->GetTag()] |= input->GetUsage();
            lastImageUsages[input->GetTag()] = input->GetUsage();

            m_resourceState.imageLastUsePass[input->GetTag()] = pass->GetTag();
        }
        for (auto& output : pass->GetOutImageResources())
        {
            if (lastImageUsages.find(output->GetTag()) == lastImageUsages.end())
            {
                m_resourceState.imageFirstUsePass[output->GetTag()] = pass->GetTag();
                // set at the end
                lastImageUsages[output->GetTag()] = VK_IMAGE_USAGE_FLAG_BITS_MAX_ENUM;
            }
            imageTransitions[output->GetTag()].srcUsage = lastImageUsages[output->GetTag()];
            imageTransitions[output->GetTag()].dstUsage = output->GetUsage();
            m_resourceState.totalUsages[output->GetTag()] |= output->GetUsage();
            lastImageUsages[output->GetTag()] = output->GetUsage();

            m_resourceState.imageLastUsePass[output->GetTag()] = pass->GetTag();
        }
    }
    for (const auto& [bufferTag, passTag] : m_resourceState.bufferFirstUsePass)
    {
        auto& firstBufferTransition    = m_resourceState.perPassBufferState[passTag][bufferTag];
        firstBufferTransition.srcUsage = lastBufferUsages[bufferTag];
    }
    for (const auto& [imageTag, passTag] : m_resourceState.imageFirstUsePass)
    {
        auto& firstImageTransition    = m_resourceState.perPassImageState[passTag][imageTag];
        firstImageTransition.srcUsage = lastImageUsages[imageTag];
    }
}

void RenderGraph::BuildPhysicalImage(RDGImage* image)
{
    const auto& info = image->GetInfo();

    val::ImageCreateInfo imageCI{};
    imageCI.usage       = info.usage;
    imageCI.format      = info.format;
    imageCI.samples     = static_cast<VkSampleCountFlagBits>(info.samples);
    imageCI.arrayLayers = info.layers;
    imageCI.mipLevels   = info.levels;
    imageCI.vmaFlags    = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT; // TODO: Is it OK?
    switch (info.sizeType)
    {
        case ImageSizeType::SwapchainRelative:
            imageCI.extent3D.width  = static_cast<uint32_t>(info.sizeX * m_backBufferExtent.width);
            imageCI.extent3D.height = static_cast<uint32_t>(info.sizeX * m_backBufferExtent.height);
            imageCI.extent3D.depth  = 1;
            break;
        case ImageSizeType::Absolute:
            imageCI.extent3D.width  = info.extent3D.width;
            imageCI.extent3D.height = info.extent3D.height;
            imageCI.extent3D.depth  = info.extent3D.depth;
            break;
    }
    auto physicalIndex = m_physicalImages.size();
    image->SetPhysicalIndex(physicalIndex);
    m_physicalImages.emplace_back(val::Image::CreateUnique(m_valDevice, imageCI));
}

void RenderGraph::BuildPhysicalBuffer(RDGBuffer* buffer)
{
    const auto& info = buffer->GetInfo();

    val::BufferCreateInfo bufferCI{};
    bufferCI.size     = info.size;
    bufferCI.usage    = info.usage;
    bufferCI.vmaFlags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT; // TODO: Choose right vma flags;

    auto physicalIndex = m_physicalBuffers.size();
    buffer->SetPhysicalIndex(physicalIndex);
    m_physicalBuffers.emplace_back(val::Buffer::CreateUnique(m_valDevice, bufferCI));
}

void RenderGraph::BuildPhysicalResources()
{
    for (auto& resource : m_resources)
    {
        if (resource->GetType() == RDGResource::Type::Image)
        {
            BuildPhysicalImage(resource->As<RDGImage>());
        }
        else
        {
            BuildPhysicalBuffer(resource->As<RDGBuffer>());
        }
    }
}

static VkImageLayout ImageUsageToImageLayout(VkImageUsageFlags usage)
{
    if (usage == 0)
        return VK_IMAGE_LAYOUT_UNDEFINED;
    if (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT)
        return VK_IMAGE_LAYOUT_GENERAL;
    if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
        return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    return VK_IMAGE_LAYOUT_UNDEFINED;
}
void RenderGraph::BuildPhysicalPasses()
{
    for (const auto& pass : m_passes)
    {
        PhysicalPass physicalPass{};

        std::vector<VkAttachmentDescription> attachmentDescriptions;
        std::vector<VkAttachmentReference>   attachmentReferences;
        std::vector<VkImageView>             imageViews;
        VkAttachmentReference                depthReference{};
        bool                                 hasDepthRef{false};

        uint32_t framebufferWidth  = 0;
        uint32_t framebufferHeight = 0;

        auto& rdgImages = pass->GetOutImageResources();
        if (!rdgImages.empty())
        {
            for (uint32_t attIndex = 0; attIndex < rdgImages.size(); attIndex++)
            {
                const auto& rdgImage        = rdgImages[attIndex];
                const auto& phyImage        = m_physicalImages.at(rdgImage->GetPhysicalIndex());
                const auto& imageTransition = m_resourceState.perPassImageState.at(pass->GetTag());

                auto extent3D     = phyImage->GetExtent3D();
                framebufferWidth  = std::max(framebufferWidth, extent3D.width);
                framebufferHeight = std::max(framebufferHeight, extent3D.height);
                imageViews.push_back(phyImage->GetView());

                VkAttachmentDescription attachmentDescription{};
                attachmentDescription.format         = rdgImage->GetInfo().format;
                attachmentDescription.samples        = static_cast<VkSampleCountFlagBits>(rdgImage->GetInfo().samples);
                attachmentDescription.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // TODO: set proper load op
                attachmentDescription.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
                attachmentDescription.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                attachmentDescription.initialLayout  = ImageUsageToImageLayout(imageTransition.at(rdgImage->GetTag()).srcUsage);
                attachmentDescription.finalLayout    = ImageUsageToImageLayout(imageTransition.at(rdgImage->GetTag()).dstUsage);
                attachmentDescriptions.push_back(attachmentDescription);

                VkAttachmentReference attachmentReference{};
                attachmentReference.attachment = attIndex;
                attachmentReference.layout     = attachmentDescription.finalLayout;
                if (rdgImage->GetUsage() & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
                {
                    depthReference = attachmentReference;
                    hasDepthRef    = true;
                }
                else
                {
                    attachmentReferences.push_back(attachmentReference);
                }
            }
        }
        VkSubpassDescription subpassDescription{};
        subpassDescription.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassDescription.colorAttachmentCount    = util::ToU32(attachmentReferences.size());
        subpassDescription.pColorAttachments       = attachmentReferences.data();
        subpassDescription.pDepthStencilAttachment = hasDepthRef ? &depthReference : nullptr;

        std::vector<VkSubpassDependency> subpassDeps(2);
        subpassDeps[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
        subpassDeps[0].dstSubpass      = 0;
        subpassDeps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        subpassDeps[0].srcAccessMask   = VK_ACCESS_MEMORY_READ_BIT;
        subpassDeps[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        subpassDeps[0].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        subpassDeps[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;

        subpassDeps[1].srcSubpass      = 0;
        subpassDeps[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
        subpassDeps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        subpassDeps[1].srcAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        subpassDeps[1].dstAccessMask   = VK_ACCESS_MEMORY_READ_BIT;
        subpassDeps[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        subpassDeps[1].dstStageMask    = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

        VkRenderPassCreateInfo renderPassCI{};
        renderPassCI.attachmentCount = util::ToU32(attachmentDescriptions.size());
        renderPassCI.pAttachments    = attachmentDescriptions.data();
        renderPassCI.dependencyCount = util::ToU32(subpassDeps.size());
        renderPassCI.pDependencies   = subpassDeps.data();
        renderPassCI.subpassCount    = 1;
        renderPassCI.pSubpasses      = &subpassDescription;
        vkCreateRenderPass(m_valDevice.GetHandle(), &renderPassCI, nullptr, &physicalPass.renderPass);
        // Create framebuffer
        physicalPass.framebuffer = new val::Framebuffer(m_valDevice, physicalPass.renderPass, imageViews, {framebufferWidth, framebufferHeight, 1});
        // Create pipeline
    }
}
} // namespace zen