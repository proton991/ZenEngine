#include <gtest/gtest.h>
#include "Common/SmallVector.h"
#include "Common/Errors.h"

using namespace zen;
TEST(small_vector_tests, constructors)
{
    SmallVector<std::string> sv0;
    EXPECT_EQ(sv0.Size(), 0);

    SmallVector<std::string> sv1 = {"a", "b"};
    EXPECT_EQ(sv1[0], "a");
    EXPECT_EQ(sv1[1], "b");
    EXPECT_EQ(sv1.Size(), 2);

    auto sv1CopyAssign = sv1;
    EXPECT_EQ(sv1CopyAssign[0], "a");
    EXPECT_EQ(sv1CopyAssign[1], "b");
    EXPECT_EQ(sv1CopyAssign.Size(), 2);

    auto sv1CopyConstruct{sv1};
    EXPECT_EQ(sv1CopyConstruct[0], "a");
    EXPECT_EQ(sv1CopyConstruct[1], "b");
    EXPECT_EQ(sv1CopyConstruct.Size(), 2);

    auto sv1MoveAssign = std::move(sv1);
    EXPECT_EQ(sv1MoveAssign[0], "a");
    EXPECT_EQ(sv1MoveAssign[1], "b");
    EXPECT_EQ(sv1.Size(), 0);
    EXPECT_EQ(sv1MoveAssign.Size(), 2);

    auto sv1MoveConstruct{std::move(sv1MoveAssign)};
    EXPECT_EQ(sv1MoveConstruct[0], "a");
    EXPECT_EQ(sv1MoveConstruct[1], "b");
    EXPECT_EQ(sv1MoveAssign.Size(), 0);
    EXPECT_EQ(sv1MoveConstruct.Size(), 2);

    std::vector<std::string> v = {"a", "b", "c"};
    SmallVector<std::string> sv2{v.data(), v.data() + v.size()};
    EXPECT_EQ(sv2.Size(), 3);
    EXPECT_EQ(sv2[0], "a");
    EXPECT_EQ(sv2[1], "b");
    EXPECT_EQ(sv2[2], "c");
}

TEST(small_vector_tests, operations)
{
    SmallVector<int> sv0 = {1, 2, 3};
    EXPECT_EQ(sv0.Back(), 3);
    sv0.PushBack(4);
    EXPECT_EQ(sv0.Back(), 4);
    sv0.PopBack();
    sv0.PopBack();
    EXPECT_EQ(sv0.Back(), 2);
    sv0.EmplaceBack(5);
    EXPECT_EQ(sv0.Back(), 5);
    sv0.Insert(sv0.End(), 6);
    EXPECT_EQ(sv0.Back(), 6);
    sv0.Insert(sv0.Begin(), 0);
    EXPECT_EQ(sv0[0], 0);
    EXPECT_EQ(sv0[1], 1);

    SmallVector<int> sv1;
    sv1.Insert(sv1.Begin(), 3);
    sv1.Insert(sv1.Begin(), 2);
    sv1.Insert(sv1.Begin(), 1);
    EXPECT_EQ(sv1[0], 1);
    EXPECT_EQ(sv1[1], 2);
    EXPECT_EQ(sv1[2], 3);
    sv1.Insert(sv1.Begin() + 1, 10);
    EXPECT_EQ(sv1[1], 10);
}