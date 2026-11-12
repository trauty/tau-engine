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
// prefabs hold instance entries too (nested); instances read the flat data:
// nested entries expanded, nested parts tagged one level deep, keyed by their guid in the prefab
namespace tau::prefab
{
    // prefab data by guid + recorded path, null if missing
    using raw_lookup_t = std::function<const nlohmann::json*(asset_guid_t guid, const std::string& path)>;
    // same, flat data
    using lookup_t = raw_lookup_t;

    // loaded prefab assets, raw
    TAU_ENGINE_API const nlohmann::json* asset_raw(asset_guid_t guid, const std::string& path);
    // flat data from raw, memoized per returned lookup, a prefab holding itself counts as missing
    TAU_ENGINE_API lookup_t flattening(raw_lookup_t raw);
    TAU_ENGINE_API lookup_t asset_lookup();
    TAU_ENGINE_API const nlohmann::json* lookup_asset(const lookup_t& lookup, asset_handle_t prefab);

    // raw prefab data holds target somewhere down its nested instances
    TAU_ENGINE_API bool holds(const nlohmann::json& prefab_data, const nlohmann::json* target,
                              const raw_lookup_t& raw);
    // loaded nested prefabs, any depth
    TAU_ENGINE_API void collect_nested(const nlohmann::json& prefab_data, std::vector<asset_handle_t>& out);

    TAU_ENGINE_API bool is_instance_entry(const nlohmann::json& entity);

    // instance entries replaced by their parts in place
    TAU_ENGINE_API nlohmann::json expand(const nlohmann::json& scene_data, const lookup_t& lookup);
    // parts of one instance, root first; missing prefab leaves the root
    TAU_ENGINE_API std::vector<nlohmann::json> expand_instance(const nlohmann::json& entry,
                                                               const nlohmann::json* prefab_data);

    // entry for an instance's part states, any order
    TAU_ENGINE_API nlohmann::json collapse_instance(const nlohmann::json* prefab_data,
                                                    std::span<const nlohmann::json> parts);
    // expand undone: instance parts back into entries at their root's place,
    // parts whose root is not among the states become plain
    TAU_ENGINE_API nlohmann::json collapse(std::span<const nlohmann::json> states, const lookup_t& lookup);

    // prefab_part / prefab / removed / unresolved of a state, instance guid remapped
    TAU_ENGINE_API nlohmann::json tags_of(const nlohmann::json& state, u64_t instance);
    TAU_ENGINE_API void strip_tags(nlohmann::json& state);

    TAU_ENGINE_API nlohmann::json new_instance(const nlohmann::json& prefab_data, const nlohmann::json& prefab_ref);

    // what one part changes against the prefab entity it comes from:
    // {"components": {component: {field: value}}, "removed": [component]}, empty when nothing
    TAU_ENGINE_API nlohmann::json diff_part(const nlohmann::json* prefab_data, std::span<const nlohmann::json> parts,
                                            const nlohmann::json& part);

    // runtime instance, returns the root
    TAU_ENGINE_API ecs::entity_t spawn(world_t& world, asset_t<prefab_t> prefab);
} // namespace tau::prefab
