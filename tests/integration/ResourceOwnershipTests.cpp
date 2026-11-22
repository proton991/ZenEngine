#include "ZenRHI/RHIResource.h"
#include <gtest/gtest.h>
#include <thread>

using namespace zen;
namespace
{
class PolicyResource : public RHIResource
{
public:
    PolicyResource(uint32_t& destructions, std::thread::id& destructionThread) :
        RHIResource(RHIResourceType::eSampler), m_destructions(destructions), m_destructionThread(destructionThread)
    {}

private:
    void Init() override {}

    void Destroy() override
    {
        ++m_destructions;

        m_destructionThread = std::this_thread::get_id();

        delete this;
    }

    uint32_t&        m_destructions;
    std::thread::id& m_destructionThread;
};

static_assert(sizeof(RHIResourcePtr<PolicyResource>) == sizeof(PolicyResource*));
static_assert(!std::is_constructible_v<RefCountPtr<PolicyResource>, RHIResourcePtr<PolicyResource>>);
} // namespace

TEST(RefCountPtr, ResourcePolicyReusesTheExistingCounterAndRhiDestruction)
{
    uint32_t destructions = 0;

    std::thread::id destructionThread;

    GetRHIThread().Start(RHIExecutionMode::eThreaded);

    const std::thread::id worker = GetRHIThread().Invoke([] { return std::this_thread::get_id(); });

    PolicyResource* raw          = new PolicyResource(destructions, destructionThread);

    EXPECT_EQ(raw->GetRefCount(), 1u);

    RHIResourcePtr<PolicyResource> retained(raw);

    EXPECT_EQ(raw->GetRefCount(), 2u);

    RHIResourcePtr<RHIResource> copied(retained);

    RHIResourcePtr<RHIResource> moved(std::move(retained));

    EXPECT_EQ(retained, nullptr);

    EXPECT_EQ(raw->GetRefCount(), 3u);

    raw->ReleaseReference();

    copied.Reset();

    EXPECT_EQ(moved.GetRefCount(), 1u);

    EXPECT_EQ(destructions, 0u);

    moved.Reset();

    GetRHIThread().Flush();

    EXPECT_EQ(destructions, 1u);

    EXPECT_EQ(destructionThread, worker);

    GetRHIThread().Stop();
}
