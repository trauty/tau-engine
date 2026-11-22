#include "tau/components/camera.h"

#include "tau/assets/material.h"
#include "tau/components/reflect.h"
#include "tau/reflection.h"

namespace tau::reflection
{
    void reflect_camera(ctx_t& ctx)
    {
        component<active_camera_tag>(ctx, "Active Camera Tag");

        component<camera_t>(ctx, "Camera")
            .field<&camera_t::fov_deg>("FOV")
            .field<&camera_t::near_plane>("Near Plane")
            .field<&camera_t::far_plane>("Far Plane")
            .field<&camera_t::aspect>("Aspect Ratio")
            .field<&camera_t::profile>("Profile")
            .field<&camera_t::effects>("Effects");
    }
} // namespace tau::reflection
