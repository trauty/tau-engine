#include "tau/assets/import_settings.h"

#include "tau/asset.h"
#include "tau/log.h"
#include "tau/reflection_json.h"

#include <string>
#include <vector>

namespace tau::import_settings
{
    namespace
    {
        void apply_layer(reflection::any_t& value, const nlohmann::json& layer, std::string_view owner)
        {
            if (layer.is_null()) { return; }

            std::vector<std::string> rejected;
            reflection_json::apply(value, layer, owner.empty() ? nullptr : &rejected);
            for (const std::string& key : rejected)
            {
                TAU_LOG_WARN("IMPORT", "{}: import setting '{}' is unknown or invalid, ignored", owner, key);
            }
        }
    } // namespace

    reflection::type_t type_of(u64_t asset_type_id)
    {
        const asset_type_t* type = assets::type_by_id(asset_type_id);
        if (!type || type->import_type == 0) { return {}; }
        return entt::resolve(reflection::get_engine_context(), type->import_type);
    }

    reflection::any_t base(u64_t asset_type_id, const nlohmann::json& project_defaults)
    {
        const reflection::type_t type = type_of(asset_type_id);
        if (!type) { return {}; }

        reflection::any_t value = type.construct();
        const asset_type_t* asset_type = assets::type_by_id(asset_type_id);
        if (project_defaults.is_object())
        {
            const auto it = project_defaults.find(std::string(asset_type->key));
            if (it != project_defaults.end())
            {
                apply_layer(value, *it, "project import_defaults." + std::string(asset_type->key));
            }
        }
        return value;
    }

    reflection::any_t resolve(u64_t asset_type_id, const nlohmann::json& project_defaults, const nlohmann::json& meta,
                              std::string_view owner)
    {
        reflection::any_t value = base(asset_type_id, project_defaults);
        if (!value || !meta.is_object()) { return value; }

        const auto it = meta.find(META_KEY);
        if (it != meta.end()) { apply_layer(value, *it, owner); }
        return value;
    }

    nlohmann::json meta_object(const reflection::any_t& value, const reflection::any_t& base)
    { return reflection_json::diff(value, base); }

    std::string fingerprint(u64_t asset_type_id, const nlohmann::json& project_defaults, const nlohmann::json& meta,
                            std::string_view owner)
    {
        const reflection::any_t value = resolve(asset_type_id, project_defaults, meta, owner);
        return value ? reflection_json::to_json(value).dump() : std::string{};
    }
} // namespace tau::import_settings

namespace tau::reflection
{
    void reflect_import_settings(ctx_t& ctx)
    {
        enumeration<texture_kind_e>(ctx, "Texture Kind");

        structure<texture_import_t>(ctx, "Texture Import")
            .field<&texture_import_t::kind>("Kind")
            .field<&texture_import_t::srgb>("sRGB")
            .field<&texture_import_t::max_size>("Max Size")
            .field<&texture_import_t::mips>("Mips");

        structure<mesh_import_t>(ctx, "Mesh Import").field<&mesh_import_t::scale>("Scale");
    }
} // namespace tau::reflection
