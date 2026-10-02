#include "Memory/Memory.h"
#include <gtest/gtest.h>

using namespace zen;

class DummyClass
{
public:
    explicit DummyClass(int data) : m_data(data) {}

    int GetData() const
    {
        return m_data;
    }

    const int* GetDataPtr() const
    {
        return &m_data;
    }

private:
    int m_data;
};

class EmptyClass
{};

TEST(mem_alloc_test, allocator)
{
    constexpr int numElements = 10;

    size_t arraySize          = sizeof(int) * numElements;

    int* pArr                 = static_cast<int*>(ZEN_MEM_ALLOC(arraySize));

    for (int i = 0; i < numElements; ++i)
    {
        pArr[i] = i + 1;
    }

    EXPECT_NE(pArr, nullptr);

    EXPECT_EQ(pArr[0], 1);

    size_t newSize   = arraySize * 2;

    int* pResizedArr = static_cast<int*>(ZEN_MEM_REALLOC(pArr, newSize));

    for (int i = 0; i < numElements; ++i)
    {
        EXPECT_EQ(pResizedArr[i], i + 1);
    }

    EXPECT_NE(pResizedArr, nullptr);

    EXPECT_EQ(pResizedArr[0], 1);

    ZEN_MEM_FREE(pResizedArr);
}

TEST(mem_alloc_test, mem_new)
{
    std::cout << "Dummy Class Size: " << sizeof(DummyClass) << std::endl;

    EmptyClass* pEmptyObj = new EmptyClass();

    delete pEmptyObj;

    DummyClass* pObj = ZEN_NEW() DummyClass(10);

    EXPECT_EQ(pObj->GetData(), 10);

    EXPECT_EQ(reinterpret_cast<size_t>(pObj->GetDataPtr()), reinterpret_cast<size_t>(pObj));

    ZEN_DELETE(pObj);
}

TEST(mem_alloc_test, aligned_reallocation_preserves_bytes)
{
    for (size_t alignment : {size_t(8), size_t(16), size_t(64), size_t(256)})
    {
        SCOPED_TRACE(alignment);

        uint8_t* memory = static_cast<uint8_t*>(DefaultAllocator::Alloc(37, alignment, __FILE__, __LINE__));

        ASSERT_NE(memory, nullptr);

        EXPECT_EQ(reinterpret_cast<uintptr_t>(memory) % alignment, 0u);

        for (uint8_t i = 0; i < 37; ++i)
        {
            memory[i] = i;
        }

        memory = static_cast<uint8_t*>(DefaultAllocator::Realloc(memory, 113, alignment, __FILE__, __LINE__));

        ASSERT_NE(memory, nullptr);

        EXPECT_EQ(reinterpret_cast<uintptr_t>(memory) % alignment, 0u);

        for (uint8_t i = 0; i < 37; ++i)
        {
            EXPECT_EQ(memory[i], i);
        }

        memory = static_cast<uint8_t*>(DefaultAllocator::Realloc(memory, 19, alignment, __FILE__, __LINE__));

        ASSERT_NE(memory, nullptr);

        EXPECT_EQ(reinterpret_cast<uintptr_t>(memory) % alignment, 0u);

        for (uint8_t i = 0; i < 19; ++i)
        {
            EXPECT_EQ(memory[i], i);
        }

        DefaultAllocator::Free(memory, __FILE__, __LINE__);
    }
}
