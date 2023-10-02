#include "Graphics/Rendering/RenderGraph.h"
#include "Common/Errors.h"
#include "Common/Helpers.h"
#include "Graphics/Rendering/RenderContext.h"
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

void RDGPass::BindSRD(const Tag& rdgResourceTag, VkShaderStageFlagBits shaderStage, const std::string& shaderResourceName)
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
    m_resourceState.totalUsages[tag] |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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
    imageCI.usage       = m_resourceState.totalUsages[image->GetTag()];
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
    bufferCI.usage    = info.usage;
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
    for (const auto& pass : m_passes)
    {
        PhysicalPass physicalPass{};
        // Render pass attachments
        std::vector<VkAttachmentDescription> attachmentDescriptions;
        // val::SubpassInfos
        std::vector<VkAttachmentReference> colorReferences;
        uint32_t                           depthRefIndex{UINT32_MAX};
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
                attachmentDescription.format         = rdgImage->GetInfo().format;
                attachmentDescription.samples        = static_cast<VkSampleCountFlagBits>(rdgImage->GetInfo().samples);
                attachmentDescription.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR; // TODO: set proper load op
                attachmentDescription.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
                attachmentDescription.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                attachmentDescription.initialLayout  = val::Image::UsageToImageLayout(imageTransition.at(rdgImage->GetTag()).srcUsage);
                attachmentDescription.finalLayout    = val::Image::UsageToImageLayout(imageTransition.at(rdgImage->GetTag()).dstUsage);
                attachmentDescriptions.push_back(attachmentDescription);

                VkAttachmentReference attachmentReference{};
                attachmentReference.attachment = attIndex;
                attachmentReference.layout     = attachmentDescription.finalLayout;
                if (rdgImage->GetUsage() & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
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
        physicalPass.renderPass = m_renderDevice.RequestRenderPass(attachmentDescriptions, subpassInfo);
        // Create framebuffer
        physicalPass.framebuffer = m_renderDevice.CreateFramebufferUnique(physicalPass.renderPass->GetHandle(), imageViews, {framebufferWidth, framebufferHeight, 1});
        // Create pipeline
        physicalPass.pipelineLayout = m_renderDevice.RequestPipelineLayout(pass->GetUsedShaders());

        physicalPass.pipelineState.SetRenderPass(physicalPass.renderPass->GetHandle());

        physicalPass.graphicPipeline = m_renderDevice.RequestGraphicsPipeline(*physicalPass.pipelineLayout, physicalPass.pipelineState);
        for (const auto& dsLayout : physicalPass.pipelineLayout->GetDescriptorSetLayouts())
        {
            physicalPass.descriptorSets.push_back(m_renderDevice.RequestDescriptorSet(dsLayout));
        }
        physicalPass.index     = currIndex;
        physicalPass.onExecute = std::move(pass->GetOnExecute());
        pass->SetPhysicalIndex(currIndex);
        m_physicalPasses.push_back(physicalPass);
        currIndex++;
    }
}

static bool HasImageWriteDependency(VkImageUsageFlags usage)
{
    if (usage & (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
        return true;
    return false;
}

static bool HasBufferWriteDependency(VkBufferUsageFlags usage)
{
    if (usage & (VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
        return true;
    return false;
}

void RenderGraph::EmitPipelineBarrier(val::CommandBuffer* commandBuffer, const std::unordered_map<Tag, ImageTransition>& imageTransitions, const std::unordered_map<Tag, BufferTransition>& bufferTransitions)
{
    VkPipelineStageFlags srcPipelineStageFlags{};
    VkPipelineStageFlags dstPipelineStageFlags{};

    std::vector<VkBufferMemoryBarrier> bufferMemBarriers;
    std::vector<VkImageMemoryBarrier>  imageMemBarriers;

    for (const auto& [bufferTag, bufferTransition] : bufferTransitions)
    {
        if (bufferTransition.srcUsage == bufferTransition.dstUsage && !HasBufferWriteDependency(bufferTransition.srcUsage))
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
        if (imageTransition.srcUsage == imageTransition.dstUsage && !HasImageWriteDependency(imageTransition.srcUsage))
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
        commandBuffer->PipelineBarrier(srcPipelineStageFlags, dstPipelineStageFlags, bufferMemBarriers, imageMemBarriers);
    }
}

/**
 * @brief handle image layout transitions from undefined to first usage layout
 */
void RenderGraph::BeforeExecuteSetup(val::CommandBuffer* commandBuffer)
{
    std::unordered_map<Tag, ImageTransition> imageTransitions;
    for (const auto& pass : m_passes)
    {
        for (const auto& rdgImage : pass->GetOutImageResources())
        {
            auto& transition = m_resourceState.perPassImageState[pass->GetTag()][rdgImage->GetTag()];
            if (m_resourceState.imageFirstUsePass.at(rdgImage->GetTag()) == pass->GetTag())
            {
                imageTransitions[rdgImage->GetTag()] = ImageTransition{
                    0,
                    transition.srcUsage};
            }
        }
    }
    EmitPipelineBarrier(commandBuffer, imageTransitions, {});
}

void RenderGraph::CopyToPresentImage(val::CommandBuffer* commandBuffer, const val::Image& presentImage)
{
    const auto& firstRenderPassTag = m_resourceState.imageFirstUsePass.at(m_backBufferTag);
    const auto& lastRenderPassTag  = m_resourceState.imageLastUsePass.at(m_backBufferTag);

    auto lastUsage  = m_resourceState.perPassImageState[lastRenderPassTag][m_backBufferTag].dstUsage;
    auto firstUsage = m_resourceState.perPassImageState[firstRenderPassTag][m_backBufferTag].srcUsage;

    auto  backBufferImageIndex = m_resources[m_resourceToIndex[m_backBufferTag]]->GetPhysicalIndex();
    auto& backBufferImage      = m_physicalImages.at(backBufferImageIndex);
    commandBuffer->BlitImage(*backBufferImage, lastUsage, presentImage, 0);
    // After blit, the back buffer image's layout has been changed
    // In order to use it in the 'next' render pass, we need to transfer the layout back to its first usage TODO: is this right?
    if (firstUsage != VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
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

        commandBuffer->PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, val::Image::UsageToPipelineStage(firstUsage), {}, {barrier});
    }
}

void RenderGraph::RunPass(RenderGraph::PhysicalPass& pass, val::CommandBuffer* commandBuffer)
{
    // update descriptors
    std::vector<VkWriteDescriptorSet>   dsWrites;
    std::vector<VkDescriptorBufferInfo> dsBufferInfos;
    std::vector<VkDescriptorImageInfo>  dsImageInfos;

    auto& rdgPass = m_passes[pass.index];
    for (const auto& [tag, shaderRes] : rdgPass->GetSRDBinding())
    {
        auto& rdgResource = m_resources[m_resourceToIndex[tag]];
        // TODO: add support for more descriptor types
        if (shaderRes.type == val::ShaderResourceType::ImageSampler)
        {
            VkDescriptorImageInfo info{};
            info.imageLayout = val::Image::UsageToImageLayout(rdgResource->As<RDGImage>()->GetUsage());
            info.imageView   = m_physicalImages[rdgResource->GetPhysicalIndex()]->GetView();
            info.sampler     = rdgPass->GetSamplerBinding().at(tag)->GetHandle();
            dsImageInfos.push_back(info);

            VkWriteDescriptorSet newWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            newWrite.descriptorCount = 1;
            newWrite.descriptorType  = val::ConvertToVkDescriptorType(shaderRes.type, shaderRes.mode == val::ShaderResourceMode::Dynamic);
            newWrite.pImageInfo      = &dsImageInfos.back();
            newWrite.dstBinding      = shaderRes.binding;
            newWrite.dstSet          = pass.descriptorSets[shaderRes.set];
            dsWrites.push_back(newWrite);
        }
        if (shaderRes.type == val::ShaderResourceType::BufferUniform || shaderRes.type == val::ShaderResourceType::BufferStorage)
        {
            VkDescriptorBufferInfo info{};
            info.buffer = m_physicalBuffers[rdgResource->GetPhysicalIndex()]->GetHandle();
            info.offset = 0;
            info.range  = m_physicalBuffers[rdgResource->GetPhysicalIndex()]->GetSize();
            dsBufferInfos.push_back(info);

            VkWriteDescriptorSet newWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            newWrite.descriptorCount = 1;
            newWrite.descriptorType  = val::ConvertToVkDescriptorType(shaderRes.type, shaderRes.mode == val::ShaderResourceMode::Dynamic);
            newWrite.pBufferInfo     = &dsBufferInfos.back();
            newWrite.dstBinding      = shaderRes.binding;
            newWrite.dstSet          = pass.descriptorSets[shaderRes.set];
            dsWrites.push_back(newWrite);
        }
    }
    m_renderDevice.UpdateDescriptorSets(dsWrites);
    // Before render
    EmitPipelineBarrier(commandBuffer, m_resourceState.perPassImageState[rdgPass->GetTag()], m_resourceState.perPassBufferState[rdgPass->GetTag()]);
    // On render
    VkClearValue clearValue{};
    clearValue.color = {{0.2f, 0.2f, 0.2f, 1.0f}};
    // Begin Pass
    if (pass.renderPass)
    {
        VkRenderPassBeginInfo rpBeginInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpBeginInfo.renderPass      = pass.renderPass->GetHandle();
        rpBeginInfo.framebuffer     = pass.framebuffer->GetHandle();
        rpBeginInfo.renderArea      = pass.framebuffer->GetRenderArea();
        rpBeginInfo.pClearValues    = &clearValue;
        rpBeginInfo.clearValueCount = 1;

        commandBuffer->BeginRenderPass(rpBeginInfo);
    }
    if (pass.graphicPipeline)
    {
        commandBuffer->BindGraphicPipeline(pass.graphicPipeline->GetHandle());
    }
    if (!pass.descriptorSets.empty())
    {
        commandBuffer->BindDescriptorSets(pass.pipelineLayout->GetHandle(), pass.descriptorSets);
    }
    // Call function from outside
    pass.onExecute(commandBuffer);
    // End Pass
    if (pass.renderPass)
    {
        commandBuffer->EndRenderPass();
    }
}
} // namespace zen