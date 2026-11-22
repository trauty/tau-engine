#pragma once

#include "tau/defines.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// graphics settings: struct defaults <- preset (a partial data asset) <- command line --set
// every value is read when used, a module that built something from one compares at the frame boundary
namespace tau::renderer
{
    struct renderer_settings_t
    { bool vsync = true; };

    struct shadow_settings_t
    {
        // multiplies the scene's base shadow distance
        f32 distance_scale = 1.0f;
        // 0 uniform, 1 logarithmic cascade splits
        f32 split_lambda = 0.75f;
        // in shadow texels
        f32 normal_bias = 2.0f;
        // light space ndc depth, the raster slope bias covers the rest
        f32 depth_bias = 0.0f;
        f32 punctual_normal_bias = 1.5f;
        f32 punctual_depth_bias = 0.0015f;
        // punctual tiles rendered per frame, a new light renders at once
        u32_t punctual_tile_budget = 8;
    };

    // the --set path is <member>.<field>, e.g. shadows.split_lambda
    struct graphics_settings_t
    {
        renderer_settings_t renderer;
        shadow_settings_t shadows;
    };
} // namespace tau::renderer

namespace tau::renderer::settings
{
    // the preset table: named partial graphics_settings_t assets, in menu order
    constexpr std::string_view PRESETS_PATH = "engine://assets/settings/graphics_presets.data";

    struct args_t
    {
        // empty: the table's default
        std::string preset;
        // "shadows.split_lambda=0.5"
        std::vector<std::string> overrides;
    };

    // --preset=NAME, --set PATH=VALUE, --set=PATH=VALUE; i moves past what it took
    TAU_ENGINE_API bool take_arg(i32 argc, char** argv, i32& i, args_t& out);

    // false: no preset table, an unknown preset or a rejected override, each logged; the rest still applies
    bool init(const args_t& args);
    // resolves again when the table or the selected preset changed (an edit, a reload); once per frame
    void refresh();

    TAU_ENGINE_API const graphics_settings_t& get();
    TAU_ENGINE_API void set(const graphics_settings_t& value);

    // names in the table's order
    TAU_ENGINE_API std::span<const std::string> presets();
    TAU_ENGINE_API std::string_view preset();
    // resolves defaults <- preset <- the command line again
    TAU_ENGINE_API bool select_preset(std::string_view name);

    // init and every later select took every value
    TAU_ENGINE_API bool clean();
} // namespace tau::renderer::settings
