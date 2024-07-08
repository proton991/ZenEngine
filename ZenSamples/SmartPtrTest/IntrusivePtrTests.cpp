#include <gtest/gtest.h>
#include "Common/Intrusive.h"
#include "Common/Errors.h"

using namespace zen;

class TestA : public IntrusivePtrEnabled<TestA>
{
public:
    virtual ~TestA() = default;

    int a = 5;
};

class TestB : public TestA
{
public:
    ~TestB() override
    {
        LOGI("Destroying TestB");
    }
    int b = 10;
};

TEST(IntrusivePtr, basic)
{
    std::vector<IntrusivePtr<TestA>> as;
    {
        auto b = MakeIntrusive<TestB>();
        IntrusivePtr<TestA> a;
        a = b;
        IntrusivePtr<TestA> c;
        c = a;
        as.push_back(a);
    }
    EXPECT_EQ(as[0]->a, 5);
}