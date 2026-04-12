#include "Memory/Memory.h"
#include <gtest/gtest.h>

using namespace zen;

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



TEST(mem_alloc_test, allocator)
{
    constexpr int numElements = 10;
    auto arraySize            = sizeof(int) * numElements;
    int* pArr = static_cast<int*>(ZEN_MEM_ALLOC(arraySize));

    for (int i = 0; i < numElements; ++i)
    {
        pArr[i] = i + 1;
    }

    EXPECT_NE(pArr, nullptr);
    EXPECT_EQ(pArr[0], 1);

    auto newSize    = arraySize * 2;
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

    auto* pObj = ZEN_NEW() DummyClass(10);
    EXPECT_EQ(pObj->GetData(), 10);
    EXPECT_EQ(reinterpret_cast<size_t>(pObj->GetDataPtr()), reinterpret_cast<size_t>(pObj));
    ZEN_DELETE(pObj);
}
