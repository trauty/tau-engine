#pragma once

#include "tau/math.h"

namespace tau
{
    struct directional_light_t
    {
        vec3 color = {1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;
        // only one/first directional light drives the cascades
        bool casts_shadow = true;
    };

    struct point_light_t
    {
        vec3 color = {1.0f, 1.0f, 1.0f};
        float intensity = 10.0f;
        float radius = 10.0f;
        bool casts_shadow = true;
    };

    struct spot_light_t
    {
        vec3 color = {1.0f, 1.0f, 1.0f};
        float intensity = 15.0f;
        float radius = 20.0f;
        float inner_angle = 12.5f;
        float outer_angle = 17.5f;
        bool casts_shadow = true;
    };
} // namespace tau