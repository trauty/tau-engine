#include "tau/components/transform.h"

#include "tau/components/reflect.h"
#include "tau/components/tag.h"
#include "tau/reflection.h"

namespace tau::reflection
{
    namespace
    {
        vec3_t get_transform_rot_euler(const tau::transform_t& transform)
        { return euler_xyz(transform.local_rotation); }

        void set_transform_rot_euler(tau::transform_t& transform, const vec3_t& euler)
        { transform.local_rotation = quat_from_euler_xyz(euler); }
    } // namespace

    void reflect_transform(ctx_t& ctx)
    {
        component<tag_t>(ctx, "Tag").field<&tag_t::name>("Entity Name");

        component<transform_t>(ctx, "Transform")
            .field<&transform_t::local_position>("Position")
            .field<&transform_t::local_rotation>("Rotation", prop_flags_e::HIDDEN)
            .accessor<&set_transform_rot_euler, &get_transform_rot_euler>(
                "Rotation", unit_e::RADIANS, stable_id{"local_euler"}, prop_flags_e::NOT_SAVED)
            .field<&transform_t::local_scale>("Scale");
    }
} // namespace tau::reflection
