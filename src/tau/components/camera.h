#pragma once

#include "tau/defines.h"
#include "tau/math.h"

namespace tau
{
    struct TAU_ENGINE_API active_camera_tag
    {
    };

    struct TAU_ENGINE_API camera_t
    {
        f32 fov_deg = 45.0f;
        f32 near_plane = 0.1f;
        f32 far_plane = 1000.0f;
        f32 aspect = 16.0f / 9.0f;

        mat4 view;
        mat4 projection;
        mat4 view_proj;

        bool is_dirty = true;
    };
} // namespace tau