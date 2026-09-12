#include "Templates/FlatHashMap.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace
{
struct ConstantHasher
{
    size_t operator()(int) const
    {
        return 7;
    }
};

struct LifetimeValue
{
    static inline int alive = 0;

    int value{0};

    explicit LifetimeValue(int initialValue = 0) : value(initialValue)
    {
        ++alive;
    }

    LifetimeValue(const LifetimeValue& other) : value(other.value)
    {
        ++alive;
    }

    LifetimeValue(LifetimeValue&& other) noexcept : value(other.value)
    {
        other.value = -1;
        ++alive;
    }

    LifetimeValue& operator=(const LifetimeValue&) = default;

    LifetimeValue& operator=(LifetimeValue&&) = default;

    ~LifetimeValue()
    {
        --alive;
    }
};

struct alignas(64) OverAlignedValue
{
    uint64_t words[8]{};
};
} // namespace

TEST(FlatHashMapTest, BasicOperations)
{
    zen::FlatHashMap<int, std::string> map{{1, "one"}, {2, "two"}};

    EXPECT_EQ(map.size(), 2u);
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.contains(1));
    EXPECT_EQ(map.count(2), 1u);
    EXPECT_EQ(map.count(3), 0u);

    const std::pair<zen::FlatHashMap<int, std::string>::iterator, bool> duplicateIterResult =
        map.try_emplace(1, "changed");
    zen::FlatHashMap<int, std::string>::iterator duplicateIter = duplicateIterResult.first;
    bool duplicateInserted                                     = duplicateIterResult.second;
    EXPECT_FALSE(duplicateInserted);
    EXPECT_EQ(duplicateIter->second, "one");

    const std::pair<zen::FlatHashMap<int, std::string>::iterator, bool> assignIterResult =
        map.insert_or_assign(1, "updated");
    zen::FlatHashMap<int, std::string>::iterator assignIter = assignIterResult.first;
    bool assignInserted                                     = assignIterResult.second;
    EXPECT_FALSE(assignInserted);
    EXPECT_EQ(assignIter->second, "updated");

    map[3] = "three";

    EXPECT_EQ(map.at(3), "three");
    EXPECT_LE(map.load_factor(), map.max_load_factor());

    map.clear();

    EXPECT_TRUE(map.empty());
    EXPECT_GT(map.capacity(), 0u);

    map.reset();

    EXPECT_EQ(map.capacity(), 0u);
}

TEST(FlatHashMapTest, ResolvesHashCollisionsAndReusesDeletedSlots)
{
    constexpr int initialCount = 1024;
    constexpr int finalCount   = 2048;
    zen::FlatHashMap<int, std::string, ConstantHasher> map;

    for (int key = 0; key < initialCount; ++key)
    {
        const std::pair<zen::FlatHashMap<int, std::string, ConstantHasher>::iterator, bool>
            iterResult = map.try_emplace(key, "value-" + std::to_string(key));
        zen::FlatHashMap<int, std::string, ConstantHasher>::iterator iter = iterResult.first;
        bool inserted                                                     = iterResult.second;
        ASSERT_TRUE(inserted);
        EXPECT_EQ(iter->first, key);
    }

    for (int key = 0; key < initialCount; ++key)
    {
        ASSERT_TRUE(map.contains(key));
        EXPECT_EQ(map.at(key), "value-" + std::to_string(key));
    }

    for (int key = 0; key < initialCount; key += 2)
    {
        EXPECT_EQ(map.erase(key), 1u);
        EXPECT_EQ(map.erase(key), 0u);
    }

    for (int key = initialCount; key < finalCount; ++key)
    {
        map[key] = "new-value";
    }

    for (int key = 0; key < initialCount; ++key)
    {
        EXPECT_EQ(map.contains(key), (key & 1) != 0);
    }

    for (int key = initialCount; key < finalCount; ++key)
    {
        EXPECT_EQ(map.at(key), "new-value");
    }
}

TEST(FlatHashMapTest, SupportsCopyMoveAndIteratorErase)
{
    zen::FlatHashMap<int, std::string, ConstantHasher> source;

    for (int key = 0; key < 256; ++key)
    {
        source.try_emplace(key, std::to_string(key));
    }

    zen::FlatHashMap<int, std::string, ConstantHasher> copy = source;
    ASSERT_EQ(copy.size(), source.size());

    for (const zen::FlatHashMap<int, std::string, ConstantHasher>::value_type& entry : source)
    {
        EXPECT_EQ(copy.at(entry.first), entry.second);
    }

    zen::FlatHashMap<int, std::string, ConstantHasher> moved = std::move(copy);
    EXPECT_TRUE(copy.empty());
    EXPECT_EQ(moved.size(), source.size());

    for (zen::FlatHashMap<int, std::string, ConstantHasher>::iterator iter = moved.begin();
         iter != moved.end();)
    {
        iter = moved.erase(iter);
    }

    EXPECT_TRUE(moved.empty());
}

TEST(FlatHashMapTest, ManagesValueLifetimesAcrossRehashAndClear)
{
    ASSERT_EQ(LifetimeValue::alive, 0);

    {
        zen::FlatHashMap<int, LifetimeValue> map;

        for (int key = 0; key < 512; ++key)
        {
            map.try_emplace(key, key);
        }

        EXPECT_EQ(LifetimeValue::alive, 512);

        map.rehash(2048);
        EXPECT_EQ(LifetimeValue::alive, 512);

        map.clear();
        EXPECT_EQ(LifetimeValue::alive, 0);
    }

    EXPECT_EQ(LifetimeValue::alive, 0);
}

TEST(FlatHashMapTest, SupportsMoveOnlyAndOverAlignedValues)
{
    zen::FlatHashMap<int, std::unique_ptr<int>> moveOnlyMap;

    for (int key = 0; key < 256; ++key)
    {
        moveOnlyMap.try_emplace(key, std::make_unique<int>(key));
    }

    for (int key = 0; key < 256; ++key)
    {
        ASSERT_NE(moveOnlyMap.at(key), nullptr);
        EXPECT_EQ(*moveOnlyMap.at(key), key);
    }

    zen::FlatHashMap<int, OverAlignedValue> alignedMap;
    alignedMap.try_emplace(1);
    const uintptr_t address = reinterpret_cast<uintptr_t>(&alignedMap.at(1));
    EXPECT_EQ(address % alignof(OverAlignedValue), 0u);
}

TEST(FlatHashMapTest, MatchesReferenceStateUnderRandomOperations)
{
    constexpr int keyCount       = 4096;
    constexpr int operationCount = 200000;
    bool present[keyCount]{};
    int expectedValues[keyCount]{};
    uint32_t randomState = 0x12345678u;

    zen::FlatHashMap<int, int> map;

    for (int operation = 0; operation < operationCount; ++operation)
    {
        randomState ^= randomState << 13;
        randomState ^= randomState >> 17;
        randomState ^= randomState << 5;

        const int key         = static_cast<int>(randomState % keyCount);
        const uint32_t action = (randomState >> 16) % 4;

        if (action == 0)
        {
            const int value = static_cast<int>(randomState);
            map.insert_or_assign(key, value);
            present[key]        = true;
            expectedValues[key] = value;
        }
        else if (action == 1)
        {
            EXPECT_EQ(map.erase(key), present[key] ? 1u : 0u);
            present[key] = false;
        }
        else
        {
            EXPECT_EQ(map.contains(key), present[key]);

            if (present[key])
            {
                EXPECT_EQ(map.at(key), expectedValues[key]);
            }
        }

        if ((operation % 997) == 0)
        {
            map.rehash(map.capacity());
        }
    }

    size_t expectedSize = 0;

    for (int key = 0; key < keyCount; ++key)
    {
        EXPECT_EQ(map.contains(key), present[key]);

        if (present[key])
        {
            ++expectedSize;
            EXPECT_EQ(map.at(key), expectedValues[key]);
        }
    }

    EXPECT_EQ(map.size(), expectedSize);
}
