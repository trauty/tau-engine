#pragma once

#include "tau/defines.h"

#include <cstddef>
#include <string_view>

namespace tau
{
    // http://www.isthe.com/chongo/tech/comp/fnv/index.html
    constexpr u32_t hash_string(std::string_view str)
    {
        u32_t hash = 2166136261u;
        for (char c : str)
        {
            hash ^= static_cast<u8_t>(c);
            hash *= 16777619u;
        }
        return hash;
    }

    constexpr u64_t hash_string64(std::string_view str)
    {
        u64_t hash = 14695981039346656037ull;
        for (char c : str)
        {
            hash ^= static_cast<u8_t>(c);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    // the hash of the key T declares, never of a compiler-generated name: ids are cooked into scenes and
    // compared between the engine and game libraries, which need not come from the same compiler
    template <typename T>
    constexpr u64_t get_type_id()
    { return hash_string64(T::type_key); }

    constexpr u32_t operator""_h(const char* str, size_t len) { return hash_string(std::string_view(str, len)); }
    constexpr u64_t operator""_h64(const char* str, size_t len) { return hash_string64(std::string_view(str, len)); }
} // namespace tau