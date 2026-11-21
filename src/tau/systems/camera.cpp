#include "camera.h"

#include "tau/components/camera.h"
#include "tau/components/transform.h"
#include "tau/math.h"
#include "tau/rendering/renderer.h"

namespace tau::camera_system
{
    void set_fov(ecs::registry_t& reg, ecs::entity_t entity, f32 fov_deg)
    {
        if (camera_t* cam = reg.try_get<camera_t>(entity))
        {
            cam->fov_deg = fov_deg;
            cam->is_dirty = true;
        }
    }

    ecs::entity_t get_active_camera(ecs::registry_t& reg)
    {
        auto view = reg.view<active_camera_tag>();
        return view.empty() ? ecs::NULL_ENTITY : view.front();
    }

    void update(ecs::registry_t& reg)
    {
        u32_t cur_width = tau::renderer::ctx.logical_extent.width;
        u32_t cur_height = tau::renderer::ctx.logical_extent.height;

        if (cur_width == 0 || cur_height == 0) { return; }

        f32 cur_aspect = static_cast<f32>(cur_width) / static_cast<f32>(cur_height);

        for (auto [entity, cam, transform] : reg.view<camera_t, transform_t>().each())
        {
            if (std::abs(cam.aspect - cur_aspect) > 0.001f)
            {
                cam.aspect = cur_aspect;
                cam.is_dirty = true;
            }

            vec3_t eye = {transform.world_mat[3][0], transform.world_mat[3][1], transform.world_mat[3][2]};
            vec3_t forward = {transform.world_mat[2][0], transform.world_mat[2][1], transform.world_mat[2][2]};
            vec3_t up = {transform.world_mat[1][0], transform.world_mat[1][1], transform.world_mat[1][2]};

            build_view_projection(eye, forward, up, cam.fov_deg, cam.aspect, cam.near_plane, cam.far_plane, cam.view,
                                  cam.projection, cam.view_proj);
            cam.is_dirty = false;
        }
    }

    void build_view_projection(vec3_t eye, vec3_t forward, vec3_t up, f32 fov_deg, f32 aspect, f32 near_plane,
                               f32 far_plane, mat4_t& out_view, mat4_t& out_projection, mat4_t& out_view_proj)
    {
        (void)far_plane;
        out_projection = perspective_reverse_infinite_lh_zo(radians(fov_deg), aspect, near_plane);
        flip_clip_y(out_projection);

        out_view = look_at_lh(eye, eye + forward, up);
        out_view_proj = out_projection * out_view;
    }
} // namespace tau::camera_system