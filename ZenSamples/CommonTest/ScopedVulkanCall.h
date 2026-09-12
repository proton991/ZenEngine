#pragma once

namespace zen::test
{
template <typename T> class ScopedVulkanCall
{
public:
    ScopedVulkanCall(T& slot, T replacement) : slot(slot), previous(slot)
    {
        slot = replacement;
    }
    ~ScopedVulkanCall()
    {
        slot = previous;
    }
    ScopedVulkanCall(const ScopedVulkanCall&)            = delete;
    ScopedVulkanCall& operator=(const ScopedVulkanCall&) = delete;

private:
    T& slot;
    T previous;
};
} // namespace zen::test
