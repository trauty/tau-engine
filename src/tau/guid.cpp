#include "tau/guid.h"

#include <charconv>
#include <mutex>
#include <random>

namespace tau::guid
{
    u64_t generate()
    {
        static std::mutex mutex;
        static std::mt19937_64 rng{std::random_device{}()};

        std::scoped_lock lock(mutex);
        u64_t guid = 0;
        while (guid == 0) { guid = rng(); }
        return guid;
    }

    std::string to_string(u64_t guid)
    {
        char buffer[17] = {};
        const auto [end, ec] = std::to_chars(buffer, buffer + 16, guid, 16);
        const std::string digits(buffer, end);
        return std::string(16 - digits.size(), '0') + digits;
    }

    u64_t from_string(std::string_view text)
    {
        if (text.size() != 16) { return 0; }

        u64_t guid = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), guid, 16);
        return (ec == std::errc{} && end == text.data() + text.size()) ? guid : 0;
    }
} // namespace tau::guid
