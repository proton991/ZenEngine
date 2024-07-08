// PagedAllocatorTest.cpp
#include "PagedAllocatorTest.h"

TEST_F(PagedAllocatorTest, AllocateSingleObject)
{
    DummyClass* obj = allocator.Alloc(10);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->getData(), 10);
    allocator.Free(obj);
}

TEST_F(PagedAllocatorTest, AllocateMultipleObjects)
{
    DummyClass* obj1 = allocator.Alloc(10);
    DummyClass* obj2 = allocator.Alloc(20);
    ASSERT_NE(obj1, nullptr);
    ASSERT_NE(obj2, nullptr);
    EXPECT_EQ(obj1->getData(), 10);
    EXPECT_EQ(obj2->getData(), 20);
    allocator.Free(obj1);
    allocator.Free(obj2);
}

TEST_F(PagedAllocatorTest, FreeAndReuseObject)
{
    DummyClass* obj1 = allocator.Alloc(10);
    ASSERT_NE(obj1, nullptr);
    EXPECT_EQ(obj1->getData(), 10);
    allocator.Free(obj1);

    DummyClass* obj2 = allocator.Alloc(20);
    ASSERT_NE(obj2, nullptr);
    EXPECT_EQ(obj2->getData(), 20);
    allocator.Free(obj2);
}

TEST_F(PagedAllocatorTest, AllocateMoreThanPageSize)
{
    const uint32_t numObjects = ZEN_DEFAULT_PAGESIZE + 1;
    std::vector<DummyClass*> objects;

    for (uint32_t i = 0; i < numObjects; ++i)
    {
        DummyClass* obj = allocator.Alloc(i);
        ASSERT_NE(obj, nullptr);
        EXPECT_EQ(obj->getData(), i);
        objects.push_back(obj);
    }

    for (DummyClass* obj : objects)
    {
        allocator.Free(obj);
    }
}

// Main function for Google Test
int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::GTEST_FLAG(filter) = "PagedAllocator*";
    return RUN_ALL_TESTS();
}