#include "tau/components/physics.h"

#include "tau/components/reflect.h"
#include "tau/reflection.h"

namespace tau::reflection
{
    void reflect_physics(ctx_t& ctx)
    {
        enumeration<body_type_e>(ctx, "Body Type");

        component<rigidbody_t>(ctx, "Rigidbody")
            .field<&rigidbody_t::type>("Body Type")
            .field<&rigidbody_t::is_sensor>("Is Sensor")
            .field<&rigidbody_t::continuous>("Continuous")
            .field<&rigidbody_t::layer>("Layer")
            .field<&rigidbody_t::mass>("Mass")
            .field<&rigidbody_t::friction>("Friction")
            .field<&rigidbody_t::restitution>("Restitution")
            .field<&rigidbody_t::linear_damping>("Linear Damping")
            .field<&rigidbody_t::angular_damping>("Angular Damping")
            .field<&rigidbody_t::gravity_scale>("Gravity Scale")
            .field<&rigidbody_t::interpolate>("Interpolate");

        component<box_collider_t>(ctx, "Box Collider")
            .field<&box_collider_t::extents>("Extents")
            .field<&box_collider_t::offset>("Offset");

        component<sphere_collider_t>(ctx, "Sphere Collider")
            .field<&sphere_collider_t::radius>("Radius")
            .field<&sphere_collider_t::offset>("Offset");

        component<capsule_collider_t>(ctx, "Capsule Collider")
            .field<&capsule_collider_t::radius>("Radius")
            .field<&capsule_collider_t::height>("Height")
            .field<&capsule_collider_t::offset>("Offset");
    }
} // namespace tau::reflection
