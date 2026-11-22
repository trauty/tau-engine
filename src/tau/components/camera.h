#pragma once

#include "tau/asset.h"
#include "tau/defines.h"
#include "tau/math.h"
#include "tau/render/view_profile.h"

#include <vector>

namespace tau
{
    struct material_t;

    struct TAU_ENGINE_API active_camera_tag
    {
    };

    struct TAU_ENGINE_API camera_t
    {
        f32 fov_deg = 45.0f;
        f32 near_plane = 0.1f;
        f32 far_plane = 1000.0f;
        f32 aspect = 16.0f / 9.0f;

        view_profile_t profile;
        // single pass post effect materials, in order
        std::vector<asset_t<material_t>> effects;

        mat4 view;
        mat4 projection;
        mat4 view_proj;

        bool is_dirty = true;
    };
} // namespace tau