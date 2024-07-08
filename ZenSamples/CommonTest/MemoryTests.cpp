#include "Common/Memory.h"
#include <gtest/gtest.h>

class DummyClass
{
public:
    explicit DummyClass(int data) : m_data(data) {}
    auto GetData() const
    {
        return m_data;
    }
    auto* GetDataPtr() const
    {
        return &m_data;
    }

private:
    int m_data;
};

class EmptyClass
{};

using namespace zen;

TEST(mem_alloc_test, allocator)
{
    constexpr int numElements = 10;
    auto arraySize            = sizeof(int) * numElements;
    int* arr                  = static_cast<int*>(DefaultAllocator::Alloc(arraySize));

    for (int i = 0; i < numElements; ++i)
    {
        arr[i] = i + 1;
    }

    EXPECT_NE(arr, nullptr);
    EXPECT_EQ(arr[0], 1);

    auto newSize    = arraySize * 2;
    int* resizedArr = static_cast<int*>(DefaultAllocator::Realloc(arr, newSize));
    for (int i = 0; i < numElements; ++i)
    {
        EXPECT_EQ(resizedArr[i], i + 1);
    }
    EXPECT_NE(resizedArr, nullptr);
    EXPECT_EQ(resizedArr[0], 1);

    DefaultAllocator::Free(resizedArr);
}

TEST(mem_alloc_test, mem_new)
{
    std::cout << "Dummy Class Size: " << sizeof(DummyClass) << std::endl;
    EmptyClass* emptyObj = new EmptyClass();
    delete emptyObj;

    auto* obj = MemNew<DummyClass>(10);
    EXPECT_EQ(obj->GetData(), 10);
    EXPECT_EQ((size_t)obj->GetDataPtr(), (size_t)obj);
    delete obj;
}