#pragma once

#include "tau/defines.h"

#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace tau
{
    struct byte_reader_t
    {
        std::span<const u8_t> data;
        std::size_t at = 0;
        bool ok = true;

        bool take(void* out, std::size_t size)
        {
            if (!ok || at + size > data.size())
            {
                ok = false;
                std::memset(out, 0, size);
                return false;
            }
            std::memcpy(out, data.data() + at, size);
            at += size;
            return true;
        }

        template <typename T>
        T read()
        {
            static_assert(std::is_trivially_copyable_v<T>);
            T value{};
            take(&value, sizeof(T));
            return value;
        }

        std::string string(u32_t length)
        {
            std::string text(length, '\0');
            if (length > 0) { take(text.data(), length); }
            return ok ? text : std::string{};
        }

        template <typename T>
        std::vector<T> array(u32_t count)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            std::vector<T> values(count);
            if (count > 0) { take(values.data(), sizeof(T) * count); }
            return ok ? values : std::vector<T>{};
        }
    };

    struct byte_writer_t
    {
        std::vector<u8_t> data;

        void put(const void* bytes, std::size_t size)
        {
            const u8_t* begin = static_cast<const u8_t*>(bytes);
            data.insert(data.end(), begin, begin + size);
        }

        template <typename T>
        void write(const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            put(&value, sizeof(T));
        }

        void string(const std::string& text) { put(text.data(), text.size()); }

        template <typename T>
        void array(const std::vector<T>& values)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            if (!values.empty()) { put(values.data(), sizeof(T) * values.size()); }
        }
    };
} // namespace tau
