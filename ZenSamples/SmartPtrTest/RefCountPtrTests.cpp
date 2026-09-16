#include <gtest/gtest.h>
#include "Graphics/RHI/RHIResource.h"
#include "Templates/HeapVector.h"
#include <array>
#include <future>
#include <thread>
#include <type_traits>

using namespace zen;

class DummyResource : public RefCounted
{};

class DummyResourceChild : public DummyResource
{};

TEST(RefCountPtr, basic)
{
    RefCountPtr<DummyResource> res1 = MakeRefCountPtr<DummyResource>();

    EXPECT_EQ(res1.GetRefCount(), 1);

    RefCountPtr<DummyResource> res2 = res1;
    EXPECT_EQ(res2.GetRefCount(), 2);

    RefCountPtr<DummyResource> res3 = std::move(res2);
    EXPECT_EQ(res3.GetRefCount(), 2);

    RefCountPtr<DummyResourceChild> res4 = MakeRefCountPtr<DummyResourceChild>();
    RefCountPtr<DummyResource> res5(res4);
    EXPECT_EQ(res5.GetRefCount(), 2);
}

TEST(RefCountPtr, move)
{
    RefCountPtr<DummyResource> res1 = MakeRefCountPtr<DummyResource>();

    RefCountPtr<DummyResource> res2     = res1;
    RefCountPtr<DummyResource> movedRes = std::move(res2);

    EXPECT_EQ(movedRes.GetRefCount(), 2);
    EXPECT_EQ(res2.GetRefCount(), 0);
    EXPECT_EQ(res2.Get(), nullptr);
}

void addToVec(HeapVector<RefCountPtr<DummyResource>>& vec, RefCountPtr<DummyResource> item)
{
    vec.push_back(item);
}

TEST(RefCountPtr, vector)
{
    RefCountPtr<DummyResource> res1 = MakeRefCountPtr<DummyResource>();
    RefCountPtr<DummyResource> res2 = res1;
    {
        HeapVector<RefCountPtr<DummyResource>> vec;
        vec.push_back(res1);
        vec.push_back(res2);
        EXPECT_EQ(res1.GetRefCount(), 4);
    }
    {
        HeapVector<RefCountPtr<DummyResource>> vec;
        RefCountPtr<DummyResource> res3 = MakeRefCountPtr<DummyResource>();
        addToVec(vec, res3);
        EXPECT_EQ(res3.GetRefCount(), 2);
    }
}

namespace
{
class CountedObject : public RefCounted
{
public:
    explicit CountedObject(uint32_t& destructions) : m_destructions(destructions) {}
    ~CountedObject() override
    {
        ++m_destructions;
    }

    uint32_t value{42};

private:
    uint32_t& m_destructions;
};

struct OtherBase
{
    virtual ~OtherBase() = default;
    uint64_t padding{0};
};

class DerivedCountedObject : public OtherBase, public CountedObject
{
public:
    using CountedObject::CountedObject;
};

static_assert(sizeof(RefCountPtr<CountedObject>) == sizeof(CountedObject*));
static_assert(
    !std::is_constructible_v<RefCountPtr<DerivedCountedObject>, RefCountPtr<CountedObject>>);
static_assert(
    !std::is_assignable_v<RefCountPtr<DerivedCountedObject>&, RefCountPtr<CountedObject>>);
static_assert(!std::is_convertible_v<RefCountPtr<CountedObject>, CountedObject*>);
static_assert(std::is_same_v<decltype(&std::declval<RefCountPtr<CountedObject>&>()),
                             RefCountPtr<CountedObject>*>);

class ConcurrentObject : public RefCounted
{
public:
    ConcurrentObject(std::atomic<uint32_t>& destructions, std::atomic<uint32_t>& total) :
        m_destructions(destructions), m_total(total)
    {}

    ~ConcurrentObject() override
    {
        uint32_t total = 0;
        for (uint32_t value : values)
        {
            total += value;
        }
        m_total.store(total);
        ++m_destructions;
    }

    std::array<uint32_t, 8> values{};

private:
    std::atomic<uint32_t>& m_destructions;
    std::atomic<uint32_t>& m_total;
};

void ExerciseRefCount(RefCountPtr<ConcurrentObject> owner,
                      std::shared_future<void> start,
                      uint32_t index)
{
    start.wait();
    owner->values[index] = index + 1;
    for (uint32_t i = 0; i < 2000; ++i)
    {
        RefCountPtr<ConcurrentObject> copy(owner);
        RefCountPtr<ConcurrentObject> moved(std::move(copy));
        copy = moved;
        moved.Reset();
    }
}

class PolicyResource : public RHIResource
{
public:
    PolicyResource(uint32_t& destructions, std::thread::id& destructionThread) :
        RHIResource(RHIResourceType::eSampler),
        m_destructions(destructions),
        m_destructionThread(destructionThread)
    {}

private:
    void Init() override {}

    void Destroy() override
    {
        ++m_destructions;
        m_destructionThread = std::this_thread::get_id();
        delete this;
    }

    uint32_t& m_destructions;
    std::thread::id& m_destructionThread;
};

static_assert(sizeof(RHIResourcePtr<PolicyResource>) == sizeof(PolicyResource*));
static_assert(
    !std::is_constructible_v<RefCountPtr<PolicyResource>, RHIResourcePtr<PolicyResource>>);
} // namespace

TEST(RefCountPtr, SelfAssignmentAndResetPreserveTheSoleOwner)
{
    uint32_t destructions            = 0;
    RefCountPtr<CountedObject> owner = MakeRefCountPtr<CountedObject>(destructions);
    owner                            = owner;
    owner                            = std::move(owner);
    owner                            = owner.Get();
    owner.Reset(owner.Get());
    ASSERT_TRUE(owner);
    EXPECT_EQ(owner.GetRefCount(), 1u);
    EXPECT_EQ((*owner).value, 42u);
    EXPECT_EQ(destructions, 0u);
    owner.Reset();
    EXPECT_EQ(owner, nullptr);
    EXPECT_EQ(destructions, 1u);
    owner.Reset();
    EXPECT_EQ(destructions, 1u);
}

TEST(RefCountPtr, MoveAssignmentReleasesThePreviousOwner)
{
    uint32_t destructions             = 0;
    RefCountPtr<CountedObject> first  = MakeRefCountPtr<CountedObject>(destructions);
    RefCountPtr<CountedObject> second = MakeRefCountPtr<CountedObject>(destructions);
    second                            = std::move(first);
    EXPECT_EQ(first, nullptr);
    EXPECT_EQ(second.GetRefCount(), 1u);
    EXPECT_EQ(destructions, 1u);
    second = nullptr;
    EXPECT_EQ(destructions, 2u);
}

TEST(RefCountPtr, ConvertedCopiesAndMovesPreserveAdjustedPointersAndOwnership)
{
    uint32_t destructions                     = 0;
    RefCountPtr<DerivedCountedObject> derived = MakeRefCountPtr<DerivedCountedObject>(destructions);
    CountedObject* expected                   = derived.Get();
    RefCountPtr<CountedObject> copied(derived);
    RefCountPtr<CountedObject> moved(std::move(derived));
    EXPECT_EQ(derived, nullptr);
    EXPECT_EQ(copied, moved);
    EXPECT_EQ(moved.Get(), expected);
    EXPECT_EQ(moved.GetRefCount(), 2u);
    copied.Reset();

    RefCountPtr<DerivedCountedObject> next = MakeRefCountPtr<DerivedCountedObject>(destructions);
    moved                                  = next;
    EXPECT_EQ(destructions, 1u);
    EXPECT_EQ(next.GetRefCount(), 2u);
    moved = std::move(next);
    EXPECT_EQ(next, nullptr);
    EXPECT_EQ(moved.GetRefCount(), 1u);

    RefCountPtr<const CountedObject> readOnly = std::move(moved);
    EXPECT_EQ(moved, nullptr);
    EXPECT_EQ(readOnly.GetRefCount(), 1u);
    EXPECT_EQ(readOnly->value, 42u);
    readOnly.Reset();
    EXPECT_EQ(destructions, 2u);
}

TEST(RefCountPtr, DetachAndAdoptTransferAnExistingReference)
{
    uint32_t destructions               = 0;
    RefCountPtr<CountedObject> original = MakeRefCountPtr<CountedObject>(destructions);
    CountedObject* raw                  = original.Detach();
    EXPECT_EQ(original, nullptr);
    EXPECT_EQ(raw->GetRefCount(), 1u);
    RefCountPtr<CountedObject> adopted = RefCountPtr<CountedObject>::Adopt(raw);
    EXPECT_EQ(adopted.GetRefCount(), 1u);
    RefCountPtr<CountedObject> empty;
    adopted.Swap(empty);
    EXPECT_EQ(adopted, nullptr);
    EXPECT_EQ(empty.Get(), raw);
    empty.Reset();
    EXPECT_EQ(destructions, 1u);
    EXPECT_EQ(RefCountPtr<CountedObject>::Adopt(nullptr), nullptr);
}

TEST(RefCountPtr, ConcurrentFinalReleaseObservesAllOwnersAndDestroysOnce)
{
    std::atomic<uint32_t> destructions{0};
    std::atomic<uint32_t> total{0};
    RefCountPtr<ConcurrentObject> owner = MakeRefCountPtr<ConcurrentObject>(destructions, total);
    std::promise<void> start;
    const std::shared_future<void> ready = start.get_future().share();
    HeapVector<std::thread> workers;
    workers.reserve(8);
    for (uint32_t i = 0; i < 8; ++i)
    {
        workers.emplace_back(ExerciseRefCount, owner, ready, i);
    }
    owner.Reset();
    EXPECT_EQ(destructions.load(), 0u);
    start.set_value();
    for (std::thread& worker : workers)
    {
        worker.join();
    }
    EXPECT_EQ(destructions.load(), 1u);
    EXPECT_EQ(total.load(), 36u);
}

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
    EXPECT_EQ(destructions, 1u);
    EXPECT_EQ(destructionThread, worker);
    GetRHIThread().Stop();
}
