#include "tau/reflection_json.h"

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

        nlohmann::json encode(const any_t& value)
        {
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

            if (is_struct(type))
            {
                nlohmann::json out = nlohmann::json::object();
                for (auto [id, data] : type.data()) { out[std::string(data.name())] = encode(data.get(value)); }
                return out;
            }

            return nullptr;
        }

        void reject(std::vector<std::string>* rejected, const std::string& path, const nlohmann::json* value = nullptr)
        {
            if (rejected) { rejected->push_back(value ? path + ": " + value->dump() : path); }
        }

        void apply_at(any_t& value, const nlohmann::json& object, std::vector<std::string>* rejected,
                      const std::string& prefix)
        {
            const type_t type = value.type();
            if (!object.is_object() || !is_struct(type))
            {
                reject(rejected, prefix.empty() ? "(root)" : prefix, &object);
                return;
            }

            for (const auto& [key, json] : object.items())
            {
                const std::string path = prefix.empty() ? key : prefix + "." + key;

                const reflection::data_t data = type.data(hash_string(key));
                if (!data)
                {
                    reject(rejected, path);
                    continue;
                }

                const type_t field_type = data.type();
                any_t field = data.get(value);

                if (const std::optional<scene_value_type_e> tag = reflection::pod_tag(field_type))
                {
                    void* bytes = reflection::mutable_value_data(field);
                    if (!bytes ||
                        !serialization::detail::pod_from_json(pod_layout(*tag), json, static_cast<u8_t*>(bytes)))
                    {
                        reject(rejected, path, &json);
                        continue;
                    }
                }
                else if (is_string(field_type))
                {
                    if (!json.is_string())
                    {
                        reject(rejected, path, &json);
                        continue;
                    }
                    field = any_t{std::in_place_type<std::string>, json.get<std::string>()};
                }
                else if (field_type.is_enum())
                {
                    bool found = false;
                    if (json.is_string())
                    {
                        for (auto [id, enumerator] : field_type.data())
                        {
                            if (!same_name(enumerator.name(), json.get_ref<const std::string&>())) { continue; }

                            field = enumerator.get({});
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        reject(rejected, path, &json);
                        continue;
                    }
                }
                else if (is_struct(field_type)) { apply_at(field, json, rejected, path); }
                else
                {
                    reject(rejected, path, &json);
                    continue;
                }

                if (!data.set(value, field)) { reject(rejected, path, &json); }
            }
        }
    } // namespace

    nlohmann::json to_json(const reflection::any_t& value) { return encode(value); }

    nlohmann::json diff(const reflection::any_t& value, const reflection::any_t& base)
    {
        nlohmann::json out = nlohmann::json::object();

        const reflection::type_t type = value.type();
        if (!is_struct(type) || base.type() != type) { return out; }

        for (auto [id, data] : type.data())
        {
            const reflection::any_t mine = data.get(value);
            const reflection::any_t theirs = data.get(base);

            nlohmann::json changed;
            if (is_struct(data.type()))
            {
                changed = diff(mine, theirs);
                if (changed.empty()) { continue; }
            }
            else
            {
                changed = encode(mine);
                if (changed == encode(theirs)) { continue; }
            }
            out[std::string(data.name())] = std::move(changed);
        }
        return out;
    }

    void apply(reflection::any_t& value, const nlohmann::json& object, std::vector<std::string>* rejected)
    { apply_at(value, object, rejected, {}); }
} // namespace tau::reflection_json
