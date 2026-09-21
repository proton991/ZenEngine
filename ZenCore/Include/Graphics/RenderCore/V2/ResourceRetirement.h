#pragma once
#include "Graphics/RHI/RHICommandListExecutor.h"

namespace zen::rc
{
// RenderDevice permits one pending frame. A CPU ticket protects work until its
// native serials are known; the completion set still gates actual GPU retirement.
struct ResourceRetirement
{
    RHICompletionSet requiredSerials;
    RHISubmissionTicket pending;

    // Nonblocking: retain unresolved or fatal work, including uncertain native use.
    bool Resolve()
    {
        bool ready = !pending.IsValid();
        if (pending.IsReady())
        {
            const RHIBatchResult& result = pending.Wait();
            requiredSerials.Extend(result.requiredSerials);
            ready = result.submission != RHISubmissionResult::eFatal;
            if (ready)
            {
                pending = {};
            }
        }
        return ready;
    }

    bool IsCompleteAt(const RHICompletionSet& completed) const
    {
        ResourceRetirement resolved = *this;
        return resolved.Resolve() && resolved.requiredSerials.IsCompleteAt(completed);
    }
};
} // namespace zen::rc
