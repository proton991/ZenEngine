// PagedAllocatorTest.cpp
#include "PagedAllocatorTest.h"

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

// Main function for Google Test
int main(int argc, char** pArgv)
{
    ::testing::InitGoogleTest(&argc, pArgv);
    ::testing::GTEST_FLAG(filter) = "PagedAllocator*";
    return RUN_ALL_TESTS();
}
