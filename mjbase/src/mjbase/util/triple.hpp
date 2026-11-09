#pragma once

#include "libca/core/any.hpp"
#include "libca/log/log_macros.hpp"
#include "libca/str/utf8_string.hpp"
#include <cerrno>
#include <cstring>
#include <functional>
#include <iostream>
#include <list>
#include <ostream>
#include <unordered_map>
#include <utility>

namespace mjbase::util {

template<typename T1, typename T2, typename T3>
struct Triple
{
    T1 first;
    T2 second;
    T3 third;

    Triple()
        : first{}
        , second{}
        , third{}
    {}
    Triple(T1 f, T2 s, T3 t)
        : first(std::move(f))
        , second(std::move(s))
        , third(std::move(t))
    {}

    T1&       getFirst() { return first; }
    const T1& getFirst() const { return first; }

    T2&       getSecond() { return second; }
    const T2& getSecond() const { return second; }

    T3&       getThird() { return third; }
    const T3& getThird() const { return third; }

    static Triple of(T1 f, T2 s, T3 t) { return Triple(std::move(f), std::move(s), std::move(t)); }

    bool operator==(const Triple& other) const
    {
        return first == other.first && second == other.second && third == other.third;
    }

    bool operator!=(const Triple& other) const { return !(*this == other); }

    /// @brief 展示/序列化出口："(a,b,c)" 形态。
    ///
    /// 返回 Utf8String——本地 std::string 缓冲拼接后按字节收进
    /// （元素为数值/字面量文本，展示域）。
    ca::str::Utf8String toString() const
    {
        std::string out;
        out += '(';
        out += toStringImpl(first);
        out += ',';
        out += toStringImpl(second);
        out += ',';
        out += toStringImpl(third);
        out += ')';
        return ca::str::Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(out.data()),
                                                        out.size());
    }

    /// @brief 格式化展示出口：format 给出 printf 风格模板（三个 %s 槽）。
    template<typename... Args>
    ca::str::Utf8String toString(const std::string_view format) const
    {
        // string_view 不保证 \0 终止，snprintf 的格式串需先落本地缓冲。
        const std::string fmt(format);
        char buf[256];
        snprintf(buf,
                 sizeof(buf),
                 fmt.c_str(),
                 toStringImpl(first).c_str(),
                 toStringImpl(second).c_str(),
                 toStringImpl(third).c_str());
        return ca::str::Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(buf),
                                                        std::strlen(buf));
    }

private:
    template<typename T>
    static std::string toStringImpl(const T& v)
    {
        if constexpr (std::is_same_v<T, std::string>) {
            return v;
        }
        else if constexpr (std::is_same_v<T, const char*>) {
            return v ? std::string(v) : "null";
        }
        else {
            return std::to_string(v);
        }
    }
};

// -----------------------------------------------------------------------
//  TriplePool: LRU-cached Triple factory (avoids duplicate allocations)
// -----------------------------------------------------------------------

namespace detail {

template<typename K, typename V>
class LruCache
{
    using List = std::list<K>;
    using Map  = std::unordered_map<K, std::pair<V, typename List::iterator>>;

    size_t capacity_;
    List   list_;
    Map    map_;

public:
    explicit LruCache(size_t cap)
        : capacity_(cap)
    {}

    bool contains(const K& key) const { return map_.find(key) != map_.end(); }

    V* get(const K& key)
    {
        auto it = map_.find(key);
        if (it == map_.end()) return nullptr;
        list_.splice(list_.begin(), list_, it->second.second);
        return &it->second.first;
    }

    void put(const K& key, V value)
    {
        auto it = map_.find(key);
        if (it != map_.end()) {
            list_.erase(it->second.second);
            map_.erase(it);
        }
        list_.push_front(key);
        map_[key] = {std::move(value), list_.begin()};
        if (map_.size() > capacity_) {
            auto last = list_.back();
            list_.pop_back();
            map_.erase(last);
        }
    }
};

}   // namespace detail

class TriplePool
{
    static constexpr int                         capacity = 5000;
    static detail::LruCache<int, ca::core::Any>& cache()
    {
        static detail::LruCache<int, ca::core::Any> inst(capacity);
        return inst;
    }

public:
    template<typename T1, typename T2, typename T3>
    static Triple<T1, T2, T3> of(T1 first, T2 second, T3 third)
    {
        size_t seed = 0;
        seed ^= std::hash<T1>{}(first) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed ^= std::hash<T2>{}(second) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed ^= std::hash<T3>{}(third) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        size_t key = seed;

        auto& c     = cache();
        auto* entry = c.get(key);
        if (entry) {
            // Any::cast<T>() 返回引用且不做类型检查；缓存键是哈希值，必须用
            // as<T>() 安全取值（类型不符返回 nullptr，哈希冲突时按 miss 处理）。
            auto* cached = entry->template as<Triple<T1, T2, T3>>();
            if (cached != nullptr && cached->first == first && cached->second == second &&
                cached->third == third) {
                return *cached;
            }
            if (cached != nullptr) {
                CA_LOG_WARN("TriplePool hash collision: new ({},{},{}) vs old ({},{},{})",
                            first, second, third, cached->first, cached->second,
                            cached->third);
            }
        }

        Triple<T1, T2, T3> t(first, second, third);
        c.put(key, ca::core::Any(t));
        return t;
    }
};

}   // namespace mjbase::util

namespace std {

template<typename T1, typename T2, typename T3>
struct hash<mjbase::util::Triple<T1, T2, T3>>
{
    size_t operator()(const mjbase::util::Triple<T1, T2, T3>& t) const
    {
        size_t seed = 0;
        seed ^= hash<T1>{}(t.first) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed ^= hash<T2>{}(t.second) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed ^= hash<T3>{}(t.third) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};

}   // namespace std
