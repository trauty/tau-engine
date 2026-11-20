#include "tau/asset.h"
#include "tau/assets/prefab.h"
#include "tau/assets/scene_format.h"
#include "tau/components/prefab.h"
#include "tau/guid_json.h"
#include "tau/prefab.h"
#include "tau/serialization.h"
#include "tau/serialization_detail.h"

#include <algorithm>
#include <tuple>
#include <unordered_set>
#include <vector>

// queries and edits over saved or live data: refs, duplicates, assets, unknown keys
namespace tau::serialization
{
    using namespace detail;

    namespace
    {
        void remap_guid(nlohmann::json& value, const std::unordered_map<u64_t, u64_t>& new_guids)
        {
            const auto it = new_guids.find(guid::from_json(value));
            if (it != new_guids.end()) { value = guid::to_json(it->second); }
        }

    } // namespace

    void remap_entity_refs(nlohmann::json& value, const std::unordered_map<u64_t, u64_t>& new_guids)
    {
        if (value.is_array())
        {
            for (nlohmann::json& element : value) { remap_entity_refs(element, new_guids); }
            return;
        }
        if (!value.is_object()) { return; }

        const auto type = value.find("ty");
        if (type != value.end() && value.contains("v") && type->is_number() &&
            type->get<u8_t>() == static_cast<u8_t>(scene_value_type_e::ENTITY))
        {
            remap_guid(value["v"], new_guids);
            return;
        }

        for (auto& [key, child] : value.items()) { remap_entity_refs(child, new_guids); }
    }

    void duplicate_entities(tau::world_t& world, std::span<const tau::ecs::entity_t> entities,
                            const std::unordered_map<u64_t, u64_t>& new_guids)
    {
        std::vector<nlohmann::json> states;
        states.reserve(entities.size());
        std::vector<entity_state_t> batch;

        for (const tau::ecs::entity_t entity : entities)
        {
            nlohmann::json state = serialize_entity(world, entity);
            remap_guid(state["id"], new_guids);
            if (state.contains("parent")) { remap_guid(state["parent"], new_guids); }
            if (state.contains("next")) { remap_guid(state["next"], new_guids); }
            remap_entity_refs(state["components"], new_guids);

            // copied instance is new, part without its root is plain
            if (state.contains("prefab_part"))
            {
                if (new_guids.contains(guid::from_json(state["prefab_part"].value("instance", nlohmann::json{}))))
                {
                    remap_guid(state["prefab_part"]["instance"], new_guids);
                }
                else
                {
                    state.erase("prefab_part");
                }
            }

            const u64_t guid = guid::from_json(state["id"]);
            states.push_back(std::move(state));
            batch.push_back({guid, &states.back()});
        }

        apply_entities(world, batch);
    }

    namespace
    {
        bool refers_to(tau::world_t& world, tau::reflection::type_t type, tau::reflection::any_t& value,
                       const std::unordered_set<tau::ecs::entity_t>& targets)
        {
            tau::reflection::ctx_t& ctx = *world.reflection_ctx;

            if (type == tau::reflection::resolve<tau::ecs::entity_t>(ctx))
            {
                return targets.contains(value.cast<tau::ecs::entity_t>());
            }
            if (type.is_enum() || tau::reflection::pod_tag(type) || type == tau::reflection::resolve<std::string>(ctx))
            {
                return false;
            }

            if (type.is_sequence_container())
            {
                auto list = value.as_sequence_container();
                const tau::reflection::type_t element_type = list.value_type();
                for (tau::reflection::any_t element : list)
                {
                    if (refers_to(world, element_type, element, targets)) { return true; }
                }
                return false;
            }

            for (auto [data_id, data] : type.data())
            {
                const auto* prop = static_cast<tau::reflection::editor_prop_t*>(data.custom());
                if (prop && prop->asset_type_hash != 0) { continue; }

                tau::reflection::any_t field = data.get(value);
                if (refers_to(world, data.type(), field, targets)) { return true; }
            }
            return false;
        }
    } // namespace

    std::vector<tau::ecs::entity_t> find_referrers(tau::world_t& world, std::span<const tau::ecs::entity_t> targets)
    {
        const std::unordered_set<tau::ecs::entity_t> target_set(targets.begin(), targets.end());
        const std::unordered_map<u32_t, tau::reflection::type_t> by_pool = component_types_by_pool(world);

        std::unordered_set<tau::ecs::entity_t> found;
        for (auto [pool_id, pool] : world.registry.storage())
        {
            const auto type_it = by_pool.find(static_cast<u32_t>(pool_id));
            if (type_it == by_pool.end()) { continue; }

            const tau::reflection::type_t type = type_it->second;
            const tau::reflection::func_t get_func = type.func("get"_h);

            for (tau::ecs::entity_t entity : pool)
            {
                if (target_set.contains(entity) || found.contains(entity)) { continue; }

                tau::reflection::any_t instance =
                    get_func.invoke({}, tau::reflection::forward_as_meta(world.registry), entity);
                if (instance && refers_to(world, type, instance, target_set)) { found.insert(entity); }
            }
        }

        return {found.begin(), found.end()};
    }

    nlohmann::json asset_ref(asset_handle_t handle)
    {
        const std::string path = tau::assets::path_of(handle);
        const asset_guid_t guid = handle.is_valid() && tau::assets::guid_of(path) == handle.guid ? handle.guid : 0;
        return {
            {"g", tau::assets::guid_text(guid)},
            {"p", path                        },
        };
    }

    namespace
    {
        void collect_value(tau::reflection::type_t type, tau::reflection::any_t& value,
                           std::vector<asset_handle_t>& out);

        void collect_fields(tau::reflection::type_t type, tau::reflection::any_t& instance,
                            std::vector<asset_handle_t>& out)
        {
            for (auto [data_id, data] : type.data())
            {
                tau::reflection::any_t field = data.get(instance);

                const auto* prop = static_cast<const tau::reflection::editor_prop_t*>(data.custom());
                if (prop && prop->asset_type_hash != 0 && prop->is_list)
                {
                    for (const asset_handle_t& handle : tau::reflection::assets_of(field))
                    {
                        if (handle.is_valid()) { out.push_back(handle); }
                    }
                    continue;
                }
                if (prop && prop->asset_type_hash != 0)
                {
                    if (const asset_handle_t handle = tau::reflection::asset_of(field); handle.is_valid())
                    {
                        out.push_back(handle);
                    }
                    continue;
                }

                collect_value(data.type(), field, out);
            }
        }

        void collect_value(tau::reflection::type_t type, tau::reflection::any_t& value,
                           std::vector<asset_handle_t>& out)
        {
            if (type.is_sequence_container())
            {
                auto list = value.as_sequence_container();
                const tau::reflection::type_t element_type = list.value_type();
                for (tau::reflection::any_t element : list) { collect_value(element_type, element, out); }
            }
            else if (!type.is_enum() && type.data().begin() != type.data().end()) { collect_fields(type, value, out); }
        }
    } // namespace

    void collect_asset_refs(tau::world_t& world, std::vector<asset_handle_t>& out)
    {
        const std::unordered_map<u32_t, tau::reflection::type_t> by_pool = component_types_by_pool(world);

        for (auto [pool_id, pool] : world.registry.storage())
        {
            const auto type_it = by_pool.find(static_cast<u32_t>(pool_id));
            if (type_it == by_pool.end()) { continue; }

            const tau::reflection::type_t type = type_it->second;
            const tau::reflection::func_t get_func = type.func("get"_h);

            for (tau::ecs::entity_t entity : pool)
            {
                tau::reflection::any_t instance =
                    get_func.invoke({}, tau::reflection::forward_as_meta(world.registry), entity);
                if (instance) { collect_fields(type, instance, out); }
            }
        }

        for (auto [entity, instance] : world.registry.view<prefab_instance_t>().each())
        {
            if (!instance.prefab.is_valid()) { continue; }
            out.push_back(instance.prefab);
            if (const prefab_t* prefab = tau::assets::get(instance.prefab))
            {
                tau::prefab::collect_nested(prefab->data, out);
            }
        }
    }

    namespace
    {
        void unknown_fields(const tau::reflection::type_t& type, const std::vector<scene_property_t>& props,
                            const std::string& component, const std::string& prefix, std::vector<unknown_key_t>& out)
        {
            for (const scene_property_t& prop : props)
            {
                const std::string key = prefix + describe_key(prop.prop_name, prop.prop_hash);
                const tau::reflection::data_t data = type.data(prop.prop_hash);
                if (!data) { out.push_back({component, key}); }
                else if (prop.enum_name != 0 && data.type().is_enum() &&
                         data.type().data().begin() != data.type().data().end() && !data.type().data(prop.enum_name))
                {
                    out.push_back({component, key, describe_key(prop.str, prop.enum_name)});
                }
                else if (prop.type == scene_value_type_e::STRUCT)
                {
                    unknown_fields(data.type(), prop.children, component, key + ".", out);
                }
            }
        }
    } // namespace

    std::vector<unknown_key_t> unknown_keys(const tau::world_t& world, const scene_t& scene)
    {
        std::vector<unknown_key_t> out;
        for (const scene_entity_t& entity : scene.entities)
        {
            for (const scene_component_t& comp : entity.components)
            {
                const std::string component = describe_key(comp.type_name, comp.type_hash);
                const tau::reflection::type_t type = tau::reflection::resolve(*world.reflection_ctx, comp.type_hash);
                if (!type) { out.push_back({component, {}}); }
                else
                {
                    unknown_fields(type, comp.properties, component, {}, out);
                }
            }
        }

        std::sort(
            out.begin(), out.end(), [](const unknown_key_t& a, const unknown_key_t& b)
            { return std::tie(a.component, a.field, a.enum_value) < std::tie(b.component, b.field, b.enum_value); });
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
} // namespace tau::serialization
