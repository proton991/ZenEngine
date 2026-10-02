#include <array>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>
#include "Templates/HeapVector.h"
#include "Templates/SmallVector.h"
#include "Templates/VectorView.h"
#include "Utils/Errors.h"

using zen::SmallVector;
using zen::VectorView;

namespace
{
struct alignas(128) AlignedElement
{
    uint32_t value{0};
};
} // namespace

TEST(small_vector_tests, aligned_heap_growth_and_move)
{
    SmallVector<AlignedElement, 2> values;

    for (uint32_t i = 0; i < 19; ++i)
    {
        values.push_back(AlignedElement{i});

        EXPECT_EQ(reinterpret_cast<uintptr_t>(values.data()) % alignof(AlignedElement), 0u);
    }

    AlignedElement* heapStorage = values.data();

    SmallVector<AlignedElement, 2> moved(std::move(values));

    EXPECT_TRUE(values.empty());

    EXPECT_EQ(moved.data(), heapStorage);

    SmallVector<AlignedElement, 2> assigned;

    assigned.resize(8);

    assigned = std::move(moved);

    EXPECT_TRUE(moved.empty());

    EXPECT_EQ(assigned.data(), heapStorage);

    for (uint32_t i = 0; i < assigned.size(); ++i)
    {
        EXPECT_EQ(assigned[i].value, i);
    }
}

TEST(small_vector_tests, constructors)
{
    SmallVector<std::string> sv0;

    EXPECT_EQ(sv0.size(), 0);

    SmallVector<std::string> sv1 = {"a", "b"};

    EXPECT_EQ(sv1[0], "a");

    EXPECT_EQ(sv1[1], "b");

    EXPECT_EQ(sv1.size(), 2);

    SmallVector<std::string> sv1CopyAssign = sv1;

    EXPECT_EQ(sv1CopyAssign[0], "a");

    EXPECT_EQ(sv1CopyAssign[1], "b");

    EXPECT_EQ(sv1CopyAssign.size(), 2);

    SmallVector<std::string> sv1CopyConstruct{sv1};

    EXPECT_EQ(sv1CopyConstruct[0], "a");

    EXPECT_EQ(sv1CopyConstruct[1], "b");

    EXPECT_EQ(sv1CopyConstruct.size(), 2);

    zen::SmallVector<std::string> sv1MoveAssign = std::move(sv1);

    EXPECT_EQ(sv1MoveAssign[0], "a");

    EXPECT_EQ(sv1MoveAssign[1], "b");

    EXPECT_EQ(sv1.size(), 0);

    EXPECT_EQ(sv1MoveAssign.size(), 2);

    zen::SmallVector<std::string> sv1MoveConstruct{std::move(sv1MoveAssign)};

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

    for (std::string const& item : sv2)
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

TEST(vector_view_tests, contiguous_containers)
{
    SmallVector<int> smallVector    = {1, 2, 3};

    VectorView<int> smallVectorView = smallVector;

    EXPECT_EQ(smallVectorView.data(), smallVector.data());

    EXPECT_EQ(smallVectorView.size(), smallVector.size());

    const SmallVector<int>& constSmallVector   = smallVector;

    VectorView<const int> constSmallVectorView = constSmallVector;

    EXPECT_EQ(constSmallVectorView.data(), constSmallVector.data());

    std::vector<int> standardVector    = {4, 5, 6};

    VectorView<int> standardVectorView = standardVector;

    EXPECT_EQ(standardVectorView.data(), standardVector.data());

    std::array<int, 2> array       = {7, 8};

    zen::VectorView<int> arrayView = zen::MakeVecView(array);

    EXPECT_EQ(arrayView.data(), array.data());

    EXPECT_EQ(arrayView.size(), array.size());

    zen::HeapVector<int> heapVector     = {9, 10};

    zen::VectorView<int> heapVectorView = zen::MakeVecView(heapVector);

    EXPECT_EQ(heapVectorView.data(), heapVector.data());

    EXPECT_EQ(heapVectorView.size(), heapVector.size());
}

static_assert(!std::is_constructible_v<VectorView<int>, std::vector<int>&&>);
static_assert(!std::is_base_of_v<VectorView<int>, SmallVector<int>>);
