#pragma once

#include "libca/core/datatype.hpp"

namespace mjbase::util {

template<class T>
struct Span
{
    T*        data = nullptr;
    ca::usize size = 0;

    T*   begin() const { return data; }
    T*   end() const { return data + size; }
    bool empty() const { return size == 0; }
    T&   operator[](ca::usize i) const { return data[i]; }
    T&   front() const { return data[0]; }
    T&   back() const { return data[size - 1]; }
};

}   // namespace mjbase::util
