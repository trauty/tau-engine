#pragma once

#include "tau/defines.h"
#include "tau/math.h"

namespace tau
{
    enum class light_type_e : u8_t
    {
        SUN,
        POINT,
        SPOT,
        RECT,
        TUBE,
    };

    enum class light_shadows_e : u8_t
    {
        OFF,
        ON,
    };

    // one component for every light, so a type change is a field change; faces its -Z
    // physical units: the sun in lux, every other light in lumens (a spot's cone only masks: I = lumens / 4 pi)
    struct TAU_ENGINE_API light_t
    {
        light_type_e type = light_type_e::POINT;
        // multiplies, not normalised
        vec3 color = {1.0f, 1.0f, 1.0f};
        // lux for the sun, lumen otherwise
        f32 intensity = 800.0f;
        f32 range = 10.0f;
        // degrees, spot only
        f32 inner_angle = 30.0f;
        f32 outer_angle = 45.0f;
        // rect: width, height; tube: length, radius
        vec2 size = {1.0f, 1.0f};
        light_shadows_e shadows = light_shadows_e::ON;
        // scales its scattering in fog
        f32 volumetric = 1.0f;
    };
} // namespace tau
