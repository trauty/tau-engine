#include "prefab.h"

#include "tau/hash.h"
#include "tau/log.h"

#include <vector>

namespace tau
{
    std::optional<prefab_t> asset_loader_t<prefab_t>::load(const std::string& path, std::span<const u8_t> bytes)
    {
        const std::string cooked_path = assets::cooked_path(get_type_id<prefab_t>(), path);
        nlohmann::json data = nlohmann::json::from_cbor(bytes, true, false);
        if (!data.is_object() || !data.value("entities", nlohmann::json{}).is_array())
        {
            TAU_LOG_ERROR("PREFAB", "Not a prefab, recook: {}", cooked_path);
            return std::nullopt;
        }

        return prefab_t{std::move(data)};
    }
} // namespace tau
