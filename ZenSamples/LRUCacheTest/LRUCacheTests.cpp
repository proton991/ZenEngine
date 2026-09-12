#include "Templates/LRUCache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace
{
template <typename Cache> std::vector<typename Cache::key_type> Keys(const Cache& cache)
{
    std::vector<typename Cache::key_type> keys;

    for (const typename Cache::value_type& entry : cache)
    {
        keys.push_back(entry.first);
    }

    return keys;
}

struct ModuloHasher
{
    int modulus;

    size_t operator()(int) const noexcept
    {
        return static_cast<size_t>(modulus);
    }
};

struct ModuloEqual
{
    int modulus;

    bool operator()(int lhs, int rhs) const noexcept
    {
        return lhs % modulus == rhs % modulus;
    }
};

struct ThrowingKey
{
    static inline int copiesBeforeThrow = -1;
    int value;

    explicit ThrowingKey(int value) : value(value) {}

    ThrowingKey(const ThrowingKey& other) : value(other.value)
    {
        if (copiesBeforeThrow == 0)
        {
            throw std::runtime_error("key copy failed");
        }

        if (copiesBeforeThrow > 0)
        {
            --copiesBeforeThrow;
        }
    }

    bool operator==(const ThrowingKey& other) const noexcept
    {
        return value == other.value;
    }
};

struct ThrowingKeyHasher
{
    size_t operator()(const ThrowingKey& key) const noexcept
    {
        return std::hash<int>{}(key.value);
    }
};
} // namespace

TEST(LRUCacheTest, SupportsExistingMapStyleUsage)
{
    zen::LRUCache<size_t, int*> cache;
    EXPECT_TRUE(cache.empty());
    EXPECT_EQ(cache.capacity(), 256u);
    EXPECT_FALSE(cache.contains(1));
    EXPECT_EQ(cache.find(1), cache.end());
    EXPECT_THROW(cache.at(1), std::out_of_range);

    int value = 42;
    cache[1]  = &value;

    ASSERT_TRUE(cache.contains(1));
    EXPECT_EQ(cache[1], &value);

    for (zen::LRUCache<size_t, int*>::value_type& entry : cache)
    {
        EXPECT_EQ(entry.first, 1u);
        EXPECT_EQ(entry.second, &value);
    }
}

TEST(LRUCacheTest, LookupsAndUpdatesControlEvictionOrder)
{
    zen::LRUCache<int, std::string> cache(2);
    cache.try_emplace(1, "one");
    cache.try_emplace(2, "two");

    EXPECT_EQ(Keys(cache), (std::vector<int>{2, 1}));

    const zen::LRUCache<int, std::string>& view = cache;
    EXPECT_TRUE(cache.contains(1));
    EXPECT_EQ(view.find(1)->second, "one");
    EXPECT_EQ(view.at(1), "one");
    EXPECT_THROW(view.at(3), std::out_of_range);
    EXPECT_EQ(Keys(cache), (std::vector<int>{2, 1}));

    EXPECT_NE(cache.find(1), cache.end());

    cache.try_emplace(3, "three");

    EXPECT_FALSE(cache.contains(2));
    EXPECT_EQ(Keys(cache), (std::vector<int>{3, 1}));

    const std::pair<zen::LRUCache<int, std::string>::iterator, bool> updatedResult =
        cache.insert_or_assign(1, "updated");
    zen::LRUCache<int, std::string>::iterator updated = updatedResult.first;
    bool inserted                                     = updatedResult.second;
    EXPECT_FALSE(inserted);
    EXPECT_EQ(updated->second, "updated");

    cache.try_emplace(4, "four");

    EXPECT_EQ(Keys(cache), (std::vector<int>{4, 1}));

    const std::pair<zen::LRUCache<int, std::string>::iterator, bool> existingResult =
        cache.try_emplace(1, "ignored");
    zen::LRUCache<int, std::string>::iterator existing = existingResult.first;
    bool duplicateInserted                             = existingResult.second;
    EXPECT_FALSE(duplicateInserted);
    EXPECT_EQ(existing->second, "updated");
    EXPECT_EQ(Keys(cache), (std::vector<int>{1, 4}));
    EXPECT_EQ(cache.at(4), "four");
    EXPECT_EQ(Keys(cache), (std::vector<int>{4, 1}));
}

TEST(LRUCacheTest, CapacityChangesEvictInOrderAndZeroDisablesInsertion)
{
    std::vector<int> evicted;
    zen::LRUCache<int, int> cache(3, [&](const int& key, int& value) {
        EXPECT_EQ(key, value);
        evicted.push_back(key);
    });
    cache[1] = 1;
    cache[2] = 2;
    cache[3] = 3;
    cache[4] = 4;

    EXPECT_EQ(evicted, (std::vector<int>{1}));

    cache.set_capacity(1);

    EXPECT_EQ(evicted, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(Keys(cache), (std::vector<int>{4}));

    cache.set_capacity(0);

    EXPECT_EQ(evicted, (std::vector<int>{1, 2, 3, 4}));
    EXPECT_TRUE(cache.empty());
    EXPECT_EQ(cache.try_emplace(5, 5), (std::make_pair(cache.end(), false)));
    EXPECT_EQ(cache.insert_or_assign(5, 5), (std::make_pair(cache.end(), false)));
    EXPECT_THROW(cache[5], std::length_error);

    cache.set_capacity(2);
    cache[5] = 5;
    cache[6] = 6;

    EXPECT_EQ(cache.size(), 2u);
    EXPECT_EQ(cache.capacity(), 2u);
}

TEST(LRUCacheTest, EraseClearAndDestructionDoNotInvokeEvictionCallback)
{
    int callbacks = 0;

    {
        zen::LRUCache<int, int> cache(3, [&](const int&, int&) { ++callbacks; });
        cache[1] = 1;
        cache[2] = 2;
        cache[3] = 3;
        EXPECT_EQ(cache.erase(2), 1u);
        EXPECT_EQ(cache.erase(2), 0u);
        EXPECT_EQ(cache.erase(cache.cbegin())->first, 1);
        cache.clear();
        EXPECT_TRUE(cache.empty());
        EXPECT_EQ(cache.capacity(), 3u);
        cache[4] = 4;
    }

    EXPECT_EQ(callbacks, 0);
}

TEST(LRUCacheTest, SupportsMoveOnlyValuesAndReleasesEvictedObjects)
{
    zen::LRUCache<std::string, std::unique_ptr<int>> cache(1);
    std::unique_ptr<int> value = std::make_unique<int>(7);
    cache.try_emplace("first", std::move(value));

    EXPECT_EQ(value, nullptr);

    std::unique_ptr<int> replacement = std::make_unique<int>(8);
    EXPECT_FALSE(cache.try_emplace("first", std::move(replacement)).second);
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(*cache.at("first"), 7);

    cache.insert_or_assign("first", std::move(replacement));

    EXPECT_EQ(*cache.at("first"), 8);

    zen::LRUCache<int, std::shared_ptr<int>> lifetimeCache(1);
    std::shared_ptr<int> owner = std::make_shared<int>(10);
    std::weak_ptr<int> weak    = owner;
    lifetimeCache.try_emplace(1, std::move(owner));
    lifetimeCache.try_emplace(2, std::make_shared<int>(20));

    EXPECT_TRUE(weak.expired());

    zen::LRUCache<int, std::unique_ptr<int>> disabled(0);
    std::unique_ptr<int> retained = std::make_unique<int>(9);
    EXPECT_FALSE(disabled.try_emplace(1, std::move(retained)).second);
    EXPECT_NE(retained, nullptr);
}

TEST(LRUCacheTest, CopyRebuildsIndexAndPreservesRecency)
{
    zen::LRUCache<int, std::string> source(2);
    source[1] = "one";
    source[2] = "two";
    source.at(1);
    zen::LRUCache<int, std::string> copy = source;
    source.clear();

    EXPECT_EQ(Keys(copy), (std::vector<int>{1, 2}));

    copy[3] = "three";

    EXPECT_FALSE(copy.contains(2));
    EXPECT_EQ(copy.at(1), "one");

    source[4] = "four";
    source    = copy;
    copy.clear();

    EXPECT_EQ(source.at(3), "three");
    EXPECT_EQ(source.capacity(), 2u);
    EXPECT_FALSE(source.contains(4));
}

TEST(LRUCacheTest, MoveAndSwapKeepIndexIteratorsValid)
{
    zen::LRUCache<int, std::unique_ptr<int>> source(2);
    source.try_emplace(1, std::make_unique<int>(1));
    source.try_emplace(2, std::make_unique<int>(2));
    zen::LRUCache<int, std::unique_ptr<int>>::iterator entry = source.begin();
    zen::LRUCache<int, std::unique_ptr<int>> moved           = std::move(source);
    EXPECT_TRUE(source.empty());
    EXPECT_EQ(entry, moved.find(2));

    source.try_emplace(3, std::make_unique<int>(3));
    moved = std::move(source);

    EXPECT_TRUE(source.empty());
    EXPECT_EQ(*moved.at(3), 3);

    source.set_capacity(1);
    source.try_emplace(4, std::make_unique<int>(4));
    swap(source, moved);

    EXPECT_EQ(*source.at(3), 3);
    EXPECT_EQ(*moved.at(4), 4);
    EXPECT_EQ(moved.capacity(), 1u);

    moved.try_emplace(5, std::make_unique<int>(5));

    EXPECT_FALSE(moved.contains(4));
}

TEST(LRUCacheTest, HandlesCollisionsAndStatefulEquality)
{
    zen::LRUCache<int, int, ModuloHasher, ModuloEqual> cache(3, {}, {10}, {10});
    cache[1] = 1;
    cache[2] = 2;
    cache[3] = 3;
    EXPECT_EQ(cache.at(11), 1);
    cache[4] = 4;
    EXPECT_FALSE(cache.contains(12));
    EXPECT_EQ(cache.size(), 3u);
    cache.insert_or_assign(21, 21);
    EXPECT_EQ(cache.at(1), 21);
    zen::LRUCache<int, int, ModuloHasher, ModuloEqual> copy = cache;
    EXPECT_EQ(copy.at(31), 21);
    zen::LRUCache<int, int, ModuloHasher, ModuloEqual> moved = std::move(copy);
    EXPECT_EQ(moved.at(41), 21);
}

TEST(LRUCacheTest, KeepsReferencesStableAcrossRehashAndPromotion)
{
    zen::LRUCache<int, std::string> cache(256);
    zen::LRUCache<int, std::string>::iterator entry = cache.try_emplace(0, "zero").first;
    std::string* value                              = &entry->second;

    for (int key = 1; key < 256; ++key)
    {
        cache.try_emplace(key, "value");
    }

    EXPECT_EQ(value, &cache.at(0));
    EXPECT_EQ(entry, cache.begin());

    cache.try_emplace(256, "value");

    EXPECT_FALSE(cache.contains(1));
    EXPECT_EQ(value, &cache.at(0));

    zen::LRUCache<int, std::string> single(1);
    single[1] = "aliased value";
    single.try_emplace(2, single.cbegin()->second);

    EXPECT_EQ(single.at(2), "aliased value");
}

TEST(LRUCacheTest, FailedIndexInsertionPreservesExistingEntries)
{
    zen::LRUCache<ThrowingKey, int, ThrowingKeyHasher> cache(1);
    ThrowingKey first(1);
    ThrowingKey second(2);
    cache.try_emplace(first, 1);
    ThrowingKey::copiesBeforeThrow = 1;
    EXPECT_THROW(cache.try_emplace(second, 2), std::runtime_error);
    ThrowingKey::copiesBeforeThrow = -1;
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(cache.at(first), 1);
    EXPECT_FALSE(cache.contains(second));
    cache.try_emplace(second, 2);
    EXPECT_EQ(cache.at(second), 2);
    EXPECT_FALSE(cache.contains(first));
}

TEST(LRUCacheTest, ThrowingEvictionCallbackLeavesCacheUsable)
{
    bool fail = true;
    zen::LRUCache<int, int> cache(1, [&](const int&, int&) {
        if (fail)
        {
            throw std::runtime_error("eviction failed");
        }
    });
    cache[1] = 1;

    EXPECT_THROW(cache.try_emplace(2, 2), std::runtime_error);
    EXPECT_EQ(Keys(cache), (std::vector<int>{1}));
    EXPECT_THROW(cache.set_capacity(0), std::runtime_error);
    EXPECT_EQ(cache.capacity(), 1u);
    EXPECT_EQ(cache.at(1), 1);

    fail     = false;
    cache[2] = 2;

    EXPECT_FALSE(cache.contains(1));
    EXPECT_EQ(cache.at(2), 2);
}

TEST(LRUCacheTest, MatchesReferenceModelAcrossMixedOperations)
{
    zen::LRUCache<int, int> cache(4);
    std::vector<std::pair<int, int>> reference;
    std::mt19937 random(42);
    size_t capacity = 4;

    for (int step = 0; step < 5000; ++step)
    {
        int key = static_cast<int>(random() % 12);
        std::vector<std::pair<int, int>>::iterator it =
            std::find_if(reference.begin(), reference.end(),
                         [key](const std::pair<int, int>& entry) { return entry.first == key; });

        switch (random() % 5)
        {
            case 0:
                cache.insert_or_assign(key, step);

                if (it != reference.end())
                {
                    reference.erase(it);
                }

                if (capacity > 0)
                {
                    reference.insert(reference.begin(), {key, step});
                }
                break;

            case 1:
                EXPECT_EQ(cache.find(key) != cache.end(), it != reference.end());

                if (it != reference.end())
                {
                    std::pair<int, int> entry = *it;
                    reference.erase(it);
                    reference.insert(reference.begin(), entry);
                }
                break;

            case 2: EXPECT_EQ(cache.contains(key), it != reference.end()); break;
            case 3:
                EXPECT_EQ(cache.erase(key), it != reference.end() ? 1u : 0u);

                if (it != reference.end())
                {
                    reference.erase(it);
                }
                break;

            case 4:
                capacity = random() % 8;
                cache.set_capacity(capacity);
                break;
        }

        if (reference.size() > capacity)
        {
            reference.resize(capacity);
        }

        ASSERT_EQ(cache.size(), reference.size());

        zen::LRUCache<int, int>::const_iterator actual = cache.cbegin();

        for (std::pair<int, int> const& expected : reference)
        {
            EXPECT_EQ(actual->first, expected.first);
            EXPECT_EQ(actual->second, expected.second);
            ++actual;
        }
    }
}
