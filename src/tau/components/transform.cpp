#include "tau/components/transform.h"

#include "tau/components/tag.h"
#include "tau/components/reflect.h"
#include "tau/reflection.h"

namespace tau::reflection
{
    namespace
    {
        vec3_t get_transform_rot_euler(const tau::transform_t& transform)
        { return quat_t::to_euler(transform.local_rotation); }

        void set_transform_rot_euler(tau::transform_t& transform, const vec3_t& euler)
        { transform.local_rotation = quat_t::from_euler(euler); }
    } // namespace

    void reflect_transform(ctx_t& ctx)
    {
        component<tag_t>(ctx, "Tag").field<&tag_t::name>("Entity Name");

        component<transform_t>(ctx, "Transform")
            .field<&transform_t::local_position>("Position")
            .accessor<&set_transform_rot_euler, &get_transform_rot_euler>("Rotation", unit_e::RADIANS,
                                                                          stable_id{"local_euler"})
            .field<&transform_t::local_scale>("Scale");
    }
} // namespace tau::reflection
