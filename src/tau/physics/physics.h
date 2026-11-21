#pragma once

#include "tau/defines.h"
#include "tau/ecs_fwd.h"
#include "tau/math.h"

#include <span>
#include <vector>

namespace tau { struct world_t; }

// bodies exist while playing, created at the next fixed step
// velocities/forces/impulses wait for the body
// queries see bodies only (nothing before Play)
namespace tau::physics
{
    constexpr u32_t MAX_LAYERS = 32;
    constexpr u32_t ALL_LAYERS = 0xffffffffu;

    enum class contact_phase_e : u8_t
    {
        BEGIN,
        END,
    };

    struct contact_t
    {
        ecs::entity_t a = ecs::NULL_ENTITY;
        ecs::entity_t b = ecs::NULL_ENTITY;
        contact_phase_e phase = contact_phase_e::BEGIN;
        vec3 point;
        vec3 normal;
    };

    // fixed update: previous step's contacts; update: this frame's steps
    // destroyed body reports no END, entity may be gone when read
    TAU_ENGINE_API std::span<const contact_t> contacts(const world_t& world);

    struct query_filter_t
    {
        u32_t layers = ALL_LAYERS;
        bool sensors = false;
        ecs::entity_t ignore = ecs::NULL_ENTITY;
    };

    struct ray_hit_t
    {
        ecs::entity_t entity = ecs::NULL_ENTITY;
        vec3 point;
        vec3 normal;
        f32 distance = 0.0f;
    };

    // nearest hit, ray starting inside hits at 0
    TAU_ENGINE_API bool raycast(const world_t& world, const vec3& origin, const vec3& direction, f32 max_distance,
                                ray_hit_t& out, const query_filter_t& filter = {});
    TAU_ENGINE_API std::vector<ray_hit_t> raycast_all(const world_t& world, const vec3& origin, const vec3& direction,
                                                      f32 max_distance, const query_filter_t& filter = {});
    TAU_ENGINE_API std::vector<ecs::entity_t> overlap_sphere(const world_t& world, const vec3& center, f32 radius,
                                                             const query_filter_t& filter = {});
    TAU_ENGINE_API std::vector<ecs::entity_t> overlap_box(const world_t& world, const vec3& center,
                                                          const vec3& half_extents, const quat& rotation,
                                                          const query_filter_t& filter = {});

    TAU_ENGINE_API vec3 linear_velocity(const world_t& world, ecs::entity_t entity);
    TAU_ENGINE_API void set_linear_velocity(world_t& world, ecs::entity_t entity, const vec3& velocity);
    TAU_ENGINE_API vec3 angular_velocity(const world_t& world, ecs::entity_t entity);
    TAU_ENGINE_API void set_angular_velocity(world_t& world, ecs::entity_t entity, const vec3& velocity);

    // dynamic only; forces over the next step, impulses at once
    TAU_ENGINE_API void add_force(world_t& world, ecs::entity_t entity, const vec3& force);
    TAU_ENGINE_API void add_torque(world_t& world, ecs::entity_t entity, const vec3& torque);
    TAU_ENGINE_API void add_impulse(world_t& world, ecs::entity_t entity, const vec3& impulse);
    TAU_ENGINE_API void add_angular_impulse(world_t& world, ecs::entity_t entity, const vec3& impulse);

    TAU_ENGINE_API void set_layers_collide(world_t& world, u32_t a, u32_t b, bool collide);
    TAU_ENGINE_API bool layers_collide(const world_t& world, u32_t a, u32_t b);

    TAU_ENGINE_API void set_gravity(world_t& world, const vec3& gravity);
    TAU_ENGINE_API vec3 gravity(const world_t& world);

    TAU_ENGINE_API void draw_colliders(ecs::registry_t& reg, u32_t view_hash);
} // namespace tau::physics
