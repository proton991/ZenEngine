#include "Graphics/RenderCore/RenderGraph.h"
#include "Common/Errors.h"
#include "Common/Helpers.h"
#include "Graphics/RenderCore/RenderContext.h"
#include "Graphics/RenderCore/RenderConfig.h"
#include <queue>

namespace zen
{
RDGPass::RDGPass(RenderGraph& graph, Index index, std::string tag, RDGQueueFlags queueFlags) :
    m_graph(graph), m_index(index), m_tag(std::move(tag)), m_queueFlags(queueFlags)
{}

void RDGPass::WriteToColorImage(const std::string& tag, const RDGImage::Info& info)
{
    auto* res = m_graph.GetImageResource(tag);
    res->SetInfo(info);
    res->AddImageUsage(val::ImageUsage::ColorAttachment);
    res->WriteInPass(m_index);
    m_outImageResources.push_back(res);
}

void RDGPass::ReadFromInternalImage(const std::string& tag, val::ImageUsage usage)
{
    ASSERT(m_graph.GetResourceIndexMap().count(tag) && "Depends on image not found in rdg");
    m_inImagesResources[tag] = usage;
}

void RDGPass::ReadFromInternalBuffer(const Tag& tag, val::BufferUsage usage)
{
    ASSERT(m_graph.GetResourceIndexMap().count(tag) && "Depends on buffer not found in rdg");
    m_inBufferResources[tag] = usage;
}

void RDGPass::WriteToStorageBuffer(const std::string& tag, const RDGBuffer::Info& info)
{
    auto* res = m_graph.GetBufferResource(tag);
    res->AddBufferUsage(val::BufferUsage::StorageBuffer);
    res->WriteInPass(m_index);
    res->SetInfo(info);
    m_outBufferResources.push_back(res);
}

void RDGPass::WriteToTransferDstBuffer(const Tag& tag, const RDGBuffer::Info& info)
{
    auto* res = m_graph.GetBufferResource(tag);
    res->AddBufferUsage(val::BufferUsage::TransferDst);
    res->WriteInPass(m_index);
    res->SetInfo(info);
    m_outBufferResources.push_back(res);
}

void RDGPass::WriteToDepthStencilImage(const std::string& tag, const RDGImage::Info& info)
{
    auto* res = m_graph.GetImageResource(tag);
    res->AddImageUsage(val::ImageUsage::DepthStencilAttachment);
    res->WriteInPass(m_index);
    res->SetInfo(info);
    m_outImageResources.push_back(res);
}

void RDGPass::ReadFromExternalImage(const Tag& tag, val::Image* image)
{
    m_externImageResources[tag] = {image};
}

void RDGPass::ReadFromExternalImages(const Tag& tag, const std::vector<val::Image*>& images)
{
    m_externImageResources[tag] = images;
}

void RDGPass::ReadFromExternalBuffer(const Tag& tag, val::Buffer* buffer)
{
    m_externBufferResources[tag] = buffer;
}

void RDGPass::BindSRD(const Tag& rdgResourceTag,
                      VkShaderStageFlagBits shaderStage,
                      const std::string& shaderResourceName)
{
    for (const auto& shader : m_shaders)
    {
        if (shader->GetStage() == shaderStage)
        {
            for (const auto& resource : shader->GetResources())
            {
                if (resource.name == shaderResourceName)
                {
                    m_srdBinding[rdgResourceTag] = resource;
                }
            }
        }
    }
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

RDGPass* RenderGraph::AddPass(const std::string& tag, RDGQueueFlags queueFlags)
{
    auto iter = m_passToIndex.find(tag);
    if (iter != m_passToIndex.end())
    {
        return m_passes[iter->second].Get();
    }
    else
    {
        Index index = m_passes.size();
        m_passes.emplace_back(new RDGPass(*this, index, tag, queueFlags));
        m_passToIndex[tag] = index;
        return m_passes.back().Get();
    }
}

void RenderGraph::SetBackBufferTag(const Tag& tag)
{
    m_backBufferTag = tag;
    m_resourceState.totalImageUsages[tag] |= val::ImageUsage::TransferSrc;
}

void RenderGraph::Compile()
{
    SortRenderPasses();
    ResolveResourceState();
    BuildPhysicalResources();
    BuildPhysicalPasses();
}

void RenderGraph::Execute(val::CommandBuffer* commandBuffer, RenderContext* renderContext)
{
    if (!m_initialized)
    {
        BeforeExecuteSetup(commandBuffer);
        m_initialized = true;
    }
    for (auto& physicalPass : m_physicalPasses)
    {
        RunPass(physicalPass, commandBuffer);
    }
    CopyToPresentImage(commandBuffer, *renderContext->GetActiveFrame().GetSwapchainImage());
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
    for (auto& [tag, _] : pass.GetInImageResources())
    {
        auto& input = m_resources[m_resourceToIndex[tag]];
        for (auto& dep : input->GetWrittenInPasses())
        {
            m_passDeps[pass.GetIndex()].insert(dep);
        }
    }
    for (auto& [tag, _] : pass.GetInBufferResources())
    {
        auto& input = m_resources[m_resourceToIndex[tag]];
        for (auto& dep : input->GetWrittenInPasses())
        {
            m_passDeps[pass.GetIndex()].insert(dep);
        }
    }
    level++;
    if (!m_passDeps.empty())
    {
        for (auto& index : m_passDeps[pass.GetIndex()])
        {
            m_sortedPassIndices.push_back(index);
            TraversePassDepsRecursive(index, level);
        }
    }
}

void RenderGraph::RemoveDuplicates(std::vector<Index>& list)
{
    std::unordered_set<Index> visited;
    auto tail = list.begin();
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
    HashMap<Tag, val::BufferUsage> lastBufferUsages;
    HashMap<Tag, val::ImageUsage> lastImageUsages;
    for (const auto& pass : m_passes)
    {
        auto& bufferTransitions = m_resourceState.perPassBufferState[pass->GetTag()];
        auto& imageTransitions  = m_resourceState.perPassImageState[pass->GetTag()];
        for (auto& [inputTag, usage] : pass->GetInBufferResources())
        {
            if (lastBufferUsages.find(inputTag) == lastBufferUsages.end())
            {
                m_resourceState.bufferFirstUsePass[inputTag] = pass->GetTag();
                // set at the end
                lastBufferUsages[inputTag] = val::BufferUsage::MaxEnum;
            }
            bufferTransitions[inputTag].srcUsage = lastBufferUsages[inputTag];
            bufferTransitions[inputTag].dstUsage = usage;
            m_resourceState.totalBufferUsages[inputTag] |= usage;
            lastBufferUsages[inputTag] = usage;

            m_resourceState.bufferLastUsePass[inputTag] = pass->GetTag();
        }
        for (auto& output : pass->GetOutBufferResources())
        {
            if (lastBufferUsages.find(output->GetTag()) == lastBufferUsages.end())
            {
                m_resourceState.bufferFirstUsePass[output->GetTag()] = pass->GetTag();
                // set at the end
                lastBufferUsages[output->GetTag()] = val::BufferUsage::MaxEnum;
            }
            bufferTransitions[output->GetTag()].srcUsage = lastBufferUsages[output->GetTag()];
            bufferTransitions[output->GetTag()].dstUsage = output->GetUsage();
            m_resourceState.totalBufferUsages[output->GetTag()] |= output->GetUsage();
            lastBufferUsages[output->GetTag()] = output->GetUsage();

            m_resourceState.bufferLastUsePass[output->GetTag()] = pass->GetTag();
        }
        for (auto& [inputTag, usage] : pass->GetInImageResources())
        {
            if (lastImageUsages.find(inputTag) == lastImageUsages.end())
            {
                m_resourceState.imageFirstUsePass[inputTag] = pass->GetTag();
                // set at the end
                lastImageUsages[inputTag] = val::ImageUsage::MaxEnum;
            }
            imageTransitions[inputTag].srcUsage = lastImageUsages[inputTag];
            imageTransitions[inputTag].dstUsage = usage;
            m_resourceState.totalImageUsages[inputTag] |= usage;
            lastImageUsages[inputTag] = usage;

            m_resourceState.imageLastUsePass[inputTag] = pass->GetTag();
        }
        for (auto& output : pass->GetOutImageResources())
        {
            if (lastImageUsages.find(output->GetTag()) == lastImageUsages.end())
            {
                m_resourceState.imageFirstUsePass[output->GetTag()] = pass->GetTag();
                // set at the end
                lastImageUsages[output->GetTag()] = val::ImageUsage::MaxEnum;
            }
            imageTransitions[output->GetTag()].srcUsage = lastImageUsages[output->GetTag()];
            imageTransitions[output->GetTag()].dstUsage = output->GetUsage();
            m_resourceState.totalImageUsages[output->GetTag()] |= output->GetUsage();
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
    imageCI.usage       = m_resourceState.totalImageUsages[image->GetTag()];
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
    m_physicalImages.emplace_back(m_renderDevice.CreateImageUnique(imageCI));
    m_physicalImages.back()->SetObjectDebugName(image->GetTag());
}

void RenderGraph::BuildPhysicalBuffer(RDGBuffer* buffer)
{
    const auto& info = buffer->GetInfo();

    val::BufferCreateInfo bufferCI{};
    bufferCI.byteSize = info.size;
    bufferCI.usage    = m_resourceState.totalBufferUsages[buffer->GetTag()];
    bufferCI.vmaFlags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT; // TODO: Choose right vma flags;

    auto physicalIndex = m_physicalBuffers.size();
    buffer->SetPhysicalIndex(physicalIndex);
    m_physicalBuffers.emplace_back(m_renderDevice.CreateBufferUnique(bufferCI));
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

// TODO: implement physical pass resources caching and management (Implementing in RenderDevice is a possible solution)
void RenderGraph::BuildPhysicalPasses()
{
    uint32_t currIndex = 0;
    for (auto& pass : m_passes)
    {
        RDGPhysicalPass physicalPass{};
        // Render pass attachments
        std::vector<VkAttachmentDescription> attachmentDescriptions;
        // val::SubpassInfos
        std::vector<VkAttachmentReference> colorReferences;
        uint32_t depthRefIndex{UINT32_MAX};
        // Framebuffer attachments
        std::vector<VkImageView> imageViews;
        // Framebuffer size
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
                attachmentDescription.format = rdgImage->GetInfo().format;
                attachmentDescription.samples =
                    static_cast<VkSampleCountFlagBits>(rdgImage->GetInfo().samples);
                attachmentDescription.loadOp =
                    VK_ATTACHMENT_LOAD_OP_CLEAR; // TODO: set proper load op
                attachmentDescription.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
                attachmentDescription.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                attachmentDescription.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
                attachmentDescription.finalLayout =
                    val::Image::UsageToImageLayout(imageTransition.at(rdgImage->GetTag()).dstUsage);
                attachmentDescriptions.push_back(attachmentDescription);

                VkAttachmentReference attachmentReference{};
                attachmentReference.attachment = attIndex;
                attachmentReference.layout     = attachmentDescription.finalLayout;
                if (rdgImage->GetUsage() == val::ImageUsage::DepthStencilAttachment)
                {
                    depthRefIndex = attIndex;
                }
                else
                {
                    colorReferences.push_back(attachmentReference);
                }
            }
        }
        // Create RenderPass
        val::SubpassInfo subpassInfo{colorReferences, {}, depthRefIndex};
        physicalPass.renderPass =
            m_renderDevice.RequestRenderPass(attachmentDescriptions, subpassInfo);
        // Create framebuffer
        physicalPass.framebuffer =
            m_renderDevice.RequestFramebuffer(physicalPass.renderPass->GetHandle(), imageViews,
                                              {framebufferWidth, framebufferHeight, 1});
        // Create pipeline
        physicalPass.pipelineLayout = m_renderDevice.RequestPipelineLayout(pass->GetUsedShaders());

        physicalPass.pipelineState.SetRenderPass(physicalPass.renderPass->GetHandle());

        physicalPass.graphicPipeline = m_renderDevice.RequestGraphicsPipeline(
            *physicalPass.pipelineLayout, physicalPass.pipelineState);

        auto& dsLayouts = physicalPass.pipelineLayout->GetDescriptorSetLayouts();
        physicalPass.descriptorSets.resize(dsLayouts.size());
        for (const auto& dsLayout : dsLayouts)
        {
            physicalPass.descriptorSets[dsLayout.GetSetIndex()] =
                m_renderDevice.RequestDescriptorSet(dsLayout);
        }

        physicalPass.index     = currIndex;
        physicalPass.onExecute = std::move(pass->GetOnExecute());
        pass->SetPhysicalIndex(currIndex);
        m_physicalPasses.push_back(physicalPass);
        currIndex++;
    }
}

static bool HasImageWriteDependency(val::ImageUsage usage)
{
    switch (usage)
    {
        case val::ImageUsage::TransferSrc:
        case val::ImageUsage::Storage:
        case val::ImageUsage::ColorAttachment:
        case val::ImageUsage::DepthStencilAttachment: return true;
        default: break;
    }
    return false;
}

static bool HasBufferWriteDependency(val::BufferUsage usage)
{
    switch (usage)
    {
        case val::BufferUsage::TransferDst:
        case val::BufferUsage::StorageBuffer:
        case val::BufferUsage::UniformBuffer: return true;
        default: break;
    }
    return false;
}

void RenderGraph::EmitPipelineBarrier(val::CommandBuffer* commandBuffer,
                                      const HashMap<Tag, ImageTransition>& imageTransitions,
                                      const HashMap<Tag, BufferTransition>& bufferTransitions)
{
    VkPipelineStageFlags srcPipelineStageFlags{};
    VkPipelineStageFlags dstPipelineStageFlags{};

    std::vector<VkBufferMemoryBarrier> bufferMemBarriers;
    std::vector<VkImageMemoryBarrier> imageMemBarriers;

    for (const auto& [bufferTag, bufferTransition] : bufferTransitions)
    {
        if (bufferTransition.srcUsage == bufferTransition.dstUsage &&
            !HasBufferWriteDependency(bufferTransition.srcUsage))
            continue;
        srcPipelineStageFlags |= val::Buffer::UsageToPipelineStage(bufferTransition.srcUsage);
        dstPipelineStageFlags |= val::Buffer::UsageToPipelineStage(bufferTransition.dstUsage);
        auto physicalIndex = m_resources[m_resourceToIndex[bufferTag]]->GetPhysicalIndex();

        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.buffer              = m_physicalBuffers[physicalIndex]->GetHandle();
        barrier.srcAccessMask       = val::Buffer::UsageToAccessFlags(bufferTransition.srcUsage);
        barrier.dstAccessMask       = val::Buffer::UsageToAccessFlags(bufferTransition.dstUsage);
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.size                = VK_WHOLE_SIZE;
        barrier.offset              = 0;
        bufferMemBarriers.push_back(barrier);
    }

    for (const auto& [imageTag, imageTransition] : imageTransitions)
    {
        if (imageTransition.srcUsage == imageTransition.dstUsage &&
            !HasImageWriteDependency(imageTransition.srcUsage))
            continue;
        srcPipelineStageFlags |= val::Image::UsageToPipelineStage(imageTransition.srcUsage);
        dstPipelineStageFlags |= val::Image::UsageToPipelineStage(imageTransition.dstUsage);
        auto physicalIndex = m_resources[m_resourceToIndex[imageTag]]->GetPhysicalIndex();

        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image               = m_physicalImages[physicalIndex]->GetHandle();
        barrier.srcAccessMask       = val::Image::UsageToAccessFlags(imageTransition.srcUsage);
        barrier.dstAccessMask       = val::Image::UsageToAccessFlags(imageTransition.dstUsage);
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.oldLayout           = val::Image::UsageToImageLayout(imageTransition.srcUsage);
        barrier.newLayout           = val::Image::UsageToImageLayout(imageTransition.dstUsage);
        barrier.subresourceRange    = m_physicalImages[physicalIndex]->GetSubResourceRange();
        imageMemBarriers.push_back(barrier);
    }
    if (bufferMemBarriers.empty() && imageMemBarriers.empty())
        return;

    if (!bufferMemBarriers.empty() || !imageMemBarriers.empty())
    {
        commandBuffer->PipelineBarrier(srcPipelineStageFlags, dstPipelineStageFlags,
                                       bufferMemBarriers, imageMemBarriers);
    }
}

/**
 * @brief handle image layout transitions from undefined to first usage layout
 */
void RenderGraph::BeforeExecuteSetup(val::CommandBuffer* commandBuffer)
{
    HashMap<Tag, ImageTransition> imageTransitions;
    for (const auto& pass : m_passes)
    {
        for (const auto& rdgImage : pass->GetOutImageResources())
        {
            auto& transition =
                m_resourceState.perPassImageState[pass->GetTag()][rdgImage->GetTag()];
            if (m_resourceState.imageFirstUsePass.at(rdgImage->GetTag()) == pass->GetTag())
            {
                imageTransitions[rdgImage->GetTag()] =
                    ImageTransition{val::ImageUsage::Undefined, transition.srcUsage};
            }
        }
    }
    EmitPipelineBarrier(commandBuffer, imageTransitions, {});
}

void RenderGraph::CopyToPresentImage(val::CommandBuffer* commandBuffer,
                                     const val::Image& presentImage)
{
    const auto& firstRenderPassTag = m_resourceState.imageFirstUsePass.at(m_backBufferTag);
    const auto& lastRenderPassTag  = m_resourceState.imageLastUsePass.at(m_backBufferTag);

    auto lastUsage = m_resourceState.perPassImageState[lastRenderPassTag][m_backBufferTag].dstUsage;
    auto firstUsage =
        m_resourceState.perPassImageState[firstRenderPassTag][m_backBufferTag].srcUsage;

    auto backBufferImageIndex = m_resources[m_resourceToIndex[m_backBufferTag]]->GetPhysicalIndex();
    auto& backBufferImage     = m_physicalImages.at(backBufferImageIndex);
    commandBuffer->BlitImage(*backBufferImage, lastUsage, presentImage, val::ImageUsage::Undefined);
    // After blit, the back buffer image's layout has been changed
    // In order to use it in the 'next' render pass, we need to transfer the layout back to its first usage TODO: is this right?
    if (firstUsage != val::ImageUsage::TransferSrc)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image               = backBufferImage->GetHandle();
        barrier.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout           = val::Image::UsageToImageLayout(firstUsage);
        barrier.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask       = val::Image::UsageToAccessFlags(firstUsage);
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange    = backBufferImage->GetSubResourceRange();

        commandBuffer->PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       val::Image::UsageToPipelineStage(firstUsage), {}, {barrier});
    }
}

void RenderGraph::UpdateDescriptorSets(RDGPhysicalPass& pass)
{
    // do not support dynamic descriptors for now, only update once
    if (pass.descriptorSetsUpdated)
        return;
    auto& rdgPass = m_passes[pass.index];
    // update descriptors
    std::vector<VkWriteDescriptorSet> dsWrites;
    std::vector<VkDescriptorBufferInfo> dsBufferInfos;
    std::vector<VkDescriptorImageInfo> dsImageInfos;

    size_t dsBufferInfoCount = 0;
    size_t dsImageInfoCount  = 0;
    for (const auto& [tag, shaderRes] : rdgPass->GetSRDBinding())
    {
        if (shaderRes.type == val::ShaderResourceType::ImageSampler ||
            shaderRes.type == val::ShaderResourceType::Image)
        {
            dsImageInfoCount++;
        }
        if (shaderRes.type == val::ShaderResourceType::BufferStorage ||
            shaderRes.type == val::ShaderResourceType::BufferUniform)
        {
            dsBufferInfoCount++;
        }
    }
    dsBufferInfos.reserve(dsBufferInfoCount);
    dsImageInfos.reserve(dsImageInfoCount);

    //    dsBufferInfos.reserve(rdgPass->GetSRDBinding().size());
    //    dsImageInfos.reserve(rdgPass->GetSRDBinding().size());
    for (const auto& [tag, shaderRes] : rdgPass->GetSRDBinding())
    {
        // TODO: add support for more descriptor types
        if (shaderRes.type == val::ShaderResourceType::ImageSampler)
        {
            bool isExternal = rdgPass->GetExternImageResources().count(tag) != 0;
            if (isExternal)
            {
                for (const auto* image : rdgPass->GetExternImageResources().at(tag))
                {
                    VkDescriptorImageInfo info{};
                    info.imageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
                    info.sampler     = rdgPass->GetSamplerBinding().at(tag)->GetHandle();
                    info.imageView   = image->GetView();
                    dsImageInfos.push_back(info);
                }
            }
            else
            {
                VkDescriptorImageInfo info{};
                info.imageLayout   = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
                info.sampler       = rdgPass->GetSamplerBinding().at(tag)->GetHandle();
                auto physicalIndex = m_resources[m_resourceToIndex[tag]]->GetPhysicalIndex();
                info.imageView     = m_physicalImages[physicalIndex]->GetView();
                dsImageInfos.push_back(info);
            }
            if (!dsImageInfos.empty())
            {
                VkWriteDescriptorSet newWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                newWrite.descriptorCount = dsImageInfos.size();
                newWrite.descriptorType  = val::ConvertToVkDescriptorType(
                    shaderRes.type, shaderRes.mode == val::ShaderResourceMode::Dynamic);
                newWrite.pImageInfo = &dsImageInfos[0];
                newWrite.dstBinding = shaderRes.binding;
                newWrite.dstSet     = pass.descriptorSets[shaderRes.set];
                dsWrites.push_back(newWrite);
            }
        }
        if (shaderRes.type == val::ShaderResourceType::BufferUniform ||
            shaderRes.type == val::ShaderResourceType::BufferStorage)
        {
            bool isExternal = rdgPass->GetExternBufferResources().count(tag) != 0;

            VkDescriptorBufferInfo info{};
            info.offset = 0;
            if (isExternal)
            {
                auto* buffer = rdgPass->GetExternBufferResources().at(tag);
                info.buffer  = buffer->GetHandle();
                info.range   = buffer->GetSize();
            }
            else
            {
                auto physicalIndex = m_resources[m_resourceToIndex[tag]]->GetPhysicalIndex();
                info.buffer        = m_physicalBuffers[physicalIndex]->GetHandle();
                info.range         = m_physicalBuffers[physicalIndex]->GetSize();
            }

            dsBufferInfos.push_back(info);

            if (!dsBufferInfos.empty())
            {
                VkWriteDescriptorSet newWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                newWrite.descriptorCount = 1;
                newWrite.descriptorType  = val::ConvertToVkDescriptorType(
                    shaderRes.type, shaderRes.mode == val::ShaderResourceMode::Dynamic);
                newWrite.pBufferInfo = &dsBufferInfos.back();
                newWrite.dstBinding  = shaderRes.binding;
                newWrite.dstSet      = pass.descriptorSets[shaderRes.set];
                dsWrites.push_back(newWrite);
            }
        }
    }
    m_renderDevice.UpdateDescriptorSets(dsWrites);
    pass.descriptorSetsUpdated = true;
}

void RenderGraph::RunPass(RDGPhysicalPass& pass, val::CommandBuffer* primaryCmdBuffer)
{
    UpdateDescriptorSets(pass);
    auto& rdgPass = m_passes[pass.index];
    // Before render
    EmitPipelineBarrier(primaryCmdBuffer, m_resourceState.perPassImageState[rdgPass->GetTag()],
                        m_resourceState.perPassBufferState[rdgPass->GetTag()]);
    // On render
    VkClearValue colorClear{{0.2f, 0.2f, 0.2f, 1.0f}};
    VkClearValue dsClear{1.0f, 0};
    VkClearValue clearValues[2] = {colorClear, dsClear};
    // Begin Pass
    if (pass.renderPass)
    {
        VkRenderPassBeginInfo rpBeginInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpBeginInfo.renderPass      = pass.renderPass->GetHandle();
        rpBeginInfo.framebuffer     = pass.framebuffer->GetHandle();
        rpBeginInfo.renderArea      = pass.framebuffer->GetRenderArea();
        rpBeginInfo.pClearValues    = clearValues;
        rpBeginInfo.clearValueCount = 2;

        // hard code subpass content here
        if (RenderConfig::GetInstance().useSecondaryCmd)
        {
            primaryCmdBuffer->BeginRenderPass(rpBeginInfo,
                                              VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        }
        else
        {
            primaryCmdBuffer->BeginRenderPass(rpBeginInfo);
        }
    }
    // Call function from outside
    pass.onExecute(primaryCmdBuffer);
    // End Pass
    if (pass.renderPass)
    {
        primaryCmdBuffer->EndRenderPass();
    }
}
} // namespace zen