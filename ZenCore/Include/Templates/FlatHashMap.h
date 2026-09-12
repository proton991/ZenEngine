#pragma once

#include "Memory/Memory.h"
#include "Utils/Errors.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace zen
{
/// A cache-friendly hash map that stores its slots in one contiguous allocation.
///
/// Collision resolution uses linear probing. Erased slots become tombstones and are
/// compacted on a later rehash. References and iterators are invalidated whenever the
/// map rehashes; erasing an element invalidates only iterators to that element.
template <typename Key,
          typename Value,
          typename Hasher   = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
class FlatHashMap
{
public:
    static_assert(std::is_copy_constructible_v<Key>,
                  "FlatHashMap keys must be copy constructible for rehashing");

    using key_type        = Key;
    using mapped_type     = Value;
    using value_type      = std::pair<const Key, Value>;
    using size_type       = size_t;
    using difference_type = ptrdiff_t;
    using hasher          = Hasher;
    using key_equal       = KeyEqual;

private:
    enum class SlotState : uint8_t
    {
        eEmpty,
        eOccupied,
        eDeleted
    };

    struct Slot
    {
        size_t hash{0};
        SlotState state{SlotState::eEmpty};
        alignas(value_type) std::byte storage[sizeof(value_type)];

        value_type* GetStorage()
        {
            return reinterpret_cast<value_type*>(storage);
        }

        value_type* GetValue()
        {
            return std::launder(GetStorage());
        }

        const value_type* GetValue() const
        {
            return std::launder(reinterpret_cast<const value_type*>(storage));
        }
    };

    template <bool IsConst> class IteratorBase
    {
    private:
        using MapType = std::conditional_t<IsConst, const FlatHashMap, FlatHashMap>;

        friend class FlatHashMap;
        template <bool> friend class IteratorBase;

        IteratorBase(MapType* pMap, size_type index) : m_pMap(pMap), m_index(index)
        {
            SkipToOccupiedSlot();
        }

        void SkipToOccupiedSlot()
        {
            if (m_pMap != nullptr)
            {
                while (m_index < m_pMap->m_capacity &&
                       m_pMap->m_pSlots[m_index].state != SlotState::eOccupied)
                {
                    ++m_index;
                }
            }
        }

        MapType* m_pMap{nullptr};
        size_type m_index{0};

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = FlatHashMap::value_type;
        using difference_type   = FlatHashMap::difference_type;
        using pointer           = std::conditional_t<IsConst, const value_type*, value_type*>;
        using reference         = std::conditional_t<IsConst, const value_type&, value_type&>;

        IteratorBase() = default;

        IteratorBase(const IteratorBase&) = default;

        IteratorBase& operator=(const IteratorBase&) = default;

        template <bool OtherIsConst> IteratorBase(const IteratorBase<OtherIsConst>& other)
            requires(IsConst && !OtherIsConst)
            : m_pMap(other.m_pMap), m_index(other.m_index)
        {}

        reference operator*() const
        {
            ASSERT(m_pMap != nullptr && m_index < m_pMap->m_capacity);
            return *m_pMap->m_pSlots[m_index].GetValue();
        }

        pointer operator->() const
        {
            return &operator*();
        }

        IteratorBase& operator++()
        {
            ASSERT(m_pMap != nullptr && m_index < m_pMap->m_capacity);
            ++m_index;
            SkipToOccupiedSlot();

            return *this;
        }

        IteratorBase operator++(int)
        {
            IteratorBase previous = *this;
            ++(*this);

            return previous;
        }

        template <bool OtherIsConst> bool operator==(const IteratorBase<OtherIsConst>& other) const
        {
            return m_pMap == other.m_pMap && m_index == other.m_index;
        }

        template <bool OtherIsConst> bool operator!=(const IteratorBase<OtherIsConst>& other) const
        {
            return !(*this == other);
        }
    };

public:
    using iterator       = IteratorBase<false>;
    using const_iterator = IteratorBase<true>;

    FlatHashMap() = default;

    explicit FlatHashMap(size_type reserveCount,
                         const Hasher& hash    = Hasher(),
                         const KeyEqual& equal = KeyEqual()) :
        m_hasher(hash), m_keyEqual(equal)
    {
        reserve(reserveCount);
    }

    FlatHashMap(std::initializer_list<value_type> values,
                const Hasher& hash    = Hasher(),
                const KeyEqual& equal = KeyEqual()) :
        m_hasher(hash), m_keyEqual(equal)
    {
        reserve(values.size());
        insert(values);
    }

    FlatHashMap(const FlatHashMap& other) : m_hasher(other.m_hasher), m_keyEqual(other.m_keyEqual)
    {
        reserve(other.m_size);

        for (const value_type& value : other)
        {
            try_emplace(value.first, value.second);
        }
    }

    FlatHashMap(FlatHashMap&& other) noexcept :
        m_pAllocation(other.m_pAllocation),
        m_pSlots(other.m_pSlots),
        m_size(other.m_size),
        m_deletedCount(other.m_deletedCount),
        m_capacity(other.m_capacity),
        m_hasher(std::move(other.m_hasher)),
        m_keyEqual(std::move(other.m_keyEqual))
    {
        other.m_pAllocation  = nullptr;
        other.m_pSlots       = nullptr;
        other.m_size         = 0;
        other.m_deletedCount = 0;
        other.m_capacity     = 0;
    }

    ~FlatHashMap()
    {
        ReleaseStorage();
    }

    FlatHashMap& operator=(const FlatHashMap& other)
    {
        if (this != &other)
        {
            FlatHashMap copy(other);
            Swap(copy);
        }

        return *this;
    }

    FlatHashMap& operator=(FlatHashMap&& other) noexcept
    {
        if (this != &other)
        {
            ReleaseStorage();

            m_pAllocation  = other.m_pAllocation;
            m_pSlots       = other.m_pSlots;
            m_size         = other.m_size;
            m_deletedCount = other.m_deletedCount;
            m_capacity     = other.m_capacity;
            m_hasher       = std::move(other.m_hasher);
            m_keyEqual     = std::move(other.m_keyEqual);

            other.m_pAllocation  = nullptr;
            other.m_pSlots       = nullptr;
            other.m_size         = 0;
            other.m_deletedCount = 0;
            other.m_capacity     = 0;
        }

        return *this;
    }

    iterator begin()
    {
        return iterator(this, 0);
    }

    const_iterator begin() const
    {
        return const_iterator(this, 0);
    }

    const_iterator cbegin() const
    {
        return const_iterator(this, 0);
    }

    iterator end()
    {
        return iterator(this, m_capacity);
    }

    const_iterator end() const
    {
        return const_iterator(this, m_capacity);
    }

    const_iterator cend() const
    {
        return const_iterator(this, m_capacity);
    }

    bool empty() const
    {
        return m_size == 0;
    }

    size_type size() const
    {
        return m_size;
    }

    size_type capacity() const
    {
        return m_capacity;
    }

    float load_factor() const
    {
        return m_capacity == 0 ? 0.0f : static_cast<float>(m_size) / static_cast<float>(m_capacity);
    }

    static constexpr float max_load_factor()
    {
        return static_cast<float>(cMaxLoadNumerator) / static_cast<float>(cMaxLoadDenominator);
    }

    Hasher hash_function() const
    {
        return m_hasher;
    }

    KeyEqual key_eq() const
    {
        return m_keyEqual;
    }

    iterator find(const Key& key)
    {
        const size_type index = FindIndex(key, MixedHash(key));
        return index == cInvalidIndex ? end() : iterator(this, index);
    }

    const_iterator find(const Key& key) const
    {
        const size_type index = FindIndex(key, MixedHash(key));
        return index == cInvalidIndex ? end() : const_iterator(this, index);
    }

    bool contains(const Key& key) const
    {
        return FindIndex(key, MixedHash(key)) != cInvalidIndex;
    }

    size_type count(const Key& key) const
    {
        return contains(key) ? 1 : 0;
    }

    Value& at(const Key& key)
    {
        iterator iter = find(key);
        ASSERT(iter != end());

        return iter->second;
    }

    const Value& at(const Key& key) const
    {
        const_iterator iter = find(key);
        ASSERT(iter != end());

        return iter->second;
    }

    Value& operator[](const Key& key)
    {
        return try_emplace(key).first->second;
    }

    Value& operator[](Key&& key)
    {
        return try_emplace(std::move(key)).first->second;
    }

    std::pair<iterator, bool> insert(const value_type& value)
    {
        return try_emplace(value.first, value.second);
    }

    std::pair<iterator, bool> insert(value_type&& value)
    {
        return try_emplace(value.first, std::move(value.second));
    }

    void insert(std::initializer_list<value_type> values)
    {
        reserve(m_size + values.size());

        for (const value_type& value : values)
        {
            insert(value);
        }
    }

    template <typename... Args>
    std::pair<iterator, bool> try_emplace(const Key& key, Args&&... args)
    {
        return TryEmplaceInternal(key, std::forward<Args>(args)...);
    }

    template <typename... Args> std::pair<iterator, bool> try_emplace(Key&& key, Args&&... args)
    {
        return TryEmplaceInternal(std::move(key), std::forward<Args>(args)...);
    }

    template <typename... Args> std::pair<iterator, bool> emplace(const Key& key, Args&&... args)
    {
        return try_emplace(key, std::forward<Args>(args)...);
    }

    template <typename... Args> std::pair<iterator, bool> emplace(Key&& key, Args&&... args)
    {
        return try_emplace(std::move(key), std::forward<Args>(args)...);
    }

    template <typename M> std::pair<iterator, bool> insert_or_assign(const Key& key, M&& value)
    {
        std::pair<iterator, bool> result{};

        iterator iter = find(key);

        if (iter != end())
        {
            iter->second = std::forward<M>(value);
            result       = {iter, false};
        }
        else
        {
            result = try_emplace(key, std::forward<M>(value));
        }

        return result;
    }

    template <typename M> std::pair<iterator, bool> insert_or_assign(Key&& key, M&& value)
    {
        std::pair<iterator, bool> result{};

        iterator iter = find(key);

        if (iter != end())
        {
            iter->second = std::forward<M>(value);
            result       = {iter, false};
        }
        else
        {
            result = try_emplace(std::move(key), std::forward<M>(value));
        }

        return result;
    }

    size_type erase(const Key& key)
    {
        size_type result{};

        const size_type index = FindIndex(key, MixedHash(key));

        if (!(index == cInvalidIndex))
        {
            EraseSlot(index);
            result = 1;
        }

        return result;
    }

    iterator erase(iterator position)
    {
        ASSERT(position.m_pMap == this && position.m_index < m_capacity);
        const size_type nextIndex = position.m_index + 1;
        EraseSlot(position.m_index);

        return iterator(this, nextIndex);
    }

    iterator erase(const_iterator position)
    {
        ASSERT(position.m_pMap == this && position.m_index < m_capacity);
        const size_type nextIndex = position.m_index + 1;
        EraseSlot(position.m_index);

        return iterator(this, nextIndex);
    }

    void clear()
    {
        for (size_type i = 0; i < m_capacity; ++i)
        {
            Slot& slot = m_pSlots[i];

            if (slot.state == SlotState::eOccupied)
            {
                std::destroy_at(slot.GetValue());
            }

            slot.hash  = 0;
            slot.state = SlotState::eEmpty;
        }

        m_size         = 0;
        m_deletedCount = 0;
    }

    void reset()
    {
        ReleaseStorage();
    }

    void reserve(size_type elementCount)
    {
        const size_type requiredCapacity = CapacityForElements(elementCount);

        if (requiredCapacity > m_capacity)
        {
            RehashInternal(requiredCapacity);
        }
    }

    void rehash(size_type bucketCount)
    {
        const size_type minimumCapacity   = CapacityForElements(m_size);
        const size_type requestedCapacity = bucketCount == 0 ? 0 : NextPowerOfTwo(bucketCount);
        const size_type targetCapacity =
            requestedCapacity > minimumCapacity ? requestedCapacity : minimumCapacity;

        if (targetCapacity == 0)
        {
            ReleaseStorage();
        }
        else if (targetCapacity != m_capacity || m_deletedCount != 0)
        {
            RehashInternal(targetCapacity);
        }
    }

    void Swap(FlatHashMap& other)
    {
        using std::swap;
        swap(m_pAllocation, other.m_pAllocation);
        swap(m_pSlots, other.m_pSlots);
        swap(m_size, other.m_size);
        swap(m_deletedCount, other.m_deletedCount);
        swap(m_capacity, other.m_capacity);
        swap(m_hasher, other.m_hasher);
        swap(m_keyEqual, other.m_keyEqual);
    }

private:
    static constexpr size_type cMinimumCapacity    = 8;
    static constexpr size_type cMaxLoadNumerator   = 4;
    static constexpr size_type cMaxLoadDenominator = 5;
    static constexpr size_type cInvalidIndex       = std::numeric_limits<size_type>::max();

    struct ProbeResult
    {
        size_type index{cInvalidIndex};
        bool found{false};
    };

    void* m_pAllocation{nullptr};
    Slot* m_pSlots{nullptr};
    size_type m_size{0};
    size_type m_deletedCount{0};
    size_type m_capacity{0};
    Hasher m_hasher{};
    KeyEqual m_keyEqual{};

    size_type MixedHash(const Key& key) const
    {
        size_type hash = static_cast<size_type>(m_hasher(key));

        if constexpr (sizeof(size_type) == sizeof(uint64_t))
        {
            hash ^= hash >> 30;
            hash *= static_cast<size_type>(0xbf58476d1ce4e5b9ULL);
            hash ^= hash >> 27;
            hash *= static_cast<size_type>(0x94d049bb133111ebULL);
            hash ^= hash >> 31;
        }
        else
        {
            hash ^= hash >> 16;
            hash *= static_cast<size_type>(0x7feb352dU);
            hash ^= hash >> 15;
            hash *= static_cast<size_type>(0x846ca68bU);
            hash ^= hash >> 16;
        }

        return hash;
    }

    size_type FindIndex(const Key& key, size_type hash) const
    {
        size_type foundIndex = cInvalidIndex;

        if (m_capacity != 0)
        {
            const size_type mask = m_capacity - 1;
            size_type index      = hash & mask;

            for (size_type probeCount = 0; probeCount < m_capacity; ++probeCount)
            {
                const Slot& slot = m_pSlots[index];

                if (slot.state == SlotState::eEmpty)
                {
                    break;
                }

                if (slot.state == SlotState::eOccupied && slot.hash == hash &&
                    m_keyEqual(slot.GetValue()->first, key))
                {
                    foundIndex = index;
                    break;
                }

                index = (index + 1) & mask;
            }
        }

        return foundIndex;
    }

    ProbeResult FindInsertionSlot(const Key& key, size_type hash) const
    {
        ASSERT(m_capacity != 0);
        const size_type mask        = m_capacity - 1;
        size_type index             = hash & mask;
        size_type firstDeletedIndex = cInvalidIndex;
        ProbeResult result{cInvalidIndex, false};

        for (size_type probeCount = 0; probeCount < m_capacity; ++probeCount)
        {
            const Slot& slot = m_pSlots[index];

            if (slot.state == SlotState::eEmpty)
            {
                result.index = firstDeletedIndex == cInvalidIndex ? index : firstDeletedIndex;
                break;
            }

            if (slot.state == SlotState::eDeleted)
            {
                if (firstDeletedIndex == cInvalidIndex)
                {
                    firstDeletedIndex = index;
                }
            }
            else if (slot.hash == hash && m_keyEqual(slot.GetValue()->first, key))
            {
                result = {index, true};
                break;
            }

            index = (index + 1) & mask;
        }

        if (result.index == cInvalidIndex)
        {
            ASSERT(firstDeletedIndex != cInvalidIndex);
            result.index = firstDeletedIndex;
        }

        return result;
    }

    template <typename KeyArg, typename... Args>
    std::pair<iterator, bool> TryEmplaceInternal(KeyArg&& key, Args&&... args)
    {
        std::pair<iterator, bool> result{};

        const size_type hash          = MixedHash(key);
        const size_type existingIndex = FindIndex(key, hash);

        if (existingIndex != cInvalidIndex)
        {
            result = {iterator(this, existingIndex), false};
        }
        else
        {
            EnsureCapacityForInsert();

            const ProbeResult probe = FindInsertionSlot(key, hash);
            ASSERT(!probe.found && probe.index != cInvalidIndex);

            Slot& slot                   = m_pSlots[probe.index];
            const bool reusedDeletedSlot = slot.state == SlotState::eDeleted;

            std::construct_at(slot.GetStorage(), std::piecewise_construct,
                              std::forward_as_tuple(std::forward<KeyArg>(key)),
                              std::forward_as_tuple(std::forward<Args>(args)...));
            slot.hash  = hash;
            slot.state = SlotState::eOccupied;
            ++m_size;

            if (reusedDeletedSlot)
            {
                --m_deletedCount;
            }

            result = {iterator(this, probe.index), true};
        }

        return result;
    }

    void EnsureCapacityForInsert()
    {
        if (m_capacity == 0)
        {
            RehashInternal(cMinimumCapacity);
            return;
        }

        const size_type maxOccupied = MaxOccupiedForCapacity(m_capacity);

        if (m_size + m_deletedCount + 1 > maxOccupied)
        {
            if (m_size + 1 > maxOccupied)
            {
                ASSERT(m_capacity <= std::numeric_limits<size_type>::max() / 2);
                RehashInternal(m_capacity * 2);
            }
            else
            {
                RehashInternal(m_capacity);
            }
        }
    }

    void EraseSlot(size_type index)
    {
        Slot& slot = m_pSlots[index];
        ASSERT(slot.state == SlotState::eOccupied);

        std::destroy_at(slot.GetValue());
        slot.hash  = 0;
        slot.state = SlotState::eDeleted;
        --m_size;
        ++m_deletedCount;

        if (m_size == 0)
        {
            clear();
        }
    }

    static size_type MaxOccupiedForCapacity(size_type capacity)
    {
        return (capacity / cMaxLoadDenominator) * cMaxLoadNumerator +
            ((capacity % cMaxLoadDenominator) * cMaxLoadNumerator) / cMaxLoadDenominator;
    }

    static size_type CapacityForElements(size_type elementCount)
    {
        size_type result{};

        if (!(elementCount == 0))
        {
            ASSERT(elementCount <=
                   (std::numeric_limits<size_type>::max() - (cMaxLoadNumerator - 1)) /
                       cMaxLoadDenominator);

            const size_type requiredBuckets =
                (elementCount * cMaxLoadDenominator + cMaxLoadNumerator - 1) / cMaxLoadNumerator;
            result = NextPowerOfTwo(requiredBuckets);
        }

        return result;
    }

    static size_type NextPowerOfTwo(size_type value)
    {
        size_type result = cMinimumCapacity;

        while (result < value)
        {
            ASSERT(result <= std::numeric_limits<size_type>::max() / 2);
            result *= 2;
        }

        return result;
    }

    static Slot* AllocateSlots(size_type capacity, void*& pAllocation)
    {
        ASSERT(IsPowerOfTwo(capacity));
        ASSERT(capacity <=
               (std::numeric_limits<size_type>::max() - (alignof(Slot) - 1)) / sizeof(Slot));

        const size_type allocationSize = sizeof(Slot) * capacity + alignof(Slot) - 1;
        pAllocation                    = ZEN_MEM_ALLOC(allocationSize);
        ASSERT(pAllocation != nullptr);

        const uintptr_t address = reinterpret_cast<uintptr_t>(pAllocation);
        const uintptr_t alignedAddress =
            (address + alignof(Slot) - 1) & ~(static_cast<uintptr_t>(alignof(Slot)) - 1);
        Slot* pSlots = reinterpret_cast<Slot*>(alignedAddress);

        for (size_type i = 0; i < capacity; ++i)
        {
            std::construct_at(&pSlots[i]);
        }

        return pSlots;
    }

    static size_type FindEmptySlot(Slot* pSlots, size_type capacity, size_type hash)
    {
        const size_type mask = capacity - 1;
        size_type index      = hash & mask;

        while (pSlots[index].state == SlotState::eOccupied)
        {
            index = (index + 1) & mask;
        }

        return index;
    }

    void RehashInternal(size_type newCapacity)
    {
        ASSERT(IsPowerOfTwo(newCapacity));
        ASSERT(newCapacity >= CapacityForElements(m_size));

        void* pNewAllocation = nullptr;
        Slot* pNewSlots      = AllocateSlots(newCapacity, pNewAllocation);

        for (size_type i = 0; i < m_capacity; ++i)
        {
            Slot& oldSlot = m_pSlots[i];

            if (oldSlot.state != SlotState::eOccupied)
            {
                continue;
            }

            value_type* pOldValue    = oldSlot.GetValue();
            const size_type newIndex = FindEmptySlot(pNewSlots, newCapacity, oldSlot.hash);
            Slot& newSlot            = pNewSlots[newIndex];

            std::construct_at(newSlot.GetStorage(), std::piecewise_construct,
                              std::forward_as_tuple(pOldValue->first),
                              std::forward_as_tuple(std::move_if_noexcept(pOldValue->second)));
            newSlot.hash  = oldSlot.hash;
            newSlot.state = SlotState::eOccupied;

            std::destroy_at(pOldValue);
        }

        for (size_type i = 0; i < m_capacity; ++i)
        {
            std::destroy_at(&m_pSlots[i]);
        }

        if (m_pAllocation != nullptr)
        {
            ZEN_MEM_FREE(m_pAllocation);
        }

        m_pAllocation  = pNewAllocation;
        m_pSlots       = pNewSlots;
        m_deletedCount = 0;
        m_capacity     = newCapacity;
    }

    void ReleaseStorage()
    {
        if (m_pSlots != nullptr)
        {
            clear();

            for (size_type i = 0; i < m_capacity; ++i)
            {
                std::destroy_at(&m_pSlots[i]);
            }
        }

        if (m_pAllocation != nullptr)
        {
            ZEN_MEM_FREE(m_pAllocation);
        }

        m_pAllocation  = nullptr;
        m_pSlots       = nullptr;
        m_size         = 0;
        m_deletedCount = 0;
        m_capacity     = 0;
    }
};

template <typename Key, typename Value, typename Hasher, typename KeyEqual>
void swap(FlatHashMap<Key, Value, Hasher, KeyEqual>& lhs,
          FlatHashMap<Key, Value, Hasher, KeyEqual>& rhs)
{
    lhs.Swap(rhs);
}
} // namespace zen
