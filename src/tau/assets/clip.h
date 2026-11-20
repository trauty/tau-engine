#pragma once

#include "tau/asset.h"
#include "tau/defines.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tau
{
    enum class clip_path_e : u8_t
    {
        TRANSLATION,
        ROTATION,
        SCALE,
        WEIGHTS,
    };

    enum class clip_interpolation_e : u8_t
    {
        STEP,
        LINEAR,
        CUBIC_SPLINE,
    };

    struct TAU_ENGINE_API clip_channel_t
    {
        std::string target;
        clip_path_e path = clip_path_e::TRANSLATION;
        clip_interpolation_e interpolation = clip_interpolation_e::LINEAR;
        std::vector<f32> times;
        // 3 / 4 / 3 / target count floats per key, x3 for cubic spline
        std::vector<f32> values;
    };

    struct TAU_ENGINE_API clip_t
    {
        static constexpr std::string_view type_key = "clip";

        f32 duration = 0.0f;
        std::vector<clip_channel_t> channels;
    };

    template <>
    struct TAU_ENGINE_API asset_loader_t<clip_t>
    {
        static std::optional<clip_t> load(const std::string& path);
    };
} // namespace tau
