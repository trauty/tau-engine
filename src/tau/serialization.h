#pragma once

#include "tau/assets/scene.h"
#include "tau/defines.h"
#include "tau/world.h"

#include "json/json.hpp"
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace tau::serialization
{
    // assigns guids, instances become one entry each
    TAU_ENGINE_API nlohmann::json serialize_scene(tau::world_t& world);
    // returns created entities in scene order, instances expanded
    TAU_ENGINE_API std::vector<ecs::entity_t> deserialize_scene(tau::world_t& world, const nlohmann::json& scene_data);

    TAU_ENGINE_API scene_t scene_from_json(const nlohmann::json& scene_data);
    TAU_ENGINE_API void write_scene_binary(const scene_t& scene, const std::string& output_path);
    TAU_ENGINE_API std::vector<ecs::entity_t> instantiate_scene(tau::world_t& world, const scene_t& scene);

    TAU_ENGINE_API void clear_scene(tau::world_t& world);

    // one entity for undo: guid, parent, next sibling, components, refs as guids
    TAU_ENGINE_API nlohmann::json serialize_entity(tau::world_t& world, ecs::entity_t entity);

    struct entity_state_t
    {
        u64_t guid = 0;
        const nlohmann::json* state = nullptr;
    };

    // creates missing, replaces existing in place so refs stay valid
    TAU_ENGINE_API void apply_entities(tau::world_t& world, std::span<const entity_state_t> states);

    // copies under new guids: old -> new, refs among them follow
    TAU_ENGINE_API void duplicate_entities(tau::world_t& world, std::span<const ecs::entity_t> entities,
                                           const std::unordered_map<u64_t, u64_t>& new_guids);

    TAU_ENGINE_API void remap_entity_refs(nlohmann::json& components,
                                          const std::unordered_map<u64_t, u64_t>& new_guids);

    TAU_ENGINE_API std::vector<ecs::entity_t> find_referrers(tau::world_t& world,
                                                             std::span<const ecs::entity_t> targets);

    // asset ref: {"g": guid, "p": path}, no guid if path only
    TAU_ENGINE_API nlohmann::json asset_ref(asset_handle_t handle);

    TAU_ENGINE_API void collect_asset_refs(tau::world_t& world, std::vector<asset_handle_t>& out);

    struct unknown_key_t
    {
        std::string component;
        std::string field;

        bool operator==(const unknown_key_t&) const = default;
    };

    TAU_ENGINE_API std::vector<unknown_key_t> unknown_keys(const tau::world_t& world, const scene_t& scene);
} // namespace tau::serialization
