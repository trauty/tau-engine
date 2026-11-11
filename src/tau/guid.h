#pragma once

#include "tau/defines.h"

#include <string>
#include <string_view>

namespace tau::guid
{
    TAU_ENGINE_API u64_t generate();

    TAU_ENGINE_API std::string to_string(u64_t guid);
    TAU_ENGINE_API u64_t from_string(std::string_view text);
} // namespace tau::guid
