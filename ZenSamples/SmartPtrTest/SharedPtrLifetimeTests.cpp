#include "Utils/SharedPtr.h"
#include "Templates/HeapVector.h"
#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <thread>

using namespace zen;

namespace
{
struct CountedValue
{
    explicit CountedValue(uint32_t& destructions) : destructions(destructions) {}
    ~CountedValue()
    {
        ++destructions;
    }
    uint32_t& destructions;
    int value{42};
};

struct BaseValue
{
    explicit BaseValue(uint32_t& destructions) : destructions(destructions) {}
    ~BaseValue()
    {
        ++destructions;
    }
    uint32_t& destructions;
};

struct DerivedValue : BaseValue
{
    DerivedValue(uint32_t& baseDestructions, uint32_t& derivedDestructions) :
        BaseValue(baseDestructions), derivedDestructions(derivedDestructions)
    {}
    ~DerivedValue()
    {
        ++derivedDestructions;
    }
    uint32_t& derivedDestructions;
};

struct CastBase
{
    virtual ~CastBase() = default;
};
struct CastDerived : CastBase
{};

void CopySharedOwner(SharedPtr<int, MultiThreadCounter> owner, std::shared_future<void> start)
{
    start.wait();
    for (uint32_t i = 0; i < 2000; ++i)
    {
        SharedPtr<int, MultiThreadCounter> copy(owner);
        SharedPtr<int, MultiThreadCounter> moved(std::move(copy));
        copy = moved;
        moved.Reset();
    }
}
} // namespace

TEST(SharedPtrLifetime, SelfAssignmentPreservesTheSoleOwner)
{
    uint32_t destructions         = 0;
    SharedPtr<CountedValue> owner = MakeShared<CountedValue>(destructions);
    owner                         = owner;
    EXPECT_EQ(owner.UseCount(), 1u);
    EXPECT_EQ(destructions, 0u);
    owner = std::move(owner);
    ASSERT_TRUE(owner);
    EXPECT_EQ(owner->value, 42);
    EXPECT_EQ(owner.UseCount(), 1u);
    owner.Reset();
    EXPECT_EQ(destructions, 1u);
}

TEST(SharedPtrLifetime, MoveAssignmentReleasesThePreviousOwner)
{
    uint32_t destructions          = 0;
    SharedPtr<CountedValue> first  = MakeShared<CountedValue>(destructions);
    SharedPtr<CountedValue> second = MakeShared<CountedValue>(destructions);
    second                         = std::move(first);
    EXPECT_EQ(destructions, 1u);
    EXPECT_EQ(first, nullptr);
    EXPECT_EQ(second.UseCount(), 1u);
    second.Reset();
    EXPECT_EQ(destructions, 2u);
}

TEST(SharedPtrLifetime, AliasedMemberRetainsTheOriginalObjectAndCustomDeleter)
{
    uint32_t destructions = 0;
    uint32_t deleterCalls = 0;
    CountedValue* object  = new CountedValue(destructions);
    SharedPtr<CountedValue> owner(object, [&deleterCalls](CountedValue* pointer) {
        ++deleterCalls;
        delete pointer;
    });
    SharedPtr<int> member(owner, &object->value);
    owner.Reset();
    EXPECT_EQ(*member, 42);
    EXPECT_EQ(member.UseCount(), 1u);
    EXPECT_EQ(deleterCalls, 0u);
    member.Reset();
    EXPECT_EQ(deleterCalls, 1u);
    EXPECT_EQ(destructions, 1u);
}

TEST(SharedPtrLifetime, NullAliasesStillRetainAndReleaseOwnership)
{
    uint32_t destructions         = 0;
    SharedPtr<CountedValue> owner = MakeShared<CountedValue>(destructions);
    SharedPtr<int> alias(owner, nullptr);
    SharedPtr<int> copy(alias);
    EXPECT_FALSE(alias);
    EXPECT_EQ(alias.UseCount(), 3u);
    owner.Reset();
    alias.Reset();
    EXPECT_EQ(copy.UseCount(), 1u);
    EXPECT_EQ(destructions, 0u);
    copy.Reset();
    EXPECT_EQ(destructions, 1u);
}

TEST(SharedPtrLifetime, ConvertedOwnerRetainsTheConcreteDestructor)
{
    uint32_t baseDestructions    = 0;
    uint32_t derivedDestructions = 0;
    SharedPtr<DerivedValue, MultiThreadCounter> derived =
        MakeShared<DerivedValue, MultiThreadCounter>(baseDestructions, derivedDestructions);
    SharedPtr<BaseValue, MultiThreadCounter> base = derived;
    derived.Reset();
    EXPECT_EQ(base.UseCount(), 1u);
    base.Reset();
    EXPECT_EQ(baseDestructions, 1u);
    EXPECT_EQ(derivedDestructions, 1u);

    SharedPtr<BaseValue> direct(new DerivedValue(baseDestructions, derivedDestructions));
    direct.Reset();
    EXPECT_EQ(baseDestructions, 2u);
    EXPECT_EQ(derivedDestructions, 2u);
}

TEST(SharedPtrLifetime, MultiThreadCastsPreserveCounterAndFailedCastsStayEmpty)
{
    SharedPtr<CastDerived, MultiThreadCounter> derived =
        MakeShared<CastDerived, MultiThreadCounter>();
    SharedPtr<CastBase, MultiThreadCounter> base    = static_pointer_cast<CastBase>(derived);
    SharedPtr<CastDerived, MultiThreadCounter> cast = dynamic_pointer_cast<CastDerived>(base);
    EXPECT_EQ(cast, derived);
    EXPECT_EQ(derived.UseCount(), 3u);
    SharedPtr<CastBase, MultiThreadCounter> other     = MakeShared<CastBase, MultiThreadCounter>();
    SharedPtr<CastDerived, MultiThreadCounter> failed = dynamic_pointer_cast<CastDerived>(other);
    EXPECT_FALSE(failed);
    EXPECT_EQ(failed.UseCount(), 0u);
    EXPECT_EQ(other.UseCount(), 1u);
}

TEST(SharedPtrLifetime, AtomicCopiesReleaseTheObjectOnceAcrossThreads)
{
    std::atomic<uint32_t> destructions{0};
    SharedPtr<int, MultiThreadCounter> owner(new int(42), [&destructions](int* pointer) {
        ++destructions;
        delete pointer;
    });
    std::promise<void> start;
    const std::shared_future<void> ready = start.get_future().share();
    HeapVector<std::thread> workers;
    workers.reserve(8);
    for (uint32_t i = 0; i < 8; ++i)
    {
        workers.emplace_back(CopySharedOwner, owner, ready);
    }
    owner.Reset();
    EXPECT_EQ(destructions.load(), 0u);
    start.set_value();
    for (std::thread& worker : workers)
    {
        worker.join();
    }
    EXPECT_EQ(destructions.load(), 1u);
}
