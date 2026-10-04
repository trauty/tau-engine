#pragma once

#include "tau/defines.h"
#include "tau/ecs.h"
#include "tau/ecs_fwd.h"
#include "tau/math.h"

namespace tau
{
    namespace transform_system { struct internal_t; }

    struct TAU_ENGINE_API transform_t
    {
        vec3_t local_position;
        quat_t local_rotation;
        vec3_t local_scale = {1.0f, 1.0f, 1.0f};

        // read only, the transform system writes it
        mat4_t world_mat;

        ecs::entity_t get_parent() const { return parent; }
        ecs::entity_t get_first_child() const { return first_child; }
        ecs::entity_t get_next_sibling() const { return next_sibling; }

      private:
        friend struct transform_system::internal_t;

        mat4_t local_mat;

        // what local_mat was built from, to detect moves
        vec3_t built_position;
        quat_t built_rotation;
        vec3_t built_scale;

        ecs::entity_t parent = ecs::NULL_ENTITY;
        ecs::entity_t first_child = ecs::NULL_ENTITY;
        ecs::entity_t next_sibling = ecs::NULL_ENTITY;
        ecs::entity_t prev_sibling = ecs::NULL_ENTITY;

        bool is_dirty = true;
    };
} // namespace tau
