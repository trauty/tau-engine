#pragma once

#include "tau/asset.h"
#include "tau/defines.h"

#include "json/json.hpp"
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tau
{
    struct TAU_ENGINE_API prefab_t
    {
        static constexpr std::string_view type_key = "prefab";

        nlohmann::json data;
    };

    template <>
    struct TAU_ENGINE_API asset_loader_t<prefab_t>
    {
        static std::optional<prefab_t> load(const std::string& path, std::span<const u8_t> bytes);
    };
} // namespace tau
