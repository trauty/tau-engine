#pragma once

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/EActivation.h>
// clang-format on

#include "tau/components/physics.h"
#include "tau/components/transform.h"
#include "tau/ecs_fwd.h"
#include "tau/math.h"
#include "tau/physics/physics_world.h"

namespace tau::physics
{
    inline void sync_to_physics(ecs::registry_t& reg, physics_world_t& physics_world)
    {
        JPH::BodyInterface& body_interface = physics_world.system.GetBodyInterface();

        for (auto [entity, rb, transform] : reg.view<rigidbody_t, transform_t>().each())
        {
            if (rb.type == body_type_e::KINEMATIC)
            {
                JPH::Vec3 pos = {transform.local_position.x, transform.local_position.y, transform.local_position.z};
                JPH::Quat rot = {transform.local_rotation.x, transform.local_rotation.y, transform.local_rotation.z,
                                 transform.local_rotation.w};

                body_interface.SetPositionAndRotation(rb.body_id, pos, rot, JPH::EActivation::Activate);
            }
        }
    }

    inline void sync_from_physics(ecs::registry_t& reg, physics_world_t& physics_world)
    {
        JPH::BodyInterface& body_interface = physics_world.system.GetBodyInterface();

        for (auto [entity, rb, transform] : reg.view<rigidbody_t, transform_t>().each())
        {
            if (rb.type != body_type_e::KINEMATIC)
            {
                JPH::Vec3 pos;
                JPH::Quat rot;

                body_interface.GetPositionAndRotation(rb.body_id, pos, rot);

                transform.local_position.x = pos.GetX();
                transform.local_position.y = pos.GetY();
                transform.local_position.z = pos.GetZ();

                transform.local_rotation.x = rot.GetX();
                transform.local_rotation.y = rot.GetY();
                transform.local_rotation.z = rot.GetZ();
                transform.local_rotation.w = rot.GetW();
            }
        }
    }
} // namespace tau::physics