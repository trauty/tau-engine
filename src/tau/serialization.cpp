#include "serialization.h"

#include "entt/meta/meta.hpp"
#include "entt/meta/resolve.hpp"
#include "tau/asset.h"
#include "tau/assets/prefab.h"
#include "tau/assets/scene_format.h"
#include "tau/components/prefab.h"
#include "tau/components/transform.h"
#include "tau/components/unresolved.h"
#include "tau/ecs_fwd.h"
#include "tau/engine.h"
#include "tau/guid.h"
#include "tau/guid_json.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/math.h"
#include "tau/prefab.h"
#include "tau/reflection.h"
#include "tau/systems/guid.h"
#include "tau/systems/transform.h"

#include "json/json.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

        template <typename T>
        nlohmann::json scalar_json(const u8_t* at)
        {
            T value;
            std::memcpy(&value, at, sizeof(T));
            return value;
        }

        nlohmann::json pod_to_json(pod_layout_t layout, const u8_t* bytes)
        {
            auto one = [&](const u8_t* at) -> nlohmann::json
            {
                switch (layout.scalar)
                {
                case scalar_e::NONE: break;
                case scalar_e::BOOL: return *at != 0;
                case scalar_e::I32: return scalar_json<i32_t>(at);
                case scalar_e::U32: return scalar_json<u32_t>(at);
                case scalar_e::I64: return scalar_json<i64_t>(at);
                case scalar_e::U64: return scalar_json<u64_t>(at);
                case scalar_e::F32: return scalar_json<f32>(at);
                case scalar_e::F64: return scalar_json<f64>(at);
                }
                return nullptr;
            };

            if (layout.count == 1) { return one(bytes); }

            nlohmann::json values = nlohmann::json::array();
            for (u32_t i = 0; i < layout.count; i++) { values.push_back(one(bytes + i * scalar_size(layout.scalar))); }
            return values;
        }

        template <typename T>
        bool scalar_from_json(const nlohmann::json& value, u8_t* at)
        {
            if (!value.is_number()) { return false; }
            const T scalar = value.get<T>();
            std::memcpy(at, &scalar, sizeof(T));
            return true;
        }

        bool pod_from_json(pod_layout_t layout, const nlohmann::json& value, u8_t* out)
        {
            auto one = [&](const nlohmann::json& v, u8_t* at)
            {
                switch (layout.scalar)
                {
                case scalar_e::NONE: break;
                case scalar_e::BOOL:
                    if (!v.is_boolean() && !v.is_number()) { return false; }
                    *at = (v.is_boolean() ? v.get<bool>() : v.get<f64>() != 0.0) ? 1 : 0;
                    return true;
                case scalar_e::I32: return scalar_from_json<i32_t>(v, at);
                case scalar_e::U32: return scalar_from_json<u32_t>(v, at);
                case scalar_e::I64: return scalar_from_json<i64_t>(v, at);
                case scalar_e::U64: return scalar_from_json<u64_t>(v, at);
                case scalar_e::F32: return scalar_from_json<f32>(v, at);
                case scalar_e::F64: return scalar_from_json<f64>(v, at);
                }
                return false;
            };

            if (layout.count == 1) { return one(value, out); }
            if (!value.is_array() || value.size() != layout.count) { return false; }

            for (u32_t i = 0; i < layout.count; i++)
            {
                if (!one(value[i], out + i * scalar_size(layout.scalar))) { return false; }
            }
            return true;
        }

        template <typename T>
        tau::reflection::any_t scalar_any(tau::reflection::ctx_t& ctx, const u8_t* at)
        {
            T value;
            std::memcpy(&value, at, sizeof(T));
            return tau::reflection::any_t{ctx, value};
        }

        tau::reflection::any_t scalar_any(tau::reflection::ctx_t& ctx, scalar_e scalar, const u8_t* at)
        {
            switch (scalar)
            {
            case scalar_e::NONE: break;
            case scalar_e::BOOL: return tau::reflection::any_t{ctx, *at != 0};
            case scalar_e::I32: return scalar_any<i32_t>(ctx, at);
            case scalar_e::U32: return scalar_any<u32_t>(ctx, at);
            case scalar_e::I64: return scalar_any<i64_t>(ctx, at);
            case scalar_e::U64: return scalar_any<u64_t>(ctx, at);
            case scalar_e::F32: return scalar_any<f32>(ctx, at);
            case scalar_e::F64: return scalar_any<f64>(ctx, at);
            }
            return {};
        }

        bool is_numeric_key(const std::string& key)
        { return !key.empty() && key.find_first_not_of("0123456789") == std::string::npos; }

        u32_t key_to_hash(const std::string& key)
        { return is_numeric_key(key) ? static_cast<u32_t>(std::stoul(key)) : tau::hash_string(key); }

        std::string describe_key(const std::string& name, u32_t hash)
        { return name.empty() ? std::to_string(hash) : name; }

        // a scene names an asset type by its key, as it names components and fields, the type id is only its hash
        std::string asset_type_key(u64_t type_id)
        {
            const asset_type_t* type = tau::assets::type_by_id(type_id);
            return type != nullptr ? std::string(type->key) : std::string{};
        }

        u64_t asset_type_id(const nlohmann::json& value, const std::string& prop_key)
        {
            const nlohmann::json key = value.value("t", nlohmann::json{});
            const asset_type_t* type = key.is_string() ? tau::assets::type_by_key(key.get<std::string>()) : nullptr;
            if (type == nullptr)
            {
                TAU_LOG_WARN("SCENE", "Property '{}' names no known asset type ({})", prop_key, key.dump());
                return 0;
            }
            return type->id;
        }

        asset_guid_t ref_guid(const nlohmann::json& ref)
        {
            const nlohmann::json guid = ref.value("g", nlohmann::json{});
            return guid.is_string() ? tau::assets::parse_guid(guid.get<std::string>()) : 0;
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
            for (tau::ecs::entity_t root = tau::transform_system::first_root(reg); root != tau::ecs::NULL_ENTITY;
                 root = reg.get<transform_t>(root).get_next_sibling())
            {
                roots.push_back(root);
            }

            // no transform: after the rest, oldest first
            const std::size_t ordered = roots.size();
            for (tau::ecs::entity_t entity : reg.view<tau::ecs::entity_t>())
            {
                if (!reg.all_of<transform_t>(entity)) { roots.push_back(entity); }
            }
            std::reverse(roots.begin() + static_cast<std::ptrdiff_t>(ordered), roots.end());

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

        struct entity_refs_t
        {
            const tau::ecs::registry_t* reg = nullptr;
            const std::unordered_map<u64_t, tau::ecs::entity_t>* by_file_id = nullptr;
            bool search_world = false;

            u64_t encode(tau::ecs::entity_t entity) const { return tau::guid_system::of(*reg, entity); }

            tau::ecs::entity_t decode(u64_t file_id) const
            {
                const auto it = by_file_id->find(file_id);
                if (it != by_file_id->end()) { return it->second; }
                return search_world ? tau::guid_system::find(*reg, file_id) : tau::ecs::NULL_ENTITY;
            }
        };

        nlohmann::json encode_fields(tau::world_t& world, tau::reflection::type_t type,
                                     tau::reflection::any_t& instance, const entity_refs_t& refs);

        // null when the type can't be stored
        nlohmann::json encode_value(tau::world_t& world, tau::reflection::type_t type, tau::reflection::any_t& value,
                                    const entity_refs_t& refs, const std::string& field)
        {
            tau::reflection::ctx_t& ctx = *world.reflection_ctx;

            if (const std::optional<scene_value_type_e> tag = tau::reflection::pod_tag(type))
            {
                const u8_t* bytes = static_cast<const u8_t*>(tau::reflection::value_data(value));
                if (!bytes) { return {}; }
                return tagged(*tag, pod_to_json(pod_layout(*tag), bytes));
            }
            if (type == tau::reflection::resolve<std::string>(ctx))
            {
                return tagged(scene_value_type_e::STRING, value.cast<std::string>());
            }
            // before the enum check, entities are an enum
            if (type == tau::reflection::resolve<tau::ecs::entity_t>(ctx))
            {
                return tagged(scene_value_type_e::ENTITY, guid::to_json(refs.encode(value.cast<tau::ecs::entity_t>())));
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
                    nlohmann::json entries = nlohmann::json::array();
                    for (const asset_handle_t& handle : tau::reflection::assets_of(field_value))
                    {
                        entries.push_back(asset_ref(handle));
                    }

                    fields[prop_key] =
                        tagged(scene_value_type_e::ASSET_REF_LIST,
                               {
                                   {"t", asset_type_key(prop->asset_type_hash)},
                                   {"e", std::move(entries)                   }
                    });
                    continue;
                }

                if (prop && prop->asset_type_hash != 0)
                {
                    nlohmann::json ref = asset_ref(tau::reflection::asset_of(field_value));
                    ref["t"] = asset_type_key(prop->asset_type_hash);
                    fields[prop_key] = tagged(scene_value_type_e::ASSET_REF, std::move(ref));
                    continue;
                }

                nlohmann::json encoded = encode_value(world, data.type(), field_value, refs, prop_key);
                if (!encoded.is_null()) { fields[prop_key] = std::move(encoded); }
            }

            return fields;
        }

        nlohmann::json encode_components(tau::world_t& world, tau::ecs::entity_t entity, const entity_refs_t& refs)
        {
            const std::unordered_map<u32_t, tau::reflection::type_t> by_pool = component_types_by_pool(world);
            nlohmann::json components = nlohmann::json::object();

            for (auto [pool_id, pool] : world.registry.storage())
            {
                if (!pool.contains(entity)) { continue; }

                const auto type_it = by_pool.find(static_cast<u32_t>(pool_id));
                if (type_it == by_pool.end()) { continue; }

                const tau::reflection::type_t type = type_it->second;
                tau::reflection::any_t instance =
                    type.func("get"_h).invoke({}, tau::reflection::forward_as_meta(world.registry), entity);
                if (!instance) { continue; }

                components[meta_key(type.name(), static_cast<u32_t>(type.id()))] =
                    encode_fields(world, type, instance, refs);
            }

            return components;
        }

        const nlohmann::json* prefab_data_of(const tau::ecs::registry_t& reg, tau::ecs::entity_t root,
                                             const tau::prefab::lookup_t& lookup)
        {
            const prefab_instance_t* instance = reg.try_get<prefab_instance_t>(root);
            return instance ? tau::prefab::lookup_asset(lookup, instance->prefab) : nullptr;
        }

        bool parse_value(const nlohmann::json& val, const std::string& prop_key, scene_property_t& prop)
        {
            if (!val.is_object() || !val.contains("ty") || !val.contains("v"))
            {
                TAU_LOG_WARN("SCENE", "Property '{}' is not a tagged value -- skipped", prop_key);
                return false;
            }

            if (!val["ty"].is_number_integer() || val["ty"].get<i64_t>() < 0 || val["ty"].get<i64_t>() > 255)
            {
                TAU_LOG_WARN("SCENE", "Property '{}' has an unknown value type -- skipped", prop_key);
                return false;
            }
            prop.type = static_cast<scene_value_type_e>(val["ty"].get<u8_t>());
            const nlohmann::json& v = val["v"];

            if (const pod_layout_t layout = pod_layout(prop.type); layout.count > 0)
            {
                if (pod_from_json(layout, v, prop.pod.data())) { return true; }
                TAU_LOG_WARN("SCENE", "Property '{}' does not hold the value its type names -- skipped", prop_key);
                return false;
            }

            switch (prop.type)
            {
            case scene_value_type_e::STRING:
                if (!v.is_string()) { break; }
                prop.str = v.get<std::string>();
                return true;
            case scene_value_type_e::ASSET_REF:
                prop.asset_type = asset_type_id(v, prop_key);
                prop.str = v.value("p", std::string{});
                prop.guid = ref_guid(v);
                return true;
            case scene_value_type_e::ASSET_REF_LIST:
            {
                prop.asset_type = asset_type_id(v, prop_key);
                const nlohmann::json entries = v.value("e", nlohmann::json::array());
                for (const nlohmann::json& entry : entries)
                {
                    prop.strs.push_back(entry.value("p", std::string{}));
                    prop.guids.push_back(ref_guid(entry));
                }
                return true;
            }
            case scene_value_type_e::ENTITY:
                if (v.is_number_integer() && v.get<i64_t>() >= 0)
                {
                    prop.ref = v.get<u64_t>() + 1;
                    prop.ref_is_index = true;
                }
                else
                {
                    prop.ref = guid::from_json(v);
                }
                return true;
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
            default: break;
            }

            TAU_LOG_WARN("SCENE", "Property '{}' has an unknown value type -- skipped", prop_key);
            return false;
        }

        void adopt_index_refs(std::vector<scene_property_t>& props, bool generated_ids)
        {
            for (scene_property_t& prop : props)
            {
                if (prop.type == scene_value_type_e::ENTITY && prop.ref_is_index)
                {
                    if (!generated_ids) { prop.ref = 0; }
                    prop.ref_is_index = false;
                }
                adopt_index_refs(prop.children, generated_ids);
            }
        }

        void write_property(std::ofstream& out, const scene_property_t& prop)
        {
            write_pod(out, prop.prop_hash);
            write_pod(out, static_cast<u8_t>(prop.type));

            if (const u32_t size = pod_size(pod_layout(prop.type)); size > 0)
            {
                out.write(reinterpret_cast<const char*>(prop.pod.data()), size);
                return;
            }

            switch (prop.type)
            {
            case scene_value_type_e::STRING: write_str(out, prop.str); break;
            case scene_value_type_e::ASSET_REF:
                write_pod(out, prop.asset_type);
                write_str(out, prop.str);
                write_pod(out, prop.guid);
                break;
            case scene_value_type_e::ASSET_REF_LIST:
                write_pod(out, prop.asset_type);
                write_pod(out, static_cast<u32_t>(prop.strs.size()));
                for (std::size_t i = 0; i < prop.strs.size(); i++)
                {
                    write_str(out, prop.strs[i]);
                    write_pod(out, i < prop.guids.size() ? prop.guids[i] : asset_guid_t{0});
                }
                break;
            case scene_value_type_e::ENTITY: write_pod(out, prop.ref); break;
            case scene_value_type_e::STRUCT:
            case scene_value_type_e::LIST:
                write_pod(out, static_cast<u32_t>(prop.children.size()));
                for (const scene_property_t& child : prop.children) { write_property(out, child); }
                break;
            default: break;
            }
        }
    } // namespace

    namespace
    {
        void add_unresolved(const tau::ecs::registry_t& reg, tau::ecs::entity_t entity, nlohmann::json& components);

        nlohmann::json collapse_instance(tau::world_t& world, tau::ecs::entity_t root,
                                         const tau::prefab::lookup_t& lookup)
        {
            const u64_t instance = tau::guid_system::of(world.registry, root);
            std::vector<nlohmann::json> parts;
            for (auto [entity, part] : world.registry.view<prefab_part_t>().each())
            {
                if (part.instance == instance) { parts.push_back(serialize_entity(world, entity)); }
            }
            return tau::prefab::collapse_instance(prefab_data_of(world.registry, root, lookup), parts);
        }
    } // namespace

    nlohmann::json serialize_scene(tau::world_t& world)
    {
        nlohmann::json scene_data;
        scene_data["entities"] = nlohmann::json::array();

        const std::unordered_map<u32_t, tau::reflection::type_t> by_pool = component_types_by_pool(world);

        const std::vector<tau::ecs::entity_t> entities = hierarchy_order(world.registry);
        for (tau::ecs::entity_t entity : entities) { tau::guid_system::ensure(world.registry, entity); }

        const tau::prefab::lookup_t lookup = tau::prefab::asset_lookup();
        std::unordered_map<tau::ecs::entity_t, std::size_t> slots;
        for (tau::ecs::entity_t entity : entities)
        {
            // instance = one entry at its root
            if (const prefab_part_t* part = world.registry.try_get<prefab_part_t>(entity))
            {
                const tau::ecs::entity_t root = tau::guid_system::find(world.registry, part->instance);
                if (root != tau::ecs::NULL_ENTITY && world.registry.all_of<prefab_instance_t>(root))
                {
                    if (root == entity) { scene_data["entities"].push_back(collapse_instance(world, root, lookup)); }
                    continue;
                }
            }

            nlohmann::json entity_json;
            entity_json["id"] = guid::to_json(tau::guid_system::of(world.registry, entity));

            const transform_t* transform = world.registry.try_get<transform_t>(entity);
            if (transform && transform->get_parent() != tau::ecs::NULL_ENTITY)
            {
                entity_json["parent"] = guid::to_json(tau::guid_system::of(world.registry, transform->get_parent()));
            }

            entity_json["components"] = nlohmann::json::object();

            slots.emplace(entity, scene_data["entities"].size());
            scene_data["entities"].push_back(std::move(entity_json));
        }

        const entity_refs_t refs{.reg = &world.registry};

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

        for (const auto& [entity, slot] : slots)
        {
            add_unresolved(world.registry, entity, scene_data["entities"][slot]["components"]);
        }

        return scene_data;
    }

    namespace
    {
        scene_entity_t parse_entity(const nlohmann::json& entity_json)
        {
            scene_entity_t out_entity;
            out_entity.id = guid::from_json(entity_json.value("id", nlohmann::json{}));

            const nlohmann::json parent = entity_json.value("parent", nlohmann::json{});
            out_entity.parent = (parent.is_number_integer() && parent.get<i64_t>() >= 0) ? parent.get<u64_t>() + 1
                                                                                         : guid::from_json(parent);

            const nlohmann::json components = entity_json.value("components", nlohmann::json::object());
            for (const auto& [type_key, props_json] : components.items())
            {
                scene_component_t& out_comp = out_entity.components.emplace_back();
                out_comp.type_hash = key_to_hash(type_key);
                if (!is_numeric_key(type_key)) { out_comp.type_name = type_key; }
                out_comp.source_json = props_json.dump();

                for (const auto& [prop_key, val] : props_json.items())
                {
                    scene_property_t prop;
                    prop.prop_hash = key_to_hash(prop_key);
                    if (!is_numeric_key(prop_key)) { prop.prop_name = prop_key; }

                    if (parse_value(val, prop_key, prop)) { out_comp.properties.push_back(std::move(prop)); }
                }
            }

            if (const auto part = entity_json.find("prefab_part"); part != entity_json.end() && part->is_object())
            {
                out_entity.prefab_instance = guid::from_json(part->value("instance", nlohmann::json{}));
                out_entity.prefab_source = guid::from_json(part->value("source", nlohmann::json{}));
            }
            if (const auto ref = entity_json.find("prefab"); ref != entity_json.end() && ref->is_object())
            {
                out_entity.prefab_root = true;
                out_entity.prefab_guid = ref_guid(*ref);
                out_entity.prefab_path = ref->value("p", std::string{});
                for (const nlohmann::json& source : entity_json.value("removed", nlohmann::json::array()))
                {
                    out_entity.prefab_removed.push_back(guid::from_json(source));
                }
                if (entity_json.contains("unresolved"))
                {
                    out_entity.prefab_unresolved = entity_json["unresolved"].dump();
                }
            }

            return out_entity;
        }
    } // namespace

    nlohmann::json serialize_entity(tau::world_t& world, tau::ecs::entity_t entity)
    {
        const entity_refs_t refs{.reg = &world.registry};

        nlohmann::json entity_json;
        entity_json["id"] = guid::to_json(tau::guid_system::ensure(world.registry, entity));

        if (const transform_t* transform = world.registry.try_get<transform_t>(entity))
        {
            entity_json["parent"] = guid::to_json(refs.encode(transform->get_parent()));
            entity_json["next"] = guid::to_json(refs.encode(transform->get_next_sibling()));
        }

        entity_json["components"] = encode_components(world, entity, refs);
        add_unresolved(world.registry, entity, entity_json["components"]);

        if (const prefab_part_t* part = world.registry.try_get<prefab_part_t>(entity))
        {
            entity_json["prefab_part"] = {
                {"instance", guid::to_json(part->instance)},
                {"source",   guid::to_json(part->source)  }
            };
        }
        if (const prefab_instance_t* instance = world.registry.try_get<prefab_instance_t>(entity))
        {
            if (!instance->unresolved.empty())
            {
                entity_json["unresolved"] = nlohmann::json::parse(instance->unresolved, nullptr, false);
                entity_json["prefab"] = entity_json["unresolved"].value("prefab", nlohmann::json::object());
            }
            else
            {
                entity_json["prefab"] = asset_ref(instance->prefab);
            }

            nlohmann::json removed = nlohmann::json::array();
            for (const u64_t source : instance->removed) { removed.push_back(guid::to_json(source)); }
            entity_json["removed"] = std::move(removed);
        }
        return entity_json;
    }

    scene_t scene_from_json(const nlohmann::json& scene_data)
    {
        scene_t scene;

        if (!scene_data.contains("entities")) { return scene; }

        for (const nlohmann::json& entity_json : scene_data["entities"])
        {
            scene.entities.push_back(parse_entity(entity_json));
        }

        const bool has_ids = std::any_of(scene.entities.begin(), scene.entities.end(),
                                         [](const scene_entity_t& entity) { return entity.id != 0; });
        scene.generated_ids = !has_ids && !scene.entities.empty();
        for (std::size_t i = 0; i < scene.entities.size(); i++)
        {
            if (scene.generated_ids) { scene.entities[i].id = i + 1; }
            for (scene_component_t& comp : scene.entities[i].components)
            {
                adopt_index_refs(comp.properties, scene.generated_ids);
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
            write_pod(out, entity.id);
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
                          const std::string& owner, std::vector<std::string>* unknown = nullptr);

        // built on `current` so fields missing from the file keep their value, empty when nothing applies
        tau::reflection::any_t decode_value(tau::world_t& world, const scene_property_t& prop,
                                            tau::reflection::type_t type, tau::reflection::any_t current,
                                            const entity_refs_t& refs, const std::string& owner)
        {
            tau::reflection::ctx_t& ctx = *world.reflection_ctx;

            if (const pod_layout_t layout = pod_layout(prop.type); layout.count > 0)
            {
                if (tau::reflection::pod_tag(type) == prop.type)
                {
                    if (!current) { current = type.construct(); }
                    void* target = current ? tau::reflection::mutable_value_data(current) : nullptr;
                    if (!target) { return {}; }
                    std::memcpy(target, prop.pod.data(), pod_size(layout));
                    return current;
                }
                // other scalars and enums: converted on set
                return layout.count == 1 ? scalar_any(ctx, layout.scalar, prop.pod.data()) : tau::reflection::any_t{};
            }

            switch (prop.type)
            {
            case scene_value_type_e::STRING: return tau::reflection::any_t{ctx, prop.str};
            case scene_value_type_e::ENTITY: return tau::reflection::any_t{ctx, refs.decode(prop.ref)};
            case scene_value_type_e::ASSET_REF:
            {
                const std::string ref = tau::assets::resolve(prop.guid, prop.str);
                if (ref.empty()) { return tau::reflection::any_t{ctx, asset_handle_t{}}; }
                return tau::reflection::any_t{ctx, tau::assets::load(prop.asset_type, ref)};
            }
            case scene_value_type_e::ASSET_REF_LIST:
            {
                std::vector<asset_handle_t> list;
                list.reserve(prop.strs.size());
                for (std::size_t i = 0; i < prop.strs.size(); i++)
                {
                    const std::string ref =
                        tau::assets::resolve(i < prop.guids.size() ? prop.guids[i] : asset_guid_t{0}, prop.strs[i]);
                    list.push_back(ref.empty() ? asset_handle_t{} : tau::assets::load(prop.asset_type, ref));
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
            default: break;
            }

            return {};
        }

        bool first_warning(const std::string& key)
        {
            static std::unordered_set<std::string> warned;
            return warned.insert(key).second;
        }

        void apply_fields(tau::world_t& world, tau::reflection::any_t& object, tau::reflection::type_t type,
                          const std::vector<scene_property_t>& props, const entity_refs_t& refs,
                          const std::string& owner, std::vector<std::string>* unknown)
        {
            for (const scene_property_t& prop : props)
            {
                tau::reflection::data_t data = type.data(prop.prop_hash);
                if (!data)
                {
                    const std::string key = describe_key(prop.prop_name, prop.prop_hash);
                    if (unknown) { unknown->push_back(key); }
                    if (first_warning(owner + "." + key))
                    {
                        TAU_LOG_WARN("SCENE", "Unknown field '{}' on '{}', {}", key, owner,
                                     unknown ? "kept as saved" : "value dropped");
                    }
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
            nlohmann::json unresolved = nlohmann::json::object();
            nlohmann::json unknown_fields = nlohmann::json::object();
            for (const scene_component_t& comp : scene_entity.components)
            {
                tau::reflection::type_t type = tau::reflection::resolve(*world.reflection_ctx, comp.type_hash);

                if (!type)
                {
                    const std::string key = describe_key(comp.type_name, comp.type_hash);
                    if (comp.source_json.empty())
                    {
                        TAU_LOG_WARN("SCENE", "Unknown component '{}', dropped from entity", key);
                        continue;
                    }

                    if (first_warning(key))
                    {
                        TAU_LOG_WARN("SCENE", "Unknown component '{}', kept as saved until its type is registered",
                                     key);
                    }
                    unresolved[key] = nlohmann::json::parse(comp.source_json, nullptr, false);
                    continue;
                }

                tau::reflection::func_t get_func = type.func("get"_h);
                if (!get_func) { continue; }

                tau::reflection::any_t instance =
                    get_func.invoke({}, tau::reflection::forward_as_meta(world.registry), entity);

                // only if missing: replacing resets e.g. transform links
                if (!instance)
                {
                    if (tau::reflection::func_t add_func = type.func("add"_h); add_func)
                    {
                        add_func.invoke({}, tau::reflection::forward_as_meta(world.registry), entity);
                    }
                    instance = get_func.invoke({}, tau::reflection::forward_as_meta(world.registry), entity);
                }
                if (!instance) { continue; }

                std::vector<std::string> unknown;
                apply_fields(world, instance, type, comp.properties, refs, describe_key(comp.type_name, comp.type_hash),
                             comp.source_json.empty() ? nullptr : &unknown);
                if (!unknown.empty())
                {
                    const nlohmann::json saved = nlohmann::json::parse(comp.source_json, nullptr, false);
                    nlohmann::json kept = nlohmann::json::object();
                    for (const std::string& key : unknown)
                    {
                        if (saved.is_object() && saved.contains(key)) { kept[key] = saved[key]; }
                    }
                    if (!kept.empty()) { unknown_fields[meta_key(type.name(), static_cast<u32_t>(type.id()))] = kept; }
                }

                if (tau::reflection::func_t on_changed = type.func("on_changed"_h); on_changed)
                {
                    on_changed.invoke({}, tau::reflection::forward_as_meta(world.registry), entity);
                }
            }

            if (unresolved.empty() && unknown_fields.empty())
            {
                world.registry.remove<unresolved_components_t>(entity);
            }
            else
            {
                world.registry.emplace_or_replace<unresolved_components_t>(entity, unresolved.dump(),
                                                                           unknown_fields.dump());
            }
        }

        // written next to the reflected ones, a reflected one of the same key wins
        void add_unresolved(const tau::ecs::registry_t& reg, tau::ecs::entity_t entity, nlohmann::json& components)
        {
            const unresolved_components_t* kept = reg.try_get<unresolved_components_t>(entity);
            if (!kept) { return; }

            const nlohmann::json saved = nlohmann::json::parse(kept->json, nullptr, false);
            if (saved.is_object())
            {
                for (const auto& [key, fields] : saved.items())
                {
                    if (!components.contains(key)) { components[key] = fields; }
                }
            }

            const nlohmann::json unknown = nlohmann::json::parse(kept->fields, nullptr, false);
            if (!unknown.is_object()) { return; }
            for (const auto& [key, fields] : unknown.items())
            {
                const auto component = components.find(key);
                if (component == components.end() || !component->is_object() || !fields.is_object()) { continue; }
                for (const auto& [field, value] : fields.items())
                {
                    if (!component->contains(field)) { (*component)[field] = value; }
                }
            }
        }
    } // namespace

    namespace
    {
        void apply_prefab_tags(tau::world_t& world, tau::ecs::entity_t entity, const scene_entity_t& scene_entity)
        {
            tau::ecs::registry_t& reg = world.registry;

            if (scene_entity.prefab_instance != 0)
            {
                reg.emplace_or_replace<prefab_part_t>(
                    entity, prefab_part_t{scene_entity.prefab_instance, scene_entity.prefab_source});
            }
            else
            {
                reg.remove<prefab_part_t>(entity);
            }

            if (!scene_entity.prefab_root)
            {
                reg.remove<prefab_instance_t>(entity);
                return;
            }

            prefab_instance_t instance;
            instance.removed = scene_entity.prefab_removed;
            instance.unresolved = scene_entity.prefab_unresolved;

            const std::string ref = tau::assets::resolve(scene_entity.prefab_guid, scene_entity.prefab_path);
            if (!ref.empty()) { instance.prefab = tau::assets::load<prefab_t>(ref); }

            reg.emplace_or_replace<prefab_instance_t>(entity, std::move(instance));
        }
    } // namespace

    std::vector<tau::ecs::entity_t> instantiate_scene(tau::world_t& world, const scene_t& scene)
    {
        std::vector<tau::ecs::entity_t> created;
        created.reserve(scene.entities.size());
        std::unordered_map<u64_t, tau::ecs::entity_t> by_file_id;

        for (const scene_entity_t& scene_entity : scene.entities)
        {
            const tau::ecs::entity_t entity = world.registry.create();

            // keep file guid unless made up or taken
            const bool fresh = scene.generated_ids || scene_entity.id == 0 ||
                               tau::guid_system::find(world.registry, scene_entity.id) != tau::ecs::NULL_ENTITY;
            tau::guid_system::assign(world.registry, entity, fresh ? tau::guid::generate() : scene_entity.id);

            by_file_id.emplace(scene_entity.id, entity);
            created.push_back(entity);
        }

        const entity_refs_t refs{.reg = &world.registry, .by_file_id = &by_file_id};
        for (std::size_t i = 0; i < scene.entities.size(); i++)
        {
            apply_scene_entity(world, created[i], scene.entities[i], refs);
            apply_prefab_tags(world, created[i], scene.entities[i]);
        }

        for (std::size_t i = 0; i < created.size(); i++)
        {
            const u64_t parent_id = scene.entities[i].parent;
            if (parent_id == 0) { continue; }

            const tau::ecs::entity_t parent = refs.decode(parent_id);
            if (parent == tau::ecs::NULL_ENTITY || !world.registry.all_of<transform_t>(created[i]) ||
                !world.registry.all_of<transform_t>(parent))
            {
                TAU_LOG_WARN("SCENE", "Entity {} has an invalid parent, left at the root", i);
                continue;
            }

            tau::transform_system::set_parent(world.registry, created[i], parent);
        }

        return created;
    }

    std::vector<tau::ecs::entity_t> deserialize_scene(tau::world_t& world, const nlohmann::json& scene_data)
    { return instantiate_scene(world, scene_from_json(tau::prefab::expand(scene_data, tau::prefab::asset_lookup()))); }

    namespace
    {
        u32_t depth_of(const tau::ecs::registry_t& reg, tau::ecs::entity_t entity)
        {
            u32_t depth = 0;
            for (const transform_t* t = reg.try_get<transform_t>(entity); t && t->get_parent() != tau::ecs::NULL_ENTITY;
                 t = reg.try_get<transform_t>(t->get_parent()))
            {
                depth++;
            }
            return depth;
        }
    } // namespace

    void apply_entities(tau::world_t& world, std::span<const entity_state_t> states)
    {
        tau::ecs::registry_t& reg = world.registry;

        std::unordered_map<u64_t, tau::ecs::entity_t> by_guid;
        for (const entity_state_t& s : states)
        {
            if (!s.state) { continue; }

            tau::ecs::entity_t entity = tau::guid_system::find(reg, s.guid);
            if (entity == tau::ecs::NULL_ENTITY)
            {
                entity = reg.create();
                tau::guid_system::assign(reg, entity, s.guid);
            }
            by_guid.emplace(s.guid, entity);
        }

        const entity_refs_t refs{.reg = &reg, .by_file_id = &by_guid, .search_world = true};
        const std::unordered_map<u32_t, tau::reflection::type_t> by_pool = component_types_by_pool(world);

        std::vector<tau::reflection::type_t> extra;
        for (const entity_state_t& s : states)
        {
            if (!s.state) { continue; }

            const tau::ecs::entity_t entity = by_guid.at(s.guid);
            const scene_entity_t parsed = parse_entity(*s.state);

            // in place: drop components the state lacks
            extra.clear();
            for (auto [pool_id, pool] : reg.storage())
            {
                const auto type_it = by_pool.find(static_cast<u32_t>(pool_id));
                if (type_it == by_pool.end() || !pool.contains(entity)) { continue; }

                const u32_t type_hash = static_cast<u32_t>(type_it->second.id());
                if (std::none_of(parsed.components.begin(), parsed.components.end(),
                                 [type_hash](const scene_component_t& c) { return c.type_hash == type_hash; }))
                {
                    extra.push_back(type_it->second);
                }
            }
            for (const tau::reflection::type_t& type : extra)
            {
                if (tau::reflection::func_t remove_func = type.func("remove"_h); remove_func)
                {
                    remove_func.invoke({}, tau::reflection::forward_as_meta(reg), entity);
                }
            }

            apply_scene_entity(world, entity, parsed, refs);
            apply_prefab_tags(world, entity, parsed);
        }

        auto place = [&](const entity_state_t& s)
        {
            const tau::ecs::entity_t entity = by_guid.at(s.guid);
            const transform_t* transform = reg.try_get<transform_t>(entity);
            if (!transform) { return; }

            tau::ecs::entity_t parent = refs.decode(guid::from_json(s.state->value("parent", nlohmann::json{})));
            if (parent != tau::ecs::NULL_ENTITY && !reg.all_of<transform_t>(parent)) { parent = tau::ecs::NULL_ENTITY; }
            const tau::ecs::entity_t next = refs.decode(guid::from_json(s.state->value("next", nlohmann::json{})));

            if (transform->get_parent() == parent && transform->get_next_sibling() == next) { return; }
            tau::transform_system::set_parent(reg, entity, parent, next);
        };

        // placed before its next sibling, which goes first if in the batch
        std::vector<const entity_state_t*> unplaced;
        std::unordered_set<u64_t> unplaced_guids;
        for (const entity_state_t& s : states)
        {
            if (!s.state) { continue; }
            unplaced.push_back(&s);
            unplaced_guids.insert(s.guid);
        }

        bool stuck = false;
        while (!unplaced.empty())
        {
            bool progressed = false;
            for (auto it = unplaced.begin(); it != unplaced.end();)
            {
                const u64_t next = guid::from_json((*it)->state->value("next", nlohmann::json{}));
                if (!stuck && next != 0 && unplaced_guids.contains(next))
                {
                    ++it;
                    continue;
                }

                place(**it);
                unplaced_guids.erase((*it)->guid);
                it = unplaced.erase(it);
                progressed = true;
            }
            // cycle: place the rest as they come
            if (!progressed) { stuck = true; }
        }

        // deepest first, a parent takes children a frame later
        std::vector<std::pair<u32_t, tau::ecs::entity_t>> doomed;
        for (const entity_state_t& s : states)
        {
            if (s.state) { continue; }

            const tau::ecs::entity_t entity = tau::guid_system::find(reg, s.guid);
            if (entity != tau::ecs::NULL_ENTITY) { doomed.emplace_back(depth_of(reg, entity), entity); }
        }
        std::sort(doomed.begin(), doomed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [depth, entity] : doomed)
        {
            if (reg.valid(entity)) { reg.destroy(entity); }
        }
    }

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

    void clear_scene(tau::world_t& world)
    {
        auto view = world.registry.view<tau::ecs::entity_t>();

        std::vector<tau::ecs::entity_t> to_destroy;
        to_destroy.insert(to_destroy.end(), view.begin(), view.end());

        world.registry.destroy(to_destroy.begin(), to_destroy.end());
    }
} // namespace tau::serialization
