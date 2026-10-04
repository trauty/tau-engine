#pragma once

#include "tau/ecs_fwd.h"
#include "tau/math.h"

namespace tau::transform_system
{
    TAU_ENGINE_API void set_local_position(ecs::registry_t& reg, ecs::entity_t entity, vec3_t new_pos);
    TAU_ENGINE_API void set_local_rotation(ecs::registry_t& reg, ecs::entity_t entity, quat_t new_rot);
    TAU_ENGINE_API void set_local_scale(ecs::registry_t& reg, ecs::entity_t entity, vec3_t new_scale);

    TAU_ENGINE_API vec3_t get_world_position(ecs::registry_t& reg, ecs::entity_t entity);
    TAU_ENGINE_API vec3_t get_world_scale(ecs::registry_t& reg, ecs::entity_t entity);
    TAU_ENGINE_API quat_t get_world_rotation(ecs::registry_t& reg, ecs::entity_t entity);

    TAU_ENGINE_API void set_world_position(ecs::registry_t& reg, ecs::entity_t entity, vec3_t world_pos);

    TAU_ENGINE_API void set_parent(ecs::registry_t& reg, ecs::entity_t child, ecs::entity_t parent);

    void connect(ecs::registry_t& reg);
    void update(ecs::registry_t& reg);
} // namespace tau::transform_system