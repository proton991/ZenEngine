#pragma once
#include "Graphics/RHI/RHICommandListExecutor.h"
#include <algorithm>

namespace zen::rc
{
// Each ordered batch contributes its own serials once its CPU ticket resolves.
// Keep every unresolved ticket: a later batch may use a different native queue.
struct ResourceRetirement
{
    // Threaded rendering usually has several frame and upload batches outstanding.
    RHICompletionSet                    requiredSerials;
    SmallVector<RHISubmissionTicket, 4> pending;

    void Extend(const ResourceRetirement& other)
    {
        requiredSerials.Extend(other.requiredSerials);

        for (const RHISubmissionTicket& ticket : other.pending)
        {
            if (std::find(pending.begin(), pending.end(), ticket) == pending.end())
            {
                pending.push_back(ticket);
            }
        }
    }

    // Nonblocking: retain unresolved or fatal work, including uncertain native use.
    bool Resolve()
    {
        for (size_t i = 0; i < pending.size();)
        {
            if (Fold(pending[i], requiredSerials))
            {
                pending.erase(pending.begin() + i);
            }
            else
            {
                ++i;
            }
        }

        return pending.empty();
    }

    bool Wait()
    {
        for (const RHISubmissionTicket& ticket : pending)
        {
            ticket.Wait();
        }

        return Resolve();
    }

    // Read-only form of Resolve() for const owners.
    bool IsCompleteAt(const RHICompletionSet& completed) const
    {
        RHICompletionSet required = requiredSerials;

        const bool ready = std::all_of(pending.begin(), pending.end(),
                                       [&required](const RHISubmissionTicket& ticket) { return Fold(ticket, required); });

        return ready && required.IsCompleteAt(completed);
    }

private:
    // Adds a ready ticket's serials. True once it no longer needs to be retained.
    static bool Fold(const RHISubmissionTicket& ticket, RHICompletionSet& required)
    {
        bool resolved = false;

        if (ticket.IsReady())
        {
            const RHIBatchResult& result = ticket.Wait();

            required.Extend(result.requiredSerials);

            resolved = result.submission != RHISubmissionResult::eFatal;
        }

        return resolved;
    }
};
} // namespace zen::rc
