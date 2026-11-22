#pragma once

#include "tau/asset.h"
#include "tau/assets/data.h"
#include "tau/defines.h"
#include "tau/math.h"
#include "tau/render/view_profile.h"

#include <vector>

namespace tau
{
    struct material_t;

    // blends a profile asset (only the fields it sets) and effect overrides into the views inside it
    struct TAU_ENGINE_API profile_volume_t
    {
        data_ref_t<view_profile_t> profile;
        // matched to the camera's effects by shader
        std::vector<asset_t<material_t>> effect_overrides;
        bool is_global = false;
        vec3 extents = {5.0f, 5.0f, 5.0f};
        f32 blend_distance = 2.0f;
        f32 weight = 1.0f;
        i32_t priority = 0;
    };
} // namespace tau
