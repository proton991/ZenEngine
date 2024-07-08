#include <gtest/gtest.h>
#include "Common/SmallVector.h"
#include "Common/Errors.h"

using namespace zen;
TEST(small_vector_tests, constructors)
{
    SmallVector<std::string> sv0;
    EXPECT_EQ(sv0.size(), 0);

    SmallVector<std::string> sv1 = {"a", "b"};
    EXPECT_EQ(sv1[0], "a");
    EXPECT_EQ(sv1[1], "b");
    EXPECT_EQ(sv1.size(), 2);

    auto sv1CopyAssign = sv1;
    EXPECT_EQ(sv1CopyAssign[0], "a");
    EXPECT_EQ(sv1CopyAssign[1], "b");
    EXPECT_EQ(sv1CopyAssign.size(), 2);

    auto sv1CopyConstruct{sv1};
    EXPECT_EQ(sv1CopyConstruct[0], "a");
    EXPECT_EQ(sv1CopyConstruct[1], "b");
    EXPECT_EQ(sv1CopyConstruct.size(), 2);

    auto sv1MoveAssign = std::move(sv1);
    EXPECT_EQ(sv1MoveAssign[0], "a");
    EXPECT_EQ(sv1MoveAssign[1], "b");
    EXPECT_EQ(sv1.size(), 0);
    EXPECT_EQ(sv1MoveAssign.size(), 2);

    auto sv1MoveConstruct{std::move(sv1MoveAssign)};
    EXPECT_EQ(sv1MoveConstruct[0], "a");
    EXPECT_EQ(sv1MoveConstruct[1], "b");
    EXPECT_EQ(sv1MoveAssign.size(), 0);
    EXPECT_EQ(sv1MoveConstruct.size(), 2);

    std::vector<std::string> v = {"a", "b", "c"};
    SmallVector<std::string> sv2{v.data(), v.data() + v.size()};
    EXPECT_EQ(sv2.size(), 3);
    EXPECT_EQ(sv2[0], "a");
    EXPECT_EQ(sv2[1], "b");
    EXPECT_EQ(sv2[2], "c");
    for (const auto& item : sv2)
    {
        std::cout << item << std::endl;
    }
}

TEST(small_vector_tests, operations)
{
    SmallVector<int> sv0 = {1, 2, 3};
    EXPECT_EQ(sv0.back(), 3);
    sv0.push_back(4);
    EXPECT_EQ(sv0.back(), 4);
    sv0.pop_back();
    sv0.pop_back();
    EXPECT_EQ(sv0.back(), 2);
    sv0.emplace_back(5);
    EXPECT_EQ(sv0.back(), 5);
    sv0.insert(sv0.end(), 6);
    EXPECT_EQ(sv0.back(), 6);
    sv0.insert(sv0.begin(), 0);
    EXPECT_EQ(sv0[0], 0);
    EXPECT_EQ(sv0[1], 1);

    SmallVector<int> sv1;
    sv1.insert(sv1.begin(), 3);
    sv1.insert(sv1.begin(), 2);
    sv1.insert(sv1.begin(), 1);
    EXPECT_EQ(sv1[0], 1);
    EXPECT_EQ(sv1[1], 2);
    EXPECT_EQ(sv1[2], 3);
    sv1.insert(sv1.begin() + 1, 10);
    EXPECT_EQ(sv1[1], 10);
}