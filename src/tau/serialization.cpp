#include "serialization.h"

#include "entt/meta/meta.hpp"
#include "entt/meta/resolve.hpp"
#include "tau/asset.h"
#include "tau/asset_meta.h"
#include "tau/asset_serde.h"
#include "tau/asset_table.h"
#include "tau/assets/scene_format.h"
#include "tau/components/transform.h"
#include "tau/ecs_fwd.h"
#include "tau/engine.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/math.h"
#include "tau/reflection.h"
#include "tau/systems/transform.h"

#include "json/json.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tau::serialization
{
    namespace
    {
        nlohmann::json tagged(scene_value_type_e type, nlohmann::json value)
        {
            return {
                {"ty", static_cast<u8_t>(type)},
                {"v",  std::move(value)       }
            };
        }

        std::string meta_key(const char* name, u32_t id) { return name ? std::string(name) : std::to_string(id); }

        bool is_numeric_key(const std::string& key)
        { return !key.empty() && key.find_first_not_of("0123456789") == std::string::npos; }

        u32_t key_to_hash(const std::string& key)
        { return is_numeric_key(key) ? static_cast<u32_t>(std::stoul(key)) : tau::hash_string(key); }

        std::string describe_key(const std::string& name, u32_t hash)
        { return name.empty() ? std::to_string(hash) : name; }

        // a scene names an asset type by its key, as it names components and fields, the type id is only its hash
        std::string asset_type_key(u64_t type_id)
        {
            const asset_type_t* type = tau::asset_table::by_type_id(type_id);
            return type != nullptr ? std::string(type->key) : std::string{};
        }

        u64_t asset_type_id(const nlohmann::json& value, const std::string& prop_key)
        {
            const nlohmann::json key = value.value("t", nlohmann::json{});
            const asset_type_t* type = key.is_string() ? tau::asset_table::by_key(key.get<std::string>()) : nullptr;
            if (type == nullptr)
            {
                TAU_LOG_WARN("SCENE", "Property '{}' names no known asset type ({})", prop_key, key.dump());
                return 0;
            }
            return type->type_id;
        }

        template <typename T>
        void write_pod(std::ofstream& out, const T& v)
        { out.write(reinterpret_cast<const char*>(&v), sizeof(T)); }

        void write_str(std::ofstream& out, const std::string& s)
        {
            u32_t len = static_cast<u32_t>(s.size());
            write_pod(out, len);
            out.write(s.data(), s.size());
        }

        // parents before children, siblings in order
        std::vector<tau::ecs::entity_t> hierarchy_order(tau::ecs::registry_t& reg)
        {
            std::vector<tau::ecs::entity_t> roots;
            for (tau::ecs::entity_t entity : reg.view<tau::ecs::entity_t>())
            {
                const transform_t* transform = reg.try_get<transform_t>(entity);
                if (!transform || transform->get_parent() == tau::ecs::NULL_ENTITY) { roots.push_back(entity); }
            }
            // views run newest first
            std::reverse(roots.begin(), roots.end());

            std::vector<tau::ecs::entity_t> order;
            std::vector<tau::ecs::entity_t> stack;

            for (tau::ecs::entity_t root : roots)
            {
                stack.push_back(root);
                while (!stack.empty())
                {
                    const tau::ecs::entity_t entity = stack.back();
                    stack.pop_back();
                    order.push_back(entity);

                    const transform_t* transform = reg.try_get<transform_t>(entity);
                    if (!transform) { continue; }

                    const std::size_t first = stack.size();
                    for (tau::ecs::entity_t child = transform->get_first_child(); child != tau::ecs::NULL_ENTITY;
                         child = reg.get<transform_t>(child).get_next_sibling())
                    {
                        stack.push_back(child);
                    }
                    std::reverse(stack.begin() + static_cast<std::ptrdiff_t>(first), stack.end());
                }
            }

            return order;
        }

        std::unordered_map<u32_t, tau::reflection::type_t> component_types_by_pool(tau::world_t& world)
        {
            std::unordered_map<u32_t, tau::reflection::type_t> by_pool;
            for (auto [meta_id, type] : tau::reflection::resolve(*world.reflection_ctx))
            {
                if (!type.func("get"_h)) { continue; } // not a component
                by_pool.emplace(static_cast<u32_t>(type.info().hash()), type);
            }
            return by_pool;
        }

        // scenes store entity references as scene indices, saving maps through slots, loading through created
        struct entity_refs_t
        {
            const std::unordered_map<tau::ecs::entity_t, std::size_t>* slots = nullptr;
            const std::vector<tau::ecs::entity_t>* created = nullptr;

            u32_t encode(tau::ecs::entity_t entity) const
            {
                const auto it = slots->find(entity);
                return it != slots->end() ? static_cast<u32_t>(it->second) : TAU_SCENE_NULL_ENTITY;
            }

            tau::ecs::entity_t decode(u32_t value) const
            { return value < created->size() ? (*created)[value] : tau::ecs::NULL_ENTITY; }
        };

        nlohmann::json encode_fields(tau::world_t& world, tau::reflection::type_t type,
                                     tau::reflection::any_t& instance, const entity_refs_t& refs);

        // null when the type can't be stored
        nlohmann::json encode_value(tau::world_t& world, tau::reflection::type_t type, tau::reflection::any_t& value,
                                    const entity_refs_t& refs, const std::string& field)
        {
            tau::reflection::ctx_t& ctx = *world.reflection_ctx;

            if (type == tau::reflection::resolve<i32>(ctx))
            {
                return tagged(scene_value_type_e::I32, value.cast<i32>());
            }
            if (type == tau::reflection::resolve<u32>(ctx))
            {
                return tagged(scene_value_type_e::U32, value.cast<u32>());
            }
            if (type == tau::reflection::resolve<f32>(ctx))
            {
                return tagged(scene_value_type_e::F32, value.cast<f32>());
            }
            if (type == tau::reflection::resolve<bool>(ctx))
            {
                return tagged(scene_value_type_e::BOOL, value.cast<bool>());
            }
            if (type == tau::reflection::resolve<std::string>(ctx))
            {
                return tagged(scene_value_type_e::STRING, value.cast<std::string>());
            }
            if (type == tau::reflection::resolve<vec3_t>(ctx))
            {
                const vec3_t vec = value.cast<vec3_t>();
                return tagged(scene_value_type_e::VEC3, {vec.x, vec.y, vec.z});
            }
            // before the enum check, entities are an enum
            if (type == tau::reflection::resolve<tau::ecs::entity_t>(ctx))
            {
                const u32_t ref = refs.encode(value.cast<tau::ecs::entity_t>());
                return tagged(scene_value_type_e::ENTITY,
                              ref == TAU_SCENE_NULL_ENTITY ? nlohmann::json(nullptr) : nlohmann::json(ref));
            }

            if (type.is_enum())
            {
                const tau::reflection::any_t as_int = std::as_const(value).allow_cast<i32>();
                if (as_int) { return tagged(scene_value_type_e::I32, as_int.cast<i32>()); }
            }
            else if (type.is_sequence_container())
            {
                auto list = value.as_sequence_container();
                const tau::reflection::type_t element_type = list.value_type();

                nlohmann::json elements = nlohmann::json::array();
                for (tau::reflection::any_t element : list)
                {
                    nlohmann::json encoded = encode_value(world, element_type, element, refs, field);
                    if (encoded.is_null()) { return {}; }
                    elements.push_back(std::move(encoded));
                }
                return tagged(scene_value_type_e::LIST, std::move(elements));
            }
            else if (type.data().begin() != type.data().end())
            {
                return tagged(scene_value_type_e::STRUCT, encode_fields(world, type, value, refs));
            }

            TAU_LOG_WARN("SCENE", "Field '{}' has unsupported type '{}' -- not saved", field,
                         type.name() ? type.name() : "?");
            return {};
        }

        nlohmann::json encode_fields(tau::world_t& world, tau::reflection::type_t type,
                                     tau::reflection::any_t& instance, const entity_refs_t& refs)
        {
            nlohmann::json fields = nlohmann::json::object();

            for (auto [data_id, data] : type.data())
            {
                tau::reflection::any_t field_value = data.get(instance);
                const std::string prop_key = meta_key(data.name(), data_id);

                tau::reflection::editor_prop_t* prop = static_cast<tau::reflection::editor_prop_t*>(data.custom());
                if (prop && prop->asset_type_hash != 0 && prop->is_list)
                {
                    const std::vector<asset_handle_t> list = field_value.cast<std::vector<asset_handle_t>>();

                    nlohmann::json entries = nlohmann::json::array();
                    for (const asset_handle_t& handle : list)
                    {
                        std::string path = tau::engine::get_asset_registry().get_path(handle);
                        std::string_view guid = tau::asset_meta::get_guid(path);
                        entries.push_back({
                            {"g", guid.empty() ? "" : std::string(guid)},
                            {"p", path                                 }
                        });
                    }

                    const std::string asset_type = asset_type_key(prop->asset_type_hash);
                    fields[prop_key] =
                        tagged(scene_value_type_e::ASSET_REF_LIST, {
                                                                       {"t", asset_type        },
                                                                       {"e", std::move(entries)}
                    });
                    continue;
                }

                if (prop && prop->asset_type_hash != 0)
                {
                    asset_handle_t handle = field_value.cast<asset_handle_t>();
                    std::string path = tau::engine::get_asset_registry().get_path(handle);
                    std::string_view guid = tau::asset_meta::get_guid(path);
                    const std::string asset_type = asset_type_key(prop->asset_type_hash);
                    fields[prop_key] =
                        tagged(scene_value_type_e::ASSET_REF,
                               {
                                   {"t", asset_type                           },
                                   {"g", guid.empty() ? "" : std::string(guid)},
                                   {"p", path                                 }
                    });
                    continue;
                }

                nlohmann::json encoded = encode_value(world, data.type(), field_value, refs, prop_key);
                if (!encoded.is_null()) { fields[prop_key] = std::move(encoded); }
            }

            return fields;
        }

        bool parse_value(const nlohmann::json& val, const std::string& prop_key, scene_property_t& prop)
        {
            if (!val.is_object() || !val.contains("ty") || !val.contains("v"))
            {
                TAU_LOG_WARN("SCENE", "Property '{}' is not a tagged value -- skipped", prop_key);
                return false;
            }

            prop.type = static_cast<scene_value_type_e>(val["ty"].get<u8_t>());
            const nlohmann::json& v = val["v"];

            switch (prop.type)
            {
            case scene_value_type_e::I32: prop.i = v.get<i32_t>(); return true;
            case scene_value_type_e::U32: prop.u = v.get<u32_t>(); return true;
            case scene_value_type_e::F32: prop.f = v.get<f32>(); return true;
            case scene_value_type_e::BOOL: prop.b = v.get<bool>(); return true;
            case scene_value_type_e::STRING: prop.str = v.get<std::string>(); return true;
            case scene_value_type_e::VEC3:
                prop.vec = vec3_t(v[0].get<f32>(), v[1].get<f32>(), v[2].get<f32>());
                return true;
            case scene_value_type_e::ASSET_REF:
                prop.asset_type = asset_type_id(v, prop_key);
                prop.str = v.value("p", std::string{});
                prop.guid = v.value("g", std::string{});
                return true;
            case scene_value_type_e::ASSET_REF_LIST:
            {
                prop.asset_type = asset_type_id(v, prop_key);
                const nlohmann::json entries = v.value("e", nlohmann::json::array());
                for (const nlohmann::json& entry : entries)
                {
                    prop.strs.push_back(entry.value("p", std::string{}));
                    prop.guids.push_back(entry.value("g", std::string{}));
                }
                return true;
            }
            case scene_value_type_e::ENTITY: prop.u = v.is_null() ? TAU_SCENE_NULL_ENTITY : v.get<u32_t>(); return true;
            case scene_value_type_e::STRUCT:
                for (const auto& [child_key, child_val] : v.items())
                {
                    scene_property_t child;
                    child.prop_hash = key_to_hash(child_key);
                    if (!is_numeric_key(child_key)) { child.prop_name = child_key; }

                    if (parse_value(child_val, child_key, child)) { prop.children.push_back(std::move(child)); }
                }
                return true;
            case scene_value_type_e::LIST:
                for (const nlohmann::json& element : v)
                {
                    scene_property_t child;
                    if (parse_value(element, prop_key, child)) { prop.children.push_back(std::move(child)); }
                }
                return true;
            }

            TAU_LOG_WARN("SCENE", "Property '{}' has an unknown value type -- skipped", prop_key);
            return false;
        }

        void write_property(std::ofstream& out, const scene_property_t& prop)
        {
            write_pod(out, prop.prop_hash);
            write_pod(out, static_cast<u8_t>(prop.type));

            switch (prop.type)
            {
            case scene_value_type_e::I32: write_pod(out, prop.i); break;
            case scene_value_type_e::U32: write_pod(out, prop.u); break;
            case scene_value_type_e::F32: write_pod(out, prop.f); break;
            case scene_value_type_e::BOOL: write_pod(out, static_cast<u8_t>(prop.b ? 1 : 0)); break;
            case scene_value_type_e::VEC3:
                write_pod(out, prop.vec.x);
                write_pod(out, prop.vec.y);
                write_pod(out, prop.vec.z);
                break;
            case scene_value_type_e::STRING: write_str(out, prop.str); break;
            case scene_value_type_e::ASSET_REF:
                write_pod(out, prop.asset_type);
                write_str(out, prop.str);
                write_str(out, prop.guid);
                break;
            case scene_value_type_e::ASSET_REF_LIST:
                write_pod(out, prop.asset_type);
                write_pod(out, static_cast<u32_t>(prop.strs.size()));
                for (std::size_t i = 0; i < prop.strs.size(); i++)
                {
                    write_str(out, prop.strs[i]);
                    write_str(out, i < prop.guids.size() ? prop.guids[i] : std::string{});
                }
                break;
            case scene_value_type_e::ENTITY: write_pod(out, prop.u); break;
            case scene_value_type_e::STRUCT:
            case scene_value_type_e::LIST:
                write_pod(out, static_cast<u32_t>(prop.children.size()));
                for (const scene_property_t& child : prop.children) { write_property(out, child); }
                break;
            }
        }
    } // namespace

    nlohmann::json serialize_scene(tau::world_t& world, std::vector<tau::ecs::entity_t>* order)
    {
        nlohmann::json scene_data;
        scene_data["entities"] = nlohmann::json::array();

        const std::unordered_map<u32_t, tau::reflection::type_t> by_pool = component_types_by_pool(world);

        const std::vector<tau::ecs::entity_t> entities = hierarchy_order(world.registry);
        if (order) { *order = entities; }

        std::unordered_map<tau::ecs::entity_t, std::size_t> slots;
        for (tau::ecs::entity_t entity : entities)
        {
            nlohmann::json entity_json;

            const transform_t* transform = world.registry.try_get<transform_t>(entity);
            if (transform && transform->get_parent() != tau::ecs::NULL_ENTITY)
            {
                entity_json["parent"] = slots.at(transform->get_parent());
            }

            entity_json["components"] = nlohmann::json::object();

            slots.emplace(entity, scene_data["entities"].size());
            scene_data["entities"].push_back(std::move(entity_json));
        }

        const entity_refs_t refs{.slots = &slots};

        for (auto [pool_id, pool] : world.registry.storage())
        {
            const auto type_it = by_pool.find(static_cast<u32_t>(pool_id));
            if (type_it == by_pool.end()) { continue; }

            const tau::reflection::type_t type = type_it->second;
            const tau::reflection::func_t get_func = type.func("get"_h);
            const std::string type_key = meta_key(type.name(), static_cast<u32_t>(type.id()));

            for (tau::ecs::entity_t entity : pool)
            {
                const auto slot_it = slots.find(entity);
                if (slot_it == slots.end()) { continue; }

                tau::reflection::any_t instance =
                    get_func.invoke({}, tau::reflection::forward_as_meta(world.registry), entity);
                if (!instance) { continue; }

                scene_data["entities"][slot_it->second]["components"][type_key] =
                    encode_fields(world, type, instance, refs);
            }
        }

        return scene_data;
    }

    scene_t scene_from_json(const nlohmann::json& scene_data)
    {
        scene_t scene;

        if (!scene_data.contains("entities")) { return scene; }

        for (const nlohmann::json& entity_json : scene_data["entities"])
        {
            scene_entity_t& out_entity = scene.entities.emplace_back();
            out_entity.parent = entity_json.value("parent", -1);

            for (const auto& [type_key, props_json] : entity_json["components"].items())
            {
                scene_component_t& out_comp = out_entity.components.emplace_back();
                out_comp.type_hash = key_to_hash(type_key);
                if (!is_numeric_key(type_key)) { out_comp.type_name = type_key; }

                for (const auto& [prop_key, val] : props_json.items())
                {
                    scene_property_t prop;
                    prop.prop_hash = key_to_hash(prop_key);
                    if (!is_numeric_key(prop_key)) { prop.prop_name = prop_key; }

                    if (parse_value(val, prop_key, prop)) { out_comp.properties.push_back(std::move(prop)); }
                }
            }
        }

        return scene;
    }

    void write_scene_binary(const scene_t& scene, const std::string& output_path)
    {
        std::ofstream out(output_path, std::ios::binary);
        if (!out.is_open())
        {
            TAU_LOG_ERROR("SCENE", "Failed to open output for scene: {}", output_path);
            return;
        }

        scene_header_t header;
        header.entity_count = static_cast<u32_t>(scene.entities.size());
        write_pod(out, header);

        for (const scene_entity_t& entity : scene.entities)
        {
            write_pod(out, entity.parent);
            write_pod(out, static_cast<u32_t>(entity.components.size()));
            for (const scene_component_t& comp : entity.components)
            {
                write_pod(out, comp.type_hash);
                write_pod(out, static_cast<u32_t>(comp.properties.size()));
                for (const scene_property_t& prop : comp.properties) { write_property(out, prop); }
            }
        }
    }

    namespace
    {
        void apply_fields(tau::world_t& world, tau::reflection::any_t& object, tau::reflection::type_t type,
                          const std::vector<scene_property_t>& props, const entity_refs_t& refs,
                          const std::string& owner);

        // built on `current` so fields missing from the file keep their value, empty when nothing applies
        tau::reflection::any_t decode_value(tau::world_t& world, const scene_property_t& prop,
                                            tau::reflection::type_t type, tau::reflection::any_t current,
                                            const entity_refs_t& refs, const std::string& owner)
        {
            tau::reflection::ctx_t& ctx = *world.reflection_ctx;

            switch (prop.type)
            {
            case scene_value_type_e::I32: return tau::reflection::any_t{ctx, prop.i};
            case scene_value_type_e::U32: return tau::reflection::any_t{ctx, prop.u};
            case scene_value_type_e::F32: return tau::reflection::any_t{ctx, prop.f};
            case scene_value_type_e::BOOL: return tau::reflection::any_t{ctx, prop.b};
            case scene_value_type_e::STRING: return tau::reflection::any_t{ctx, prop.str};
            case scene_value_type_e::VEC3: return tau::reflection::any_t{ctx, prop.vec};
            case scene_value_type_e::ENTITY: return tau::reflection::any_t{ctx, refs.decode(prop.u)};
            case scene_value_type_e::ASSET_REF:
            {
                const std::string ref = tau::asset_meta::resolve_ref(prop.guid, prop.str);
                if (ref.empty()) { return {}; }
                return tau::reflection::any_t{
                    ctx, tau::asset_serde::load(prop.asset_type, tau::engine::get_asset_registry(), ref)};
            }
            case scene_value_type_e::ASSET_REF_LIST:
            {
                std::vector<asset_handle_t> list;
                list.reserve(prop.strs.size());
                for (std::size_t i = 0; i < prop.strs.size(); i++)
                {
                    const std::string ref = tau::asset_meta::resolve_ref(
                        i < prop.guids.size() ? prop.guids[i] : std::string{}, prop.strs[i]);
                    list.push_back(
                        ref.empty() ? asset_handle_t{}
                                    : tau::asset_serde::load(prop.asset_type, tau::engine::get_asset_registry(), ref));
                }
                return tau::reflection::any_t{ctx, std::move(list)};
            }
            case scene_value_type_e::STRUCT:
            {
                if (!current) { current = type.construct(); }
                if (!current) { return {}; }

                apply_fields(world, current, type, prop.children, refs, owner);
                return current;
            }
            case scene_value_type_e::LIST:
            {
                if (!current) { current = type.construct(); }
                if (!current || !type.is_sequence_container()) { return {}; }

                auto list = current.as_sequence_container();
                const tau::reflection::type_t element_type = list.value_type();

                list.clear();
                for (const scene_property_t& child : prop.children)
                {
                    tau::reflection::any_t element = decode_value(world, child, element_type, {}, refs, owner);
                    if (!element || !element.allow_cast(element_type))
                    {
                        TAU_LOG_WARN("SCENE", "List element on '{}' does not fit the list, dropped", owner);
                        continue;
                    }
                    list.insert(list.end(), element);
                }
                return current;
            }
            }

            return {};
        }

        void apply_fields(tau::world_t& world, tau::reflection::any_t& object, tau::reflection::type_t type,
                          const std::vector<scene_property_t>& props, const entity_refs_t& refs,
                          const std::string& owner)
        {
            for (const scene_property_t& prop : props)
            {
                tau::reflection::data_t data = type.data(prop.prop_hash);
                if (!data)
                {
                    TAU_LOG_WARN("SCENE", "Unknown field '{}' on '{}', value dropped",
                                 describe_key(prop.prop_name, prop.prop_hash), owner);
                    continue;
                }

                tau::reflection::any_t value = decode_value(world, prop, data.type(), data.get(object), refs, owner);
                if (value && !data.set(object, value))
                {
                    TAU_LOG_WARN("SCENE", "Field '{}' on '{}' does not take the saved value, value dropped",
                                 describe_key(prop.prop_name, prop.prop_hash), owner);
                }
            }
        }

        void apply_scene_entity(tau::world_t& world, tau::ecs::entity_t entity, const scene_entity_t& scene_entity,
                                const entity_refs_t& refs)
        {
            for (const scene_component_t& comp : scene_entity.components)
            {
                tau::reflection::type_t type = tau::reflection::resolve(*world.reflection_ctx, comp.type_hash);

                if (!type)
                {
                    TAU_LOG_WARN("SCENE", "Unknown component '{}', dropped from entity",
                                 describe_key(comp.type_name, comp.type_hash));
                    continue;
                }

                if (tau::reflection::func_t add_func = type.func("add"_h); add_func)
                {
                    add_func.invoke({}, tau::reflection::forward_as_meta(world.registry), entity);
                }

                tau::reflection::func_t get_func = type.func("get"_h);
                if (!get_func) { continue; }

                tau::reflection::any_t instance =
                    get_func.invoke({}, tau::reflection::forward_as_meta(world.registry), entity);
                if (!instance) { continue; }

                apply_fields(world, instance, type, comp.properties, refs,
                             describe_key(comp.type_name, comp.type_hash));

                if (tau::reflection::func_t on_changed = type.func("on_changed"_h); on_changed)
                {
                    on_changed.invoke({}, tau::reflection::forward_as_meta(world.registry), entity);
                }
            }
        }
    } // namespace

    std::vector<tau::ecs::entity_t> instantiate_scene(tau::world_t& world, const scene_t& scene)
    {
        std::vector<tau::ecs::entity_t> created;
        created.reserve(scene.entities.size());

        for (std::size_t i = 0; i < scene.entities.size(); i++) { created.push_back(world.registry.create()); }

        // after every entity exists, so references to later ones resolve
        const entity_refs_t refs{.created = &created};
        for (std::size_t i = 0; i < scene.entities.size(); i++)
        {
            apply_scene_entity(world, created[i], scene.entities[i], refs);
        }

        // reversed since set_parent prepends, keeps the saved sibling order
        for (std::size_t i = created.size(); i-- > 0;)
        {
            const i32_t parent = scene.entities[i].parent;
            if (parent < 0) { continue; }

            if (static_cast<std::size_t>(parent) >= created.size() || !world.registry.all_of<transform_t>(created[i]) ||
                !world.registry.all_of<transform_t>(created[static_cast<std::size_t>(parent)]))
            {
                TAU_LOG_WARN("SCENE", "Entity {} has an invalid parent {}, left at the root", i, parent);
                continue;
            }

            tau::transform_system::set_parent(world.registry, created[i], created[static_cast<std::size_t>(parent)]);
        }

        return created;
    }

    std::vector<tau::ecs::entity_t> deserialize_scene(tau::world_t& world, const nlohmann::json& scene_data)
    { return instantiate_scene(world, scene_from_json(scene_data)); }

    void clear_scene(tau::world_t& world)
    {
        auto view = world.registry.view<tau::ecs::entity_t>();

        std::vector<tau::ecs::entity_t> to_destroy;
        to_destroy.insert(to_destroy.end(), view.begin(), view.end());

        world.registry.destroy(to_destroy.begin(), to_destroy.end());
    }
} // namespace tau::serialization
