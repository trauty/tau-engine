#include "tau/render/settings.h"

#include "tau/components/reflect.h"
#include "tau/log.h"
#include "tau/reflection.h"
#include "tau/reflection_json.h"

#include "json/json.hpp"
#include <array>

namespace tau::renderer::settings
{
    namespace
    {
        // lower scales first, the menu shows them in this order
        constexpr std::array PRESETS = {
            preset_t{"low",    R"({"shadows": {"distance_scale": 0.6}})"},
            preset_t{"medium", R"({"shadows": {"distance_scale": 0.8}})"},
            preset_t{"high",   "{}"                                     },
        };

        graphics_settings_t g_settings;
        std::string g_preset(DEFAULT_PRESET);
        std::vector<std::string> g_overrides;
        bool g_clean = true;

        const preset_t* find_preset(std::string_view name)
        {
            for (const preset_t& p : PRESETS)
            {
                if (p.name == name) { return &p; }
            }
            return nullptr;
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

        bool resolve()
        {
            bool ok = true;
            reflection::any_t value{std::in_place_type<graphics_settings_t>};

            const preset_t* p = find_preset(g_preset);
            if (p == nullptr)
            {
                TAU_LOG_ERROR("SETTINGS", "Unknown preset '{}', using '{}'", g_preset, DEFAULT_PRESET);
                g_preset = DEFAULT_PRESET;
                p = find_preset(g_preset);
                ok = false;
            }
            ok &= apply_layer(value, nlohmann::json::parse(p->layer), "preset " + g_preset);

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
        g_preset = args.preset.empty() ? std::string(DEFAULT_PRESET) : args.preset;
        g_overrides = args.overrides;
        g_clean = true;
        const bool ok = resolve();
        TAU_LOG_INFO("SETTINGS", "Preset '{}', {} override(s)", g_preset, g_overrides.size());
        return ok;
    }

    const graphics_settings_t& get() { return g_settings; }

    void set(const graphics_settings_t& value) { g_settings = value; }

    std::span<const preset_t> presets() { return PRESETS; }

    std::string_view preset() { return g_preset; }

    bool select_preset(std::string_view name)
    {
        if (find_preset(name) == nullptr)
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

        structure<renderer::graphics_settings_t>(ctx, "Graphics Settings")
            .field<&renderer::graphics_settings_t::renderer>("Renderer")
            .field<&renderer::graphics_settings_t::shadows>("Shadows");
    }
} // namespace tau::reflection
