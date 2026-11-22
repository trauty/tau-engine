#pragma once

#include "tau/asset.h"
#include "tau/defines.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tau
{
    struct TAU_ENGINE_API model_sub_asset_t
    {
        std::string name;
        u64_t type_id = 0;
        asset_guid_t guid = 0;
    };

    struct TAU_ENGINE_API model_t
    {
        static constexpr std::string_view type_key = "model";

        std::vector<model_sub_asset_t> sub_assets;
    };

    template <>
    struct TAU_ENGINE_API asset_loader_t<model_t>
    {
        static std::optional<model_t> load(const std::string& path, std::span<const u8_t> bytes);
    };
} // namespace tau
