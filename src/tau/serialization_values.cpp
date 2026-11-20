#include "tau/asset.h"
#include "tau/assets/scene_format.h"
#include "tau/guid_json.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/serialization.h"
#include "tau/serialization_detail.h"

#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// one field value: reflected value <-> tagged JSON, JSON -> scene_property_t -> reflected value
namespace tau::serialization
{
    using namespace detail;

    namespace
    {
        nlohmann::json tagged(scene_value_type_e type, nlohmann::json value)
        {
            return {
                {"ty", static_cast<u8_t>(type)},
                {"v",  std::move(value)       }
            };
        }

        std::string_view enumerator_name(const tau::reflection::type_t& type, i32 value)
        {
            for (auto [id, data] : type.data())
            {
                const tau::reflection::any_t constant = data.get({});
                const tau::reflection::any_t as_int = constant.allow_cast<i32>();
                if (as_int && as_int.cast<i32>() == value) { return data.name(); }
            }
            return {};
        }

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
            if (type == tau::reflection::resolve<tau::ecs::entity_t>(ctx))
            {
                return tagged(scene_value_type_e::ENTITY, guid::to_json(refs.encode(value.cast<tau::ecs::entity_t>())));
            }

            if (type.is_enum())
            {
                const tau::reflection::any_t as_int = std::as_const(value).allow_cast<i32>();
                if (as_int)
                {
                    nlohmann::json out = tagged(scene_value_type_e::I32, as_int.cast<i32>());
                    if (const std::string_view name = enumerator_name(type, as_int.cast<i32>()); !name.empty())
                    {
                        out["n"] = name;
                    }
                    return out;
                }
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
                         type.name().empty() ? std::string_view{"?"} : type.name());
            return {};
        }

        // built on `current` so fields missing from the file keep their value, empty when nothing applies
        tau::reflection::any_t decode_value(tau::world_t& world, const scene_property_t& prop,
                                            tau::reflection::type_t type, tau::reflection::any_t current,
                                            const entity_refs_t& refs, const std::string& owner)
        {
            tau::reflection::ctx_t& ctx = *world.reflection_ctx;

            if (prop.enum_name != 0 && type.is_enum())
            {
                if (const tau::reflection::data_t enumerator = type.data(prop.enum_name)) { return enumerator.get({}); }
                if (type.data().begin() != type.data().end() &&
                    first_warning(owner + ".enum." + std::to_string(prop.enum_name)))
                {
                    TAU_LOG_WARN("SCENE", "Saved enum value '{}' on '{}' matches no enumerator, loaded by its number",
                                 describe_key(prop.str, prop.enum_name), owner);
                }
            }

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
    } // namespace

    namespace detail
    {
        bool is_numeric_key(const std::string& key)
        { return !key.empty() && key.find_first_not_of("0123456789") == std::string::npos; }

        u32_t key_to_hash(const std::string& key)
        { return is_numeric_key(key) ? static_cast<u32_t>(std::stoul(key)) : tau::hash_string(key); }

        std::string describe_key(const std::string& name, u32_t hash)
        { return name.empty() ? std::to_string(hash) : name; }

        asset_guid_t ref_guid(const nlohmann::json& ref)
        {
            const nlohmann::json guid = ref.value("g", nlohmann::json{});
            return guid.is_string() ? tau::assets::parse_guid(guid.get<std::string>()) : 0;
        }

        nlohmann::json encode_fields(tau::world_t& world, tau::reflection::type_t type,
                                     tau::reflection::any_t& instance, const entity_refs_t& refs)
        {
            nlohmann::json fields = nlohmann::json::object();

            for (auto [data_id, data] : type.data())
            {
                tau::reflection::any_t field_value = data.get(instance);
                const std::string prop_key = tau::reflection::meta_key(data.name(), data_id);

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
                if (pod_from_json(layout, v, prop.pod.data()))
                {
                    if (const auto name = val.find("n"); prop.type == scene_value_type_e::I32 && name != val.end() &&
                                                         name->is_string() && !name->get<std::string>().empty())
                    {
                        prop.str = name->get<std::string>();
                        prop.enum_name = tau::hash_string(prop.str);
                    }
                    return true;
                }
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
                        TAU_LOG_WARN("SCENE", "Unknown field '{}' on '{}', {}. Renamed? stable_id{{\"{}\"}} loads it",
                                     key, owner, unknown ? "kept as saved" : "value dropped", key);
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
    } // namespace detail
} // namespace tau::serialization
