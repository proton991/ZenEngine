#include <gtest/gtest.h>
#include "Graphics/RHI/RHIResource.h"
#include "Common/Errors.h"

using namespace zen;

class DummyResource : public rhi::RHIResource
{};

class DummyResourceChild : public DummyResource
{};

TEST(RefCountPtr, basic)
{
    RefCountPtr<DummyResource> res1 = MakeRefCountPtr<DummyResource>();

    EXPECT_EQ(res1.GetRefCount(), 1);

    auto res2 = res1;
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

    auto res2     = res1;
    auto movedRes = std::move(res2);

    EXPECT_EQ(movedRes.GetRefCount(), 2);
    EXPECT_EQ(res2.GetRefCount(), 0);
    EXPECT_EQ(res2.Get(), nullptr);
}

void addToVec(std::vector<RefCountPtr<DummyResource>>& vec, RefCountPtr<DummyResource> item)
{
    vec.push_back(item);
}

TEST(RefCountPtr, vector)
{
    RefCountPtr<DummyResource> res1 = MakeRefCountPtr<DummyResource>();
    RefCountPtr<DummyResource> res2 = res1;
    {
        std::vector<RefCountPtr<DummyResource>> vec;
        vec.push_back(res1);
        vec.push_back(res2);
        EXPECT_EQ(res1.GetRefCount(), 4);
    }
    {
        std::vector<RefCountPtr<DummyResource>> vec;
        RefCountPtr<DummyResource> res3 = MakeRefCountPtr<DummyResource>();
        addToVec(vec, res3);
        EXPECT_EQ(res3.GetRefCount(), 2);
    }
}