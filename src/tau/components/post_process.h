#pragma once

#include "tau/asset.h"
#include "tau/defines.h"
#include "tau/math.h"

#include <vector>

namespace tau
{
    struct material_t;

    inline constexpr const char* DEFAULT_FINAL_PASS_PATH = "engine://assets/materials/uberpost.mat";
    inline constexpr u32_t POST_FINAL_PASS_SLOT = 0xFFFFFFFFu;

    struct TAU_ENGINE_API post_process_t
    {
        std::vector<asset_t<material_t>> effects;
        asset_t<material_t> final_pass;
        bool enabled = true;

        void on_added();
    };

    struct TAU_ENGINE_API post_volume_t
    {
        asset_t<material_t> overrides;
        bool is_global = false;
        vec3 extents = {5.0f, 5.0f, 5.0f};
        f32 blend_distance = 2.0f;
        f32 weight = 1.0f;
        i32_t priority = 0;
    };
} // namespace tau
