#pragma once
#include "Graphics/Val/Device.h"

namespace zen
{
class SynObjPool
{
public:
    SynObjPool(val::Device& device) :
        m_valDevice(device) {}

    ~SynObjPool();

    VkFence RequestFence();

    VkSemaphore RequestSemaphore();

    void ResetFences();

    void ResetSemaphores();

    void WaitForFences(uint32_t timeout = std::numeric_limits<uint32_t>::max()) const;

private:
    val::Device& m_valDevice;
    // Fences
    std::vector<VkFence> m_fences;
    uint32_t             m_numActiveFences{0};
    // Semaphores
    std::vector<VkSemaphore> m_semaphores;
    std::vector<VkSemaphore> m_releasedSemaphores;
    uint32_t                 m_numActiveSemaphores;
};
} // namespace zen