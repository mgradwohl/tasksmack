#pragma once

// A map that holds at most a fixed number of entries, dropping the least recently used one to make
// room (#1800: the process icon cache's bounded memory). Not thread-safe; the owner locks.
//
// find() and put() on a present key move it to the front without allocating: the order is a list
// whose nodes are spliced, and the index maps keys to list positions.

#include <cstddef>
#include <functional>
#include <list>
#include <optional>
#include <unordered_map>
#include <utility>

namespace Domain
{

template<typename Key, typename Value, typename Hash = std::hash<Key>> class LruMap
{
  public:
    /// @p capacity entries at most; 0 is treated as 1, so a put() is never dropped at once.
    explicit LruMap(std::size_t capacity) : m_Capacity(capacity == 0 ? 1 : capacity)
    {
        m_Index.reserve(m_Capacity);
    }

    /// The value for @p key, made the most recently used; nullptr when absent.
    [[nodiscard]] Value* find(const Key& key)
    {
        const auto it = m_Index.find(key);
        if (it == m_Index.end())
        {
            return nullptr;
        }
        m_Order.splice(m_Order.begin(), m_Order, it->second);
        return &it->second->second;
    }

    /// The value for @p key without changing the order; nullptr when absent.
    [[nodiscard]] const Value* peek(const Key& key) const
    {
        const auto it = m_Index.find(key);
        return it == m_Index.end() ? nullptr : &it->second->second;
    }

    /// Sets @p key to @p value as the most recently used entry, dropping the least recently used
    /// entry when that makes one too many. Returns the dropped entry, for an owner that must release
    /// what it holds (a texture).
    std::optional<std::pair<Key, Value>> put(Key key, Value value)
    {
        if (const auto it = m_Index.find(key); it != m_Index.end())
        {
            it->second->second = std::move(value);
            m_Order.splice(m_Order.begin(), m_Order, it->second);
            return std::nullopt;
        }
        m_Order.emplace_front(std::move(key), std::move(value));
        try
        {
            m_Index.emplace(m_Order.front().first, m_Order.begin());
        }
        catch (...)
        {
            m_Order.pop_front(); // Keep the list and the index in step
            throw;
        }
        if (m_Order.size() <= m_Capacity)
        {
            return std::nullopt;
        }
        m_Index.erase(m_Order.back().first);
        std::optional<std::pair<Key, Value>> dropped(std::move(m_Order.back()));
        m_Order.pop_back();
        return dropped;
    }

    /// Removes @p key; whether it was present.
    bool erase(const Key& key)
    {
        const auto it = m_Index.find(key);
        if (it == m_Index.end())
        {
            return false;
        }
        m_Order.erase(it->second);
        m_Index.erase(it);
        return true;
    }

    void clear() noexcept
    {
        m_Index.clear();
        m_Order.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Order.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return m_Capacity;
    }

    /// Calls @p visit(key, value) for every entry, most recently used first.
    template<typename Visit> void forEach(const Visit& visit) const
    {
        for (const auto& [key, value] : m_Order)
        {
            visit(key, value);
        }
    }

  private:
    using Order = std::list<std::pair<Key, Value>>;

    std::size_t m_Capacity;
    Order m_Order; // Most recently used first
    std::unordered_map<Key, typename Order::iterator, Hash> m_Index;
};

} // namespace Domain
