#pragma once

#include "tau/guid.h"

#include "json/json.hpp"

// entity and asset guids in JSON: 16 hex digits, null for none
namespace tau::guid
{
    inline nlohmann::json to_json(u64_t guid) { return guid == 0 ? nlohmann::json(nullptr) : nlohmann::json(to_string(guid)); }

    inline u64_t from_json(const nlohmann::json& value)
    { return value.is_string() ? from_string(value.get<std::string>()) : 0; }
} // namespace tau::guid
