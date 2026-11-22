#pragma once

#include "tau/defines.h"
#include "tau/ecs.h"
#include "tau/ecs_fwd.h"
#include "tau/math.h"

namespace tau
{
    namespace transform_system { struct internal_t; }

    struct shown_pose_t
    {
        vec3 position;
        quat rotation;
    };

    struct TAU_ENGINE_API transform_t
    {
        vec3 local_position;
        quat local_rotation;
        vec3 local_scale = {1.0f, 1.0f, 1.0f};

        ecs::entity_t get_parent() const { return parent; }
        ecs::entity_t get_first_child() const { return first_child; }
        ecs::entity_t get_next_sibling() const { return next_sibling; }
        ecs::entity_t get_prev_sibling() const { return prev_sibling; }

      private:
        friend struct transform_system::internal_t;

        // what the local matrix was built from, to detect moves; the rotation unaligned keeps it at 112 bytes
        vec3 built_position;
        f32 built_rotation[4] = {};
        vec3 built_scale;

        ecs::entity_t parent = ecs::NULL_ENTITY;
        ecs::entity_t first_child = ecs::NULL_ENTITY;
        ecs::entity_t next_sibling = ecs::NULL_ENTITY;
        ecs::entity_t prev_sibling = ecs::NULL_ENTITY;

        // row in the transform table, none until the next update places it
        u32_t row = ~0u;
        bool is_dirty = true;
        bool has_shown = false;
    };
} // namespace tau
