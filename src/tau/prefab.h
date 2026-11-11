#pragma once

#include "tau/asset.h"
#include "tau/defines.h"
#include "tau/ecs_fwd.h"

#include "json/json.hpp"
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace tau
{
    struct world_t;
    struct prefab_t;
} // namespace tau

// scene stores an instance as one entry at its root:
// the prefab, which guid each prefab entity has here, and the fields that differ from the prefab
// expand turns entries into entity states, collapse turns an instance's states back into an entry
namespace tau::prefab
{
    // prefab data by guid + recorded path, null if missing
    using lookup_t = std::function<const nlohmann::json*(asset_guid_t guid, const std::string& path)>;

    TAU_ENGINE_API const nlohmann::json* asset_lookup(asset_guid_t guid, const std::string& path);

    TAU_ENGINE_API bool is_instance_entry(const nlohmann::json& entity);

    // instance entries replaced by their parts in place
    TAU_ENGINE_API nlohmann::json expand(const nlohmann::json& scene_data, const lookup_t& lookup);
    // parts of one instance, root first; missing prefab leaves the root
    TAU_ENGINE_API std::vector<nlohmann::json> expand_instance(const nlohmann::json& entry,
                                                               const nlohmann::json* prefab_data);

    // entry for an instance's part states, any order
    TAU_ENGINE_API nlohmann::json collapse_instance(const nlohmann::json* prefab_data,
                                                    std::span<const nlohmann::json> parts);

    TAU_ENGINE_API nlohmann::json new_instance(const nlohmann::json& prefab_data, const nlohmann::json& prefab_ref);

    // what one part changes against the prefab entity it comes from:
    // {"components": {component: {field: value}}, "removed": [component]}, empty when nothing
    TAU_ENGINE_API nlohmann::json diff_part(const nlohmann::json* prefab_data, std::span<const nlohmann::json> parts,
                                            const nlohmann::json& part);

    // runtime instance, returns the root
    TAU_ENGINE_API ecs::entity_t spawn(world_t& world, asset_t<prefab_t> prefab);
} // namespace tau::prefab
