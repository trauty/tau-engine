#pragma once

#include "tau/ecs_fwd.h"
#include "tau/math.h"

#include <span>

namespace tau::transform_system
{
    TAU_ENGINE_API void set_local_position(ecs::registry_t& reg, ecs::entity_t entity, vec3 new_pos);
    TAU_ENGINE_API void set_local_rotation(ecs::registry_t& reg, ecs::entity_t entity, quat new_rot);
    TAU_ENGINE_API void set_local_scale(ecs::registry_t& reg, ecs::entity_t entity, vec3 new_scale);

    // the shown pose as of the last update; an entity created since then: compose_world
    TAU_ENGINE_API mat4 world_matrix(const ecs::registry_t& reg, ecs::entity_t entity);
    TAU_ENGINE_API vec3 get_world_position(const ecs::registry_t& reg, ecs::entity_t entity);
    TAU_ENGINE_API vec3 get_world_scale(const ecs::registry_t& reg, ecs::entity_t entity);
    TAU_ENGINE_API quat get_world_rotation(const ecs::registry_t& reg, ecs::entity_t entity);

    TAU_ENGINE_API void set_world_position(ecs::registry_t& reg, ecs::entity_t entity, vec3 world_pos);
    TAU_ENGINE_API void set_world_rotation(ecs::registry_t& reg, ecs::entity_t entity, quat world_rot);

    // before: sibling to go in front of, null = last
    TAU_ENGINE_API void set_parent(ecs::registry_t& reg, ecs::entity_t child, ecs::entity_t parent,
                                   ecs::entity_t before = ecs::NULL_ENTITY);
    // keeps the world pose, local values adjust
    TAU_ENGINE_API void set_parent_keep_world(ecs::registry_t& reg, ecs::entity_t child, ecs::entity_t parent,
                                              ecs::entity_t before = ecs::NULL_ENTITY);

    // the true world matrix, from the locals chain now
    TAU_ENGINE_API mat4 compose_world(const ecs::registry_t& reg, ecs::entity_t entity);

    TAU_ENGINE_API ecs::entity_t first_root(const ecs::registry_t& reg);

    // entities whose shown world matrix changed in the last update, valid until the next one
    TAU_ENGINE_API std::span<const ecs::entity_t> changed(const ecs::registry_t& reg);

    void connect(ecs::registry_t& reg);
    void update(ecs::registry_t& reg);
} // namespace tau::transform_system