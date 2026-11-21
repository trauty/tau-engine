#pragma once

#include "tau/defines.h"
#include "tau/math.h"

namespace tau
{
    enum class body_type_e : u8_t
    {
        STATIC,
        KINEMATIC,
        DYNAMIC
    };

    // transform move teleports, kinematic moves over the step
    struct TAU_ENGINE_API rigidbody_t
    {
        body_type_e type = body_type_e::DYNAMIC;
        bool is_sensor = false;
        bool continuous = false;
        u32_t layer = 0;

        f32 mass = 1.0f;
        f32 friction = 0.5f;
        f32 restitution = 0.0f;
        f32 linear_damping = 0.05f;
        f32 angular_damping = 0.05f;
        f32 gravity_scale = 1.0f;
        bool interpolate = true;
    };

    // one collider per body, a rigidbody without gets a unit box
    struct TAU_ENGINE_API box_collider_t
    {
        vec3 extents = {0.5f, 0.5f, 0.5f};
        vec3 offset = {0.0f, 0.0f, 0.0f};
    };

    struct TAU_ENGINE_API sphere_collider_t
    {
        f32 radius = 0.5f;
        vec3 offset = {0.0f, 0.0f, 0.0f};
    };

    struct TAU_ENGINE_API capsule_collider_t
    {
        f32 radius = 0.5f;
        f32 height = 2.0f;
        vec3 offset = {0.0f, 0.0f, 0.0f};
    };
} // namespace tau
