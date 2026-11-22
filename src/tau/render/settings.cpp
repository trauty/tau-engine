#include "tau/render/settings.h"

#include "tau/asset.h"
#include "tau/assets/data.h"
#include "tau/components/reflect.h"
#include "tau/log.h"
#include "tau/reflection.h"
#include "tau/reflection_json.h"

#include "json/json.hpp"
#include <algorithm>

namespace tau::renderer
{
    struct graphics_preset_ref_t
    {
        std::string name;
        data_ref_t<graphics_settings_t> layer;
    };

    struct graphics_presets_t
    {
        std::vector<graphics_preset_ref_t> presets;
        std::string default_preset = "high";
    };
} // namespace tau::renderer

namespace tau::renderer::settings
{
    namespace
    {
        asset_t<data_asset_t> g_table;
        // what the last resolve read, a change resolves again
        u32_t g_table_version = 0;
        u32_t g_layer_version = 0;
        asset_handle_t g_layer;

        std::vector<std::string> g_names;
        graphics_settings_t g_settings;
        std::string g_preset;
        std::vector<std::string> g_overrides;
        bool g_clean = true;

        u32_t version_of(asset_handle_t handle)
        {
            const data_asset_t* asset = assets::get(asset_t<data_asset_t>{handle});
            return asset ? asset->version : 0;
        }

        // "shadows.split_lambda=0.5" -> {"shadows": {"split_lambda": 0.5}}
        // the value is JSON when it parses, a string otherwise (enum names)
        bool override_layer(std::string_view text, nlohmann::json& out)
        {
            const std::size_t eq = text.find('=');
            if (eq == std::string_view::npos || eq == 0 || eq + 1 == text.size()) { return false; }

            const std::string_view path = text.substr(0, eq);
            const std::string value_text(text.substr(eq + 1));

            nlohmann::json value = nlohmann::json::parse(value_text, nullptr, false);
            if (value.is_discarded()) { value = value_text; }

            out = nlohmann::json::object();
            nlohmann::json* at = &out;
            std::size_t start = 0;
            while (true)
            {
                const std::size_t dot = path.find('.', start);
                const std::string key(path.substr(start, dot == std::string_view::npos ? path.npos : dot - start));
                if (key.empty()) { return false; }
                if (dot == std::string_view::npos)
                {
                    (*at)[key] = std::move(value);
                    return true;
                }
                at = &(*at)[key];
                start = dot + 1;
            }
        }

        bool apply_layer(reflection::any_t& value, const nlohmann::json& layer, std::string_view source)
        {
            std::vector<std::string> rejected;
            reflection_json::apply(value, layer, &rejected);
            for (const std::string& r : rejected) { TAU_LOG_ERROR("SETTINGS", "{}: rejected {}", source, r); }
            return rejected.empty();
        }

        // a preset applies only the fields its file sets
        bool apply_preset(graphics_settings_t& value)
        {
            const graphics_presets_t* table = data::get<graphics_presets_t>(g_table);
            g_table_version = version_of(g_table);
            g_layer = {};
            g_layer_version = 0;
            g_names.clear();
            if (table == nullptr)
            {
                TAU_LOG_ERROR("SETTINGS", "No preset table at {}, using the struct defaults", PRESETS_PATH);
                return false;
            }

            for (const graphics_preset_ref_t& row : table->presets) { g_names.push_back(row.name); }
            if (g_preset.empty()) { g_preset = table->default_preset; }

            auto row = std::ranges::find(table->presets, g_preset, &graphics_preset_ref_t::name);
            bool ok = true;
            if (row == table->presets.end())
            {
                TAU_LOG_ERROR("SETTINGS", "Unknown preset '{}', using '{}'", g_preset, table->default_preset);
                g_preset = table->default_preset;
                row = std::ranges::find(table->presets, g_preset, &graphics_preset_ref_t::name);
                ok = false;
                if (row == table->presets.end()) { return false; }
            }

            g_layer = row->layer.handle;
            g_layer_version = version_of(g_layer);
            const data_asset_t* layer = assets::get(asset_t<data_asset_t>{g_layer});
            if (layer == nullptr || layer->type_hash != data::type_hash<graphics_settings_t>())
            {
                TAU_LOG_ERROR("SETTINGS", "Preset '{}' names no graphics settings asset", g_preset);
                return false;
            }
            return data::decode(layer->type_hash, &value, layer->fields, assets::path_of(g_layer)) && ok;
        }

        bool resolve()
        {
            graphics_settings_t resolved;
            bool ok = apply_preset(resolved);

            reflection::any_t value{std::in_place_type<graphics_settings_t>, resolved};
            for (const std::string& text : g_overrides)
            {
                nlohmann::json layer;
                if (!override_layer(text, layer))
                {
                    TAU_LOG_ERROR("SETTINGS", "--set {}: expected struct.field=value", text);
                    ok = false;
                    continue;
                }
                ok &= apply_layer(value, layer, "--set " + text);
            }

            g_settings = value.cast<graphics_settings_t>();
            g_clean = g_clean && ok;
            return ok;
        }
    } // namespace

    bool take_arg(i32 argc, char** argv, i32& i, args_t& out)
    {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--preset="))
        {
            out.preset = arg.substr(9);
            return true;
        }
        if (arg.starts_with("--set="))
        {
            out.overrides.emplace_back(arg.substr(6));
            return true;
        }
        if (arg == "--set" && i + 1 < argc)
        {
            out.overrides.emplace_back(argv[++i]);
            return true;
        }
        return false;
    }

    bool init(const args_t& args)
    {
        g_table = assets::pin(assets::load<data_asset_t>(PRESETS_PATH));
        g_preset = args.preset;
        g_overrides = args.overrides;
        g_clean = true;
        const bool ok = resolve();
        TAU_LOG_INFO("SETTINGS", "Preset '{}', {} override(s)", g_preset, g_overrides.size());
        return ok;
    }

    const graphics_settings_t& get() { return g_settings; }

    void set(const graphics_settings_t& value) { g_settings = value; }

    void refresh()
    {
        if (version_of(g_table) != g_table_version || (g_layer && version_of(g_layer) != g_layer_version))
        {
            resolve();
        }
    }

    std::span<const std::string> presets() { return g_names; }

    std::string_view preset() { return g_preset; }

    bool select_preset(std::string_view name)
    {
        if (std::ranges::find(g_names, name) == g_names.end())
        {
            TAU_LOG_ERROR("SETTINGS", "Unknown preset '{}'", name);
            return false;
        }
        g_preset = name;
        return resolve();
    }

    bool clean() { return g_clean; }
} // namespace tau::renderer::settings

namespace tau::reflection
{
    void reflect_graphics_settings(ctx_t& ctx)
    {
        structure<renderer::renderer_settings_t>(ctx, "Renderer").field<&renderer::renderer_settings_t::vsync>("VSync");

        using shadows_t = renderer::shadow_settings_t;
        structure<shadows_t>(ctx, "Shadows")
            .field<&shadows_t::distance_scale>("Distance Scale")
            .field<&shadows_t::split_lambda>("Split Lambda")
            .field<&shadows_t::normal_bias>("Normal Bias")
            .field<&shadows_t::depth_bias>("Depth Bias")
            .field<&shadows_t::punctual_normal_bias>("Punctual Normal Bias")
            .field<&shadows_t::punctual_depth_bias>("Punctual Depth Bias")
            .field<&shadows_t::punctual_tile_budget>("Punctual Tile Budget");

        data_type<renderer::graphics_settings_t>(ctx, "Graphics Settings")
            .partial()
            .field<&renderer::graphics_settings_t::renderer>("Renderer")
            .field<&renderer::graphics_settings_t::shadows>("Shadows");

        structure<renderer::graphics_preset_ref_t>(ctx, "Graphics Preset")
            .field<&renderer::graphics_preset_ref_t::name>("Name")
            .field<&renderer::graphics_preset_ref_t::layer>("Layer");
        data_type<renderer::graphics_presets_t>(ctx, "Graphics Presets")
            .field<&renderer::graphics_presets_t::presets>("Presets")
            .field<&renderer::graphics_presets_t::default_preset>("Default Preset");
    }
} // namespace tau::reflection
