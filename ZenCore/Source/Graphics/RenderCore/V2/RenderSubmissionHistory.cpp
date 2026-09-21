#include "Graphics/RenderCore/V2/RenderSubmissionHistory.h"
#include <algorithm>

namespace zen::rc
{
namespace
{
void IncludeId(HeapVector<uint32_t>& ids, uint32_t id)
{
    if (std::find(ids.begin(), ids.end(), id) == ids.end())
    {
        ids.push_back(id);
    }
}
} // namespace

uint32_t RenderSubmissionHistory::IncludePoint(RenderSubmissionUpdate& update,
                                               const RHISubmissionPoint& point)
{
    uint32_t id = 0;
    while (id < update.externalPoints.size() && !(update.externalPoints[id] == point))
    {
        ++id;
    }
    if (id == update.externalPoints.size())
    {
        update.externalPoints.push_back(point);
    }
    return id;
}

void RenderSubmissionHistory::IncludeInitial(RenderSubmissionUpdate& update,
                                             uint64_t id,
                                             const RenderResourceUse& use,
                                             bool ordersReaders)
{
    if (use.point.IsValid())
    {
        RDGExternalQueueState initial;
        initial.resourceId     = id;
        initial.queue          = use.point.queue;
        initial.dependencyId   = IncludePoint(update, use.point);
        initial.hasAccessState = true;
        initial.access         = use.access;
        initial.ordersReaders  = ordersReaders;
        update.initialStates.push_back(initial);
    }
}

bool RenderSubmissionHistory::IncludeDependency(RenderSubmissionUpdate& update,
                                                RDGSubmissionGroup& group,
                                                const RHISubmissionPoint& point,
                                                const RHIQueueCapabilities& queues,
                                                bool shared,
                                                bool serializeReads)
{
    bool valid = true;
    if (point.IsValid())
    {
        RHISubmissionDependency resolved;
        const RHISubmissionPointStatus status = point.Resolve(resolved);
        const bool local                      = queues.AreQueuesShared(point.queue, group.queue);
        valid                                 = status != RHISubmissionPointStatus::eFailed &&
            (local || serializeReads || (queues.asyncSubmissionDependencies && shared));
        if (valid && point.state.Get() == update.state.Get())
        {
            valid = point.group <= group.id;
            if (valid && point.group != group.id)
            {
                IncludeId(group.predecessors, point.group);
            }
        }
        else if (valid)
        {
            valid = !point.state || point.state->IsQueued();
            if (valid)
            {
                const uint32_t id = IncludePoint(update, point);
                IncludeId(group.externalPredecessors, id);
            }
        }
    }
    return valid;
}

void RenderSubmissionHistory::MergeUse(RenderResourceUse& use,
                                       const RHISubmissionPoint& point,
                                       const RDGAccess& access)
{
    const RDGAccess previous    = use.access;
    const bool compatibleLayout = previous.textureUsage == access.textureUsage ||
        (previous.textureUsage != RHITextureUsage::eMax &&
         access.textureUsage != RHITextureUsage::eMax &&
         RHITextureUsageToLayout(previous.textureUsage) ==
             RHITextureUsageToLayout(access.textureUsage));
    const bool merge = use.point.IsValid() &&
        (use.point == point ||
         (use.point.queue == point.queue && previous.accessMode == RHIAccessMode::eRead &&
          access.accessMode == RHIAccessMode::eRead && compatibleLayout));
    use.point  = point;
    use.access = access;
    if (merge)
    {
        use.access.pipelineStages.SetFlag(previous.pipelineStages);
        use.access.accessFlags.SetFlag(previous.accessFlags);
        use.access.bufferUsage.SetFlag(previous.bufferUsage);
        if (previous.accessMode == RHIAccessMode::eReadWrite)
        {
            use.access.accessMode = RHIAccessMode::eReadWrite;
        }
    }
}

bool RenderSubmissionHistory::PrepareAccess(RenderSubmissionUpdate& update,
                                            RDGSubmissionGroup& group,
                                            const RenderSubmissionAccess& access,
                                            const RHIQueueCapabilities& queues,
                                            bool serializeReads) const
{
    bool valid = access.resourceId != 0 &&
        (access.access.accessMode == RHIAccessMode::eRead ||
         access.access.accessMode == RHIAccessMode::eReadWrite);
    if (valid)
    {
        const std::pair<HashMap<uint64_t, RenderResourceHistory>::iterator, bool> inserted =
            update.resources.try_emplace(access.resourceId);
        RenderResourceHistory& history = inserted.first->second;
        if (inserted.second)
        {
            const RenderResourceHistory* previous = Find(access.resourceId);
            if (previous != nullptr)
            {
                history = *previous;
                IncludeInitial(update, access.resourceId, history.writer, true);
                for (const RenderResourceUse& reader : history.readers)
                {
                    IncludeInitial(update, access.resourceId, reader, false);
                }
            }
        }
        const bool transition = access.texture &&
            RHITextureUsageToLayout(history.layoutUsage) !=
                RHITextureUsageToLayout(access.access.textureUsage);
        const bool writes = access.access.accessMode == RHIAccessMode::eReadWrite;
        valid = IncludeDependency(update, group, history.writer.point, queues, access.shared,
                                  serializeReads);
        if (writes || transition || !access.shared || serializeReads)
        {
            for (const RenderResourceUse& reader : history.readers)
            {
                valid = valid &&
                    IncludeDependency(update, group, reader.point, queues, access.shared,
                                      serializeReads);
            }
        }
        if (valid)
        {
            const RHISubmissionPoint point{group.queue, 0, update.state, group.id};
            if (writes || transition)
            {
                MergeUse(history.writer, point, access.access);
                for (RenderResourceUse& reader : history.readers)
                {
                    reader = {};
                }
            }
            if (!writes)
            {
                MergeUse(history.readers[size_t(group.queue)], point, access.access);
            }
            if (access.texture)
            {
                history.layoutUsage = access.access.textureUsage;
            }
        }
    }
    return valid;
}

bool RenderSubmissionHistory::Prepare(const RDGSchedule& schedule,
                                      VectorView<const HeapVector<RenderSubmissionAccess>> accesses,
                                      const RHIQueueCapabilities& queues,
                                      RenderSubmissionUpdate& update,
                                      bool serializeReads) const
{
    update          = {};
    update.schedule = schedule;
    update.revision = m_revision;
    bool valid      = accesses.size() == schedule.groups.size();
    HeapVector<RHICommandContextType> contexts;
    for (const RDGSubmissionGroup& group : schedule.groups)
    {
        valid &= group.id == contexts.size() && size_t(group.queue) < RHICompletionSet::kQueueCount;
        contexts.push_back(group.queue);
    }
    if (valid)
    {
        update.state = MakeRefCountPtr<RHISubmissionState>(contexts);
        for (RDGSubmissionGroup& group : update.schedule.groups)
        {
            group.externalPredecessors.clear();
            for (uint32_t predecessor : group.predecessors)
            {
                valid &= predecessor < group.id;
            }
            for (const RenderSubmissionAccess& access : accesses[group.id])
            {
                valid = valid && PrepareAccess(update, group, access, queues, serializeReads);
            }
            std::sort(group.predecessors.begin(), group.predecessors.end());
        }
    }
    update.prepared = valid;
    return valid;
}

bool RenderSubmissionHistory::CanCommit(const RenderSubmissionUpdate& update) const
{
    bool valid = update.prepared && update.revision == m_revision && update.state;
    for (const RHISubmissionPoint& point : update.externalPoints)
    {
        RHISubmissionDependency resolved;
        const RHISubmissionPointStatus status = point.Resolve(resolved);
        valid &= status == RHISubmissionPointStatus::ePending ||
            status == RHISubmissionPointStatus::eAccepted;
    }
    return valid;
}

bool RenderSubmissionHistory::Commit(RenderSubmissionUpdate& update)
{
    const bool valid = CanCommit(update) && update.state->IsQueued();
    if (valid)
    {
        for (HashMap<uint64_t, RenderResourceHistory>::value_type& entry : update.resources)
        {
            m_resources[entry.first] = std::move(entry.second);
        }
        ++m_revision;
        update.prepared = false;
    }
    return valid;
}

bool RenderSubmissionHistory::ResolveUse(RenderResourceUse& use)
{
    RHISubmissionDependency accepted;
    const RHISubmissionPointStatus status = use.point.Resolve(accepted);
    const bool valid                      = status != RHISubmissionPointStatus::eFailed;
    if (status == RHISubmissionPointStatus::eAccepted && accepted.serial != 0 && use.point.state &&
        use.point.state->IsSubmissionFinished())
    {
        use.point = {accepted.queue, accepted.serial};
    }
    return valid;
}

bool RenderSubmissionHistory::ResolveAccepted()
{
    bool valid = true;
    for (HashMap<uint64_t, RenderResourceHistory>::value_type& entry : m_resources)
    {
        valid &= ResolveUse(entry.second.writer);
        for (RenderResourceUse& reader : entry.second.readers)
        {
            valid &= ResolveUse(reader);
        }
    }
    if (!valid)
    {
        Clear();
    }
    return valid;
}

void RenderSubmissionHistory::Erase(uint64_t resourceId)
{
    m_resources.erase(resourceId);
    ++m_revision;
}

void RenderSubmissionHistory::Clear()
{
    m_resources.clear();
    ++m_revision;
}

const RenderResourceHistory* RenderSubmissionHistory::Find(uint64_t resourceId) const
{
    const HashMap<uint64_t, RenderResourceHistory>::const_iterator found =
        m_resources.find(resourceId);
    return found != m_resources.end() ? &found->second : nullptr;
}
} // namespace zen::rc
