#pragma once

#include "tau/defines.h"
#include "tau/ecs.h"
#include "tau/math.h"

namespace tau::camera_system
{
    TAU_ENGINE_API void set_fov(ecs::registry_t& reg, ecs::entity_t entity, f32 fov_deg);
    TAU_ENGINE_API ecs::entity_t get_active_camera(ecs::registry_t& reg);

    TAU_ENGINE_API void update(ecs::registry_t& reg);

    TAU_ENGINE_API void build_view_projection(vec3 eye, vec3 forward, vec3 up, f32 fov_deg, f32 aspect, f32 near_plane,
                                              f32 far_plane, mat4& out_view, mat4& out_projection, mat4& out_view_proj);
} // namespace tau::camera_system