#include "tau/reflection_json.h"

#include "tau/asset.h"
#include "tau/assets/scene_format.h"
#include "tau/hash.h"
#include "tau/serialization_detail.h"

#include <cctype>
#include <cmath>
#include <string_view>
#include <utility>

namespace tau::reflection_json
{
    namespace
    {
        using reflection::any_t;
        using reflection::type_t;

        bool is_string(const type_t& type) { return type.info().hash() == entt::type_hash<std::string>::value(); }

        bool is_struct(const type_t& type)
        {
            return !type.is_enum() && !reflection::pod_tag(type) && !is_string(type) &&
                   type.data().begin() != type.data().end();
        }

        bool same_name(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size()) { return false; }
            for (std::size_t i = 0; i < a.size(); i++)
            {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                {
                    return false;
                }
            }
            return true;
        }

        void tidy_floats(nlohmann::json& value)
        {
            if (value.is_number_float()) { value = std::round(value.get<f64>() * 1e6) / 1e6; }
            else if (value.is_array())
            {
                for (nlohmann::json& element : value) { tidy_floats(element); }
            }
        }

        const reflection::editor_prop_t* prop_of(const reflection::data_t& data)
        { return static_cast<const reflection::editor_prop_t*>(data.custom()); }

        nlohmann::json asset_json(asset_handle_t handle)
        {
            return {
                {"g", assets::guid_text(handle.guid)                            },
                {"p", handle.guid != 0 ? assets::path_of(handle) : std::string{}}
            };
        }

        bool asset_from_json(const nlohmann::json& json, asset_handle_t& out)
        {
            if (json.is_null())
            {
                out = {};
                return true;
            }
            if (!json.is_object()) { return false; }

            asset_guid_t guid = assets::parse_guid(json.value("g", std::string{}));
            if (guid == 0) { guid = assets::guid_of(json.value("p", std::string{})); }
            out = asset_handle_t{guid};
            return true;
        }

        nlohmann::json encode(const any_t& value, const reflection::editor_prop_t* prop)
        {
            if (prop && prop->asset_type_hash != 0)
            {
                if (!prop->is_list) { return asset_json(reflection::asset_of(value)); }

                nlohmann::json refs = nlohmann::json::array();
                for (const asset_handle_t handle : reflection::assets_of(value)) { refs.push_back(asset_json(handle)); }
                return refs;
            }

            const type_t type = value.type();

            if (const std::optional<scene_value_type_e> tag = reflection::pod_tag(type))
            {
                const u8_t* bytes = static_cast<const u8_t*>(reflection::value_data(value));
                if (!bytes) { return nullptr; }

                const pod_layout_t layout = pod_layout(*tag);
                nlohmann::json out = serialization::detail::pod_to_json(layout, bytes);
                if (layout.scalar == scalar_e::F32) { tidy_floats(out); }
                return out;
            }
            if (is_string(type)) { return value.cast<std::string>(); }

            if (type.is_enum())
            {
                const any_t as_int = value.allow_cast<i32>();
                if (!as_int) { return nullptr; }

                const i32 number = as_int.cast<i32>();
                for (auto [id, data] : type.data())
                {
                    const any_t constant = data.get({});
                    const any_t constant_int = constant.allow_cast<i32>();
                    if (constant_int && constant_int.cast<i32>() == number) { return std::string(data.name()); }
                }
                return number;
            }

            if (type.is_sequence_container())
            {
                any_t copy = value;
                nlohmann::json out = nlohmann::json::array();
                for (any_t element : copy.as_sequence_container()) { out.push_back(encode(element, nullptr)); }
                return out;
            }

            if (is_struct(type))
            {
                nlohmann::json out = nlohmann::json::object();
                for (auto [id, data] : type.data())
                {
                    const reflection::editor_prop_t* field_prop = prop_of(data);
                    if (field_prop && has_flag(field_prop->flags, reflection::prop_flags_e::NOT_SAVED)) { continue; }
                    out[std::string(data.name())] = encode(data.get(value), field_prop);
                }
                return out;
            }

            return nullptr;
        }

        void reject(std::vector<std::string>* rejected, const std::string& path, const nlohmann::json* value = nullptr)
        {
            if (rejected) { rejected->push_back(value ? path + ": " + value->dump() : path); }
        }

        bool decode(any_t& target, const nlohmann::json& json, const reflection::editor_prop_t* prop,
                    std::vector<std::string>* rejected, const std::string& path)
        {
            if (prop && prop->asset_type_hash != 0)
            {
                if (!prop->is_list)
                {
                    asset_handle_t handle;
                    if (!asset_from_json(json, handle)) { return false; }
                    target = any_t{std::in_place_type<asset_handle_t>, handle};
                    return true;
                }

                if (!json.is_array()) { return false; }
                std::vector<asset_handle_t> handles;
                for (const nlohmann::json& element : json)
                {
                    if (!asset_from_json(element, handles.emplace_back())) { return false; }
                }
                target = any_t{std::in_place_type<std::vector<asset_handle_t>>, std::move(handles)};
                return true;
            }

            const type_t type = target.type();

            if (const std::optional<scene_value_type_e> tag = reflection::pod_tag(type))
            {
                void* bytes = reflection::mutable_value_data(target);
                return bytes && serialization::detail::pod_from_json(pod_layout(*tag), json, static_cast<u8_t*>(bytes));
            }
            if (is_string(type))
            {
                if (!json.is_string()) { return false; }
                target = any_t{std::in_place_type<std::string>, json.get<std::string>()};
                return true;
            }
            if (type.is_enum())
            {
                if (!json.is_string()) { return false; }
                for (auto [id, enumerator] : type.data())
                {
                    if (!same_name(enumerator.name(), json.get_ref<const std::string&>())) { continue; }
                    target = enumerator.get({});
                    return true;
                }
                return false;
            }
            if (type.is_sequence_container())
            {
                if (!json.is_array()) { return false; }

                auto list = target.as_sequence_container();
                const type_t element_type = list.value_type();
                list.clear();
                for (std::size_t i = 0; i < json.size(); i++)
                {
                    any_t element = element_type.construct();
                    const std::string element_path = path + "[" + std::to_string(i) + "]";
                    if (!element || !decode(element, json[i], nullptr, rejected, element_path))
                    {
                        reject(rejected, element_path, &json[i]);
                        continue;
                    }
                    list.insert(list.end(), element);
                }
                return true;
            }
            if (is_struct(type))
            {
                if (!json.is_object()) { return false; }

                for (const auto& [key, field_json] : json.items())
                {
                    const std::string field_path = path.empty() ? key : path + "." + key;

                    const reflection::data_t data = type.data(hash_string(key));
                    if (!data)
                    {
                        reject(rejected, field_path);
                        continue;
                    }

                    any_t field = data.get(target);
                    if (!decode(field, field_json, prop_of(data), rejected, field_path) || !data.set(target, field))
                    {
                        reject(rejected, field_path, &field_json);
                    }
                }
                return true;
            }
            return false;
        }
    } // namespace

    nlohmann::json to_json(const reflection::any_t& value) { return encode(value, nullptr); }

    nlohmann::json diff(const reflection::any_t& value, const reflection::any_t& base)
    {
        nlohmann::json out = nlohmann::json::object();

        const reflection::type_t type = value.type();
        if (!is_struct(type) || base.type() != type) { return out; }

        for (auto [id, data] : type.data())
        {
            const reflection::any_t mine = data.get(value);
            const reflection::any_t theirs = data.get(base);
            const reflection::editor_prop_t* prop = prop_of(data);
            if (prop && has_flag(prop->flags, reflection::prop_flags_e::NOT_SAVED)) { continue; }

            const bool nested = (!prop || prop->asset_type_hash == 0) && is_struct(data.type());

            nlohmann::json changed;
            if (nested)
            {
                changed = diff(mine, theirs);
                if (changed.empty()) { continue; }
            }
            else
            {
                changed = encode(mine, prop);
                if (changed == encode(theirs, prop)) { continue; }
            }
            out[std::string(data.name())] = std::move(changed);
        }
        return out;
    }

    void apply(reflection::any_t& value, const nlohmann::json& object, std::vector<std::string>* rejected)
    {
        if (!object.is_object() || !is_struct(value.type()))
        {
            reject(rejected, "(root)", &object);
            return;
        }
        decode(value, object, nullptr, rejected, {});
    }
} // namespace tau::reflection_json
