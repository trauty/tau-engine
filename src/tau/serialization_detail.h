#pragma once

#include "tau/assets/scene.h"
#include "tau/defines.h"
#include "tau/ecs.h"
#include "tau/reflection.h"
#include "tau/systems/guid.h"
#include "tau/world.h"

#include "json/json.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace tau::serialization::detail
{
    struct entity_refs_t
    {
        const tau::ecs::registry_t* reg = nullptr;
        const std::unordered_map<u64_t, tau::ecs::entity_t>* by_file_id = nullptr;
        bool search_world = false;

        u64_t encode(tau::ecs::entity_t entity) const { return reg ? tau::guid_system::of(*reg, entity) : 0; }

        tau::ecs::entity_t decode(u64_t file_id) const
        {
            if (by_file_id)
            {
                const auto it = by_file_id->find(file_id);
                if (it != by_file_id->end()) { return it->second; }
            }
            return search_world && reg ? tau::guid_system::find(*reg, file_id) : tau::ecs::NULL_ENTITY;
        }
    };

    bool is_numeric_key(const std::string& key);
    u32_t key_to_hash(const std::string& key);
    std::string describe_key(const std::string& name, u32_t hash);
    asset_guid_t ref_guid(const nlohmann::json& ref);

    std::unordered_map<u32_t, tau::reflection::type_t> component_types_by_pool(tau::world_t& world);

    nlohmann::json encode_fields(tau::reflection::ctx_t& ctx, tau::reflection::type_t type,
                                 tau::reflection::any_t& instance, const entity_refs_t& refs);

    bool parse_value(const nlohmann::json& val, const std::string& prop_key, scene_property_t& prop);
    void adopt_index_refs(std::vector<scene_property_t>& props, bool generated_ids);

    void apply_fields(tau::reflection::ctx_t& ctx, tau::reflection::any_t& object, tau::reflection::type_t type,
                      const std::vector<scene_property_t>& props, const entity_refs_t& refs, const std::string& owner,
                      std::vector<std::string>* unknown = nullptr);

    bool first_warning(const std::string& key);

    nlohmann::json pod_to_json(pod_layout_t layout, const u8_t* bytes);
    bool pod_from_json(pod_layout_t layout, const nlohmann::json& value, u8_t* out);
} // namespace tau::serialization::detail
