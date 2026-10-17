#pragma once

#include <functional>
#include <ostream>
#include <utility>

namespace mjbase::util {

template<typename A, typename B>
struct Pair
{
    A first;
    B second;

    Pair()
        : first{}
        , second{}
    {}
    Pair(A f, B s)
        : first(std::move(f))
        , second(std::move(s))
    {}

    A&       getFirst() { return first; }
    const A& getFirst() const { return first; }
    void     setFirst(const A& f) { first = f; }

    B&       getSecond() { return second; }
    const B& getSecond() const { return second; }
    void     setSecond(const B& s) { second = s; }

    static Pair of(A f, B s) { return Pair(std::move(f), std::move(s)); }

    bool operator==(const Pair& other) const
    {
        return first == other.first && second == other.second;
    }

    bool operator!=(const Pair& other) const { return !(*this == other); }
};

template<typename A, typename B>
std::ostream& operator<<(std::ostream& os, const Pair<A, B>& p)
{
    os << "Pair{first=" << p.first << ", second=" << p.second << "}";
    return os;
}

}   // namespace mjbase::util

namespace std {

template<typename A, typename B>
struct hash<mjbase::util::Pair<A, B>>
{
    size_t operator()(const mjbase::util::Pair<A, B>& p) const
    {
        size_t h1 = hash<A>{}(p.first);
        size_t h2 = hash<B>{}(p.second);
        return h1 ^ (h2 << 1);
    }
};

}   // namespace std
