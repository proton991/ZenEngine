#include "Graphics/Rendering/RenderGraph.h"
#include "Common/Errors.h"
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
    m_outColorImages.push_back(res);
}

void RDGPass::ReadFromAttachment(const std::string& tag)
{
    auto* res = m_graph.GetImageResource(tag);
    res->AddImageUsage(VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
    res->ReadInPass(m_index);
    m_inAttachments.push_back(res);
}

void RDGPass::WriteToStorageBuffer(const std::string& tag, const RDGBuffer::Info& info)
{
    auto* res = m_graph.GetBufferResource(tag);
    res->AddBufferUsage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    res->WriteInPass(m_index);
    res->SetInfo(info);
    m_outStorageBuffers.push_back(res);
}

void RDGPass::ReadFromStorageBuffer(const std::string& tag)
{
    auto* res = m_graph.GetBufferResource(tag);
    res->AddBufferUsage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    res->ReadInPass(m_index);
    m_inStorageBuffers.push_back(res);
}

void RDGPass::WriteToDepthStencilImage(const std::string& tag, const RDGImage::Info& info)
{
    auto* res = m_graph.GetImageResource(tag);
    res->AddImageUsage(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    res->WriteInPass(m_index);
    res->SetInfo(info);
    m_outDepthStencil = res;
}

void RDGPass::ReadFromDepthStencilImage(const std::string& tag)
{
    auto* res = m_graph.GetImageResource(tag);
    res->AddImageUsage(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    res->ReadInPass(m_index);
    m_inDepthStencil = res;
}

RDGImage* RenderGraph::GetImageResource(const std::string& tag)
{
    auto iter = m_resourceToIndex.find(tag);
    if (iter != m_resourceToIndex.end())
    {
        VERIFY_EXPR(m_resources[iter->second]->GetType() == RDGResource::Type::Image);
        return dynamic_cast<RDGImage*>(m_resources[iter->second].Get());
    }
    else
    {
        Index index = m_resources.size();
        m_resources.emplace_back(new RDGImage(index, tag));
        m_resourceToIndex[tag] = index;
        return dynamic_cast<RDGImage*>(m_resources.back().Get());
    }
}

RDGBuffer* RenderGraph::GetBufferResource(const std::string& tag)
{
    auto iter = m_resourceToIndex.find(tag);
    if (iter != m_resourceToIndex.end())
    {
        VERIFY_EXPR(m_resources[iter->second]->GetType() == RDGResource::Type::Buffer);
        return dynamic_cast<RDGBuffer*>(m_resources[iter->second].Get());
    }
    else
    {
        Index index = m_resources.size();
        m_resources.emplace_back(new RDGImage(index, tag));
        m_resourceToIndex[tag] = index;
        return dynamic_cast<RDGBuffer*>(m_resources.back().Get());
    }
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
    if (pass.GetInDepthStencil())
    {
        for (auto& dep : pass.GetInDepthStencil()->GetWrittenInPasses())
        {
            m_passDeps[pass.GetIndex()].insert(dep);
        }
    }
    for (auto& input : pass.GetInAttachments())
    {
        for (auto& dep : input->GetWrittenInPasses())
        {
            m_passDeps[pass.GetIndex()].insert(dep);
        }
    }
    for (auto& input : pass.GetInStorageBuffers())
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
} // namespace zen