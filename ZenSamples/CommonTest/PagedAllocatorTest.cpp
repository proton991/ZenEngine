// PagedAllocatorTest.cpp
#include "PagedAllocatorTest.h"
#include <algorithm>

TEST_F(PagedAllocatorTest, AllocateSingleObject)
{
    DummyClass* pObj = allocator.Alloc(10);
    ASSERT_NE(pObj, nullptr);
    EXPECT_EQ(pObj->getData(), 10);
    allocator.Free(pObj);
}

TEST_F(PagedAllocatorTest, AllocateMultipleObjects)
{
    DummyClass* pObj1 = allocator.Alloc(10);
    DummyClass* pObj2 = allocator.Alloc(20);
    ASSERT_NE(pObj1, nullptr);
    ASSERT_NE(pObj2, nullptr);
    EXPECT_EQ(pObj1->getData(), 10);
    EXPECT_EQ(pObj2->getData(), 20);
    allocator.Free(pObj1);
    allocator.Free(pObj2);
}

TEST_F(PagedAllocatorTest, FreeAndReuseObject)
{
    DummyClass* pObj1 = allocator.Alloc(10);
    ASSERT_NE(pObj1, nullptr);
    EXPECT_EQ(pObj1->getData(), 10);
    allocator.Free(pObj1);

    DummyClass* pObj2 = allocator.Alloc(20);
    ASSERT_NE(pObj2, nullptr);
    EXPECT_EQ(pObj2->getData(), 20);
    allocator.Free(pObj2);
}

TEST_F(PagedAllocatorTest, AllocateMoreThanPageSize)
{
    const uint32_t numObjects = ZEN_DEFAULT_PAGESIZE + 1;
    std::vector<DummyClass*> objects;

    for (uint32_t i = 0; i < numObjects; ++i)
    {
        DummyClass* pObj = allocator.Alloc(i);
        ASSERT_NE(pObj, nullptr);
        EXPECT_EQ(pObj->getData(), i);
        objects.push_back(pObj);
    }

    for (DummyClass* obj : objects)
    {
        allocator.Free(obj);
    }
}

TEST_F(PagedAllocatorTest, PageGrowthKeepsEveryLiveAddressAndValue)
{
    zen::PagedAllocator<DummyClass> pages(4, false);
    pages.Init();
    std::vector<DummyClass*> live;
    for (int i = 0; i < 17; ++i)
    {
        auto* object = pages.Alloc(i);
        EXPECT_EQ(std::count(live.begin(), live.end(), object), 0);
        live.push_back(object);
        for (int j = 0; j <= i; ++j)
        {
            EXPECT_EQ(live[j]->getData(), j);
        }
    }
    // Mix free-stack segments, reuse holes, then grow again with survivors still live.
    for (size_t i = 0; i < live.size(); i += 2)
    {
        pages.Free(live[i]);
        live[i] = nullptr;
    }
    std::vector<DummyClass*> replacements;
    for (int i = 0; i < 20; ++i)
    {
        auto* object = pages.Alloc(100 + i);
        EXPECT_EQ(std::count(live.begin(), live.end(), object), 0);
        EXPECT_EQ(std::count(replacements.begin(), replacements.end(), object), 0);
        replacements.push_back(object);
    }
    for (size_t i = 1; i < live.size(); i += 2)
    {
        EXPECT_EQ(live[i]->getData(), i);
        pages.Free(live[i]);
    }
    for (size_t i = 0; i < replacements.size(); ++i)
    {
        EXPECT_EQ(replacements[i]->getData(), 100 + i);
        pages.Free(replacements[i]);
    }
}

TEST_F(PagedAllocatorTest, ThreadSafePageGrowthPreservesLiveObjects)
{
    zen::PagedAllocator<DummyClass> pages(4, true);
    pages.Init();
    std::vector<DummyClass*> live;
    for (int i = 0; i < 33; ++i)
    {
        auto* object = pages.Alloc(i);
        EXPECT_EQ(std::count(live.begin(), live.end(), object), 0);
        live.push_back(object);
    }
    std::reverse(live.begin(), live.end());
    for (size_t i = 0; i < live.size(); ++i)
    {
        EXPECT_EQ(live[i]->getData(), 32 - i);
        pages.Free(live[i]);
    }
}

// Main function for Google Test
int main(int argc, char** pArgv)
{
    ::testing::InitGoogleTest(&argc, pArgv);
    return RUN_ALL_TESTS();
}
