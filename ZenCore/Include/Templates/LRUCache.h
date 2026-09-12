#pragma once

#include <cstddef>
#include <functional>
#include <iterator>
#include <list>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace zen
{
/// A bounded cache with average O(1) lookup, insertion, and eviction.
///
/// Entries are iterated from most to least recently used. Non-const find(), at(),
/// operator[], and insertion refresh recency; const lookup, contains(), and iteration do not.
/// References and iterators remain valid until their entry is erased or evicted.
/// Raw pointer values are non-owning. The optional callback runs before automatic
/// eviction (including capacity reduction), but not on erase(), clear(), or destruction.
/// Callbacks must not modify this cache. The cache is not thread-safe.
template <typename Key,
          typename Value,
          typename Hasher   = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
class LRUCache
{
public:
    using key_type         = Key;
    using mapped_type      = Value;
    using value_type       = std::pair<const Key, Value>;
    using size_type        = std::size_t;
    using EvictionCallback = std::function<void(const Key&, Value&)>;

private:
    using EntryList = std::list<value_type>;
    using Index     = std::unordered_map<Key, typename EntryList::iterator, Hasher, KeyEqual>;

public:
    using iterator       = typename EntryList::iterator;
    using const_iterator = typename EntryList::const_iterator;

    static constexpr size_type kDefaultCapacity = 256;

    explicit LRUCache(size_type capacity          = kDefaultCapacity,
                      EvictionCallback onEviction = {},
                      const Hasher& hash          = Hasher(),
                      const KeyEqual& equal       = KeyEqual()) :
        m_index(0, hash, equal), m_capacity(capacity), m_onEviction(std::move(onEviction))
    {}

    LRUCache(const LRUCache& other)
        requires(std::is_copy_constructible_v<Value>)
        :
        m_index(0, other.m_index.hash_function(), other.m_index.key_eq()),
        m_entries(other.m_entries),
        m_capacity(other.m_capacity),
        m_onEviction(other.m_onEviction)
    {
        // Copied index iterators must refer to this cache's list, not the source list.
        for (iterator it = m_entries.begin(); it != m_entries.end(); ++it)
        {
            m_index.emplace(it->first, it);
        }
    }

    LRUCache& operator=(const LRUCache& other)
        requires(std::is_copy_constructible_v<Value>)
    {
        if (this != &other)
        {
            LRUCache copy(other);
            Swap(copy);
        }

        return *this;
    }

    LRUCache(LRUCache&& other) :
        LRUCache(other.m_capacity, {}, other.m_index.hash_function(), other.m_index.key_eq())
    {
        Swap(other);
    }

    LRUCache& operator=(LRUCache&& other)
    {
        if (this != &other)
        {
            LRUCache moved(std::move(other));
            Swap(moved);
        }

        return *this;
    }

    bool empty() const noexcept
    {
        return m_entries.empty();
    }

    size_type size() const noexcept
    {
        return m_entries.size();
    }

    size_type capacity() const noexcept
    {
        return m_capacity;
    }

    /// Shrinking evicts the oldest entries first. Zero capacity disables insertion.
    void set_capacity(size_type capacity)
    {
        while (size() > capacity)
        {
            EvictLeastRecent();
        }

        m_capacity = capacity;
    }

    iterator begin() noexcept
    {
        return m_entries.begin();
    }

    const_iterator begin() const noexcept
    {
        return m_entries.begin();
    }

    const_iterator cbegin() const noexcept
    {
        return m_entries.cbegin();
    }

    iterator end() noexcept
    {
        return m_entries.end();
    }

    const_iterator end() const noexcept
    {
        return m_entries.end();
    }

    const_iterator cend() const noexcept
    {
        return m_entries.cend();
    }

    bool contains(const Key& key) const
    {
        return m_index.find(key) != m_index.end();
    }

    iterator find(const Key& key)
    {
        iterator result{};

        typename Index::iterator it = m_index.find(key);

        if (it == m_index.end())
        {
            result = end();
        }
        else
        {
            Touch(it->second);
            result = it->second;
        }

        return result;
    }

    const_iterator find(const Key& key) const
    {
        typename Index::const_iterator it = m_index.find(key);
        return it == m_index.end() ? end() : const_iterator(it->second);
    }

    Value& at(const Key& key)
    {
        iterator it = find(key);

        if (it == end())
        {
            throw std::out_of_range("LRUCache::at: key not found");
        }

        return it->second;
    }

    const Value& at(const Key& key) const
    {
        const_iterator it = find(key);

        if (it == end())
        {
            throw std::out_of_range("LRUCache::at: key not found");
        }

        return it->second;
    }

    Value& operator[](const Key& key)
    {
        return Subscript(key);
    }

    Value& operator[](Key&& key)
    {
        return Subscript(std::move(key));
    }

    /// Returns {end(), false} when capacity is zero. Existing values are not replaced.
    template <typename... Args>
    std::pair<iterator, bool> try_emplace(const Key& key, Args&&... args)
    {
        return TryEmplaceInternal(key, std::forward<Args>(args)...);
    }

    template <typename... Args> std::pair<iterator, bool> try_emplace(Key&& key, Args&&... args)
    {
        return TryEmplaceInternal(std::move(key), std::forward<Args>(args)...);
    }

    template <typename ValueArg>
    std::pair<iterator, bool> insert_or_assign(const Key& key, ValueArg&& value)
    {
        return InsertOrAssignInternal(key, std::forward<ValueArg>(value));
    }

    template <typename ValueArg>
    std::pair<iterator, bool> insert_or_assign(Key&& key, ValueArg&& value)
    {
        return InsertOrAssignInternal(std::move(key), std::forward<ValueArg>(value));
    }

    size_type erase(const Key& key)
    {
        size_type result{};

        typename Index::iterator it = m_index.find(key);

        if (!(it == m_index.end()))
        {
            iterator entry = it->second;
            m_index.erase(it);
            m_entries.erase(entry);
            result = 1;
        }

        return result;
    }

    iterator erase(const_iterator position)
    {
        m_index.erase(position->first);
        return m_entries.erase(position);
    }

    void clear() noexcept
    {
        m_index.clear();
        m_entries.clear();
    }

    void Swap(LRUCache& other) noexcept(noexcept(m_index.swap(other.m_index)))
    {
        m_index.swap(other.m_index);
        m_entries.swap(other.m_entries);
        std::swap(m_capacity, other.m_capacity);
        m_onEviction.swap(other.m_onEviction);
    }

private:
    Index m_index;
    EntryList m_entries;
    size_type m_capacity;
    EvictionCallback m_onEviction;

    void Touch(iterator entry) noexcept
    {
        m_entries.splice(m_entries.begin(), m_entries, entry);
    }

    void EvictLeastRecent()
    {
        iterator entry              = std::prev(m_entries.end());
        typename Index::iterator it = m_index.find(entry->first);

        if (m_onEviction)
        {
            m_onEviction(entry->first, entry->second);
        }

        m_index.erase(it);
        m_entries.erase(entry);
    }

    template <typename KeyArg> Value& Subscript(KeyArg&& key)
    {
        std::pair<iterator, bool> result = TryEmplaceInternal(std::forward<KeyArg>(key));

        if (result.first == end())
        {
            throw std::length_error("LRUCache::operator[]: capacity is zero");
        }

        return result.first->second;
    }

    template <typename KeyArg, typename ValueArg>
    std::pair<iterator, bool> InsertOrAssignInternal(KeyArg&& key, ValueArg&& value)
    {
        std::pair<iterator, bool> result{};

        typename Index::iterator it = m_index.find(key);

        if (it != m_index.end())
        {
            it->second->second = std::forward<ValueArg>(value);
            Touch(it->second);
            result = {it->second, false};
        }
        else
        {
            result = TryEmplaceInternal(std::forward<KeyArg>(key), std::forward<ValueArg>(value));
        }

        return result;
    }

    template <typename KeyArg, typename... Args>
    std::pair<iterator, bool> TryEmplaceInternal(KeyArg&& key, Args&&... args)
    {
        std::pair<iterator, bool> result{};

        iterator existing = find(key);

        if (existing != end())
        {
            result = {existing, false};
        }
        else if (m_capacity == 0)
        {
            result = {end(), false};
        }
        else
        {
            // Construct before eviction so arguments may safely refer to cached entries.
            m_entries.emplace_front(std::piecewise_construct,
                                    std::forward_as_tuple(std::forward<KeyArg>(key)),
                                    std::forward_as_tuple(std::forward<Args>(args)...));
            iterator entry = m_entries.begin();
            typename Index::iterator indexEntry;

            try
            {
                indexEntry = m_index.emplace(entry->first, entry).first;
            }
            catch (...)
            {
                m_entries.pop_front();
                throw;
            }

            try
            {
                if (size() > m_capacity)
                {
                    EvictLeastRecent();
                }
            }
            catch (...)
            {
                m_index.erase(indexEntry);
                m_entries.pop_front();
                throw;
            }

            result = {entry, true};
        }

        return result;
    }
};

template <typename Key, typename Value, typename Hasher, typename KeyEqual>
void swap(LRUCache<Key, Value, Hasher, KeyEqual>& lhs,
          LRUCache<Key, Value, Hasher, KeyEqual>& rhs) noexcept(noexcept(lhs.Swap(rhs)))
{
    lhs.Swap(rhs);
}
} // namespace zen
