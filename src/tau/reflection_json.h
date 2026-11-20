#pragma once

#include "tau/defines.h"
#include "tau/reflection.h"

#include "json/json.hpp"
#include <string>
#include <vector>

// a reflected value as plain JSON for hand readable files
namespace tau::reflection_json
{
    TAU_ENGINE_API nlohmann::json to_json(const reflection::any_t& value);

    // the fields of a struct that differ from base, nested structs recurse, {} when none
    TAU_ENGINE_API nlohmann::json diff(const reflection::any_t& value, const reflection::any_t& base);

    // sets the keys present, the rest keep their value
    // keys and values it cannot use land in `rejected` as "key" / "key: value"
    TAU_ENGINE_API void apply(reflection::any_t& value, const nlohmann::json& object,
                              std::vector<std::string>* rejected = nullptr);
} // namespace tau::reflection_json
