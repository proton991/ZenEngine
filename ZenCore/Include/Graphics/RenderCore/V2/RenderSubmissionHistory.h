#pragma once
#include "Graphics/RHI/RHICommandListExecutor.h"
#include "RenderGraph/RDGSchedule.h"
#include "Templates/HashMap.h"

namespace zen::rc
{
// Physical identities and access values only; no borrowed graph nodes or resource pointers.
struct RenderSubmissionAccess
{
    uint64_t resourceId{0};
    bool texture{false};
    bool shared{false};
    RDGAccess access;
};

struct RenderResourceUse
{
    RHISubmissionPoint point;
    RDGAccess access;
};

struct RenderResourceHistory
{
    RenderResourceUse writer; // Last write or layout transition; every reader depends on it.
    SmallVector<RenderResourceUse, RHICompletionSet::kQueueCount> readers =
        SmallVector<RenderResourceUse, RHICompletionSet::kQueueCount>(
            RHICompletionSet::kQueueCount);
    RHITextureUsage layoutUsage{RHITextureUsage::eNone};
};

struct RenderSubmissionUpdate
{
    RDGSchedule schedule;
    RefCountPtr<RHISubmissionState> state;
    HeapVector<RHISubmissionPoint> externalPoints;
    HeapVector<RDGExternalQueueState> initialStates;

private:
    friend class RenderSubmissionHistory;
    HashMap<uint64_t, RenderResourceHistory> resources;
    uint64_t revision{0};
    bool prepared{false};
};

// Render-thread state, owned by RenderDevice. Preparation is private until complete-batch handoff.
class RenderSubmissionHistory
{
public:
    bool Prepare(const RDGSchedule& schedule,
                 VectorView<const HeapVector<RenderSubmissionAccess>> accesses,
                 const RHIQueueCapabilities& queues,
                 RenderSubmissionUpdate& update,
                 bool serializeReads = false) const;
    bool CanCommit(const RenderSubmissionUpdate& update) const;
    bool Commit(RenderSubmissionUpdate& update);
    bool ResolveAccepted();
    void Erase(uint64_t resourceId);
    void Clear();
    const RenderResourceHistory* Find(uint64_t resourceId) const;

private:
    static uint32_t IncludePoint(RenderSubmissionUpdate& update, const RHISubmissionPoint& point);
    static void IncludeInitial(RenderSubmissionUpdate& update,
                               uint64_t id,
                               const RenderResourceUse& use,
                               bool ordersReaders);
    static bool IncludeDependency(RenderSubmissionUpdate& update,
                                  RDGSubmissionGroup& group,
                                  const RHISubmissionPoint& point,
                                  const RHIQueueCapabilities& queues,
                                  bool shared,
                                  bool serializeReads);
    static void MergeUse(RenderResourceUse& use,
                         const RHISubmissionPoint& point,
                         const RDGAccess& access);
    static bool ResolveUse(RenderResourceUse& use);
    bool PrepareAccess(RenderSubmissionUpdate& update,
                       RDGSubmissionGroup& group,
                       const RenderSubmissionAccess& access,
                       const RHIQueueCapabilities& queues,
                       bool serializeReads) const;

    HashMap<uint64_t, RenderResourceHistory> m_resources;
    uint64_t m_revision{0};
};
} // namespace zen::rc
