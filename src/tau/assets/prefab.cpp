#include "prefab.h"

#include "tau/hash.h"
#include "tau/log.h"
#include "tau/vfs.h"

#include <vector>

namespace tau
{
    std::optional<prefab_t> asset_loader_t<prefab_t>::load(const std::string& path)
    {
        const std::string cooked_path = assets::cooked_path(get_type_id<prefab_t>(), path);
        const std::vector<u8_t> bytes = vfs::read_bytes(cooked_path);
        if (bytes.empty())
        {
            TAU_LOG_ERROR("PREFAB", "Prefab not found: {}", cooked_path);
            return std::nullopt;
        }

        nlohmann::json data = nlohmann::json::from_cbor(bytes, true, false);
        if (!data.is_object() || !data.value("entities", nlohmann::json{}).is_array())
        {
            TAU_LOG_ERROR("PREFAB", "Not a prefab, recook: {}", cooked_path);
            return std::nullopt;
        }

        return prefab_t{std::move(data)};
    }
} // namespace tau
