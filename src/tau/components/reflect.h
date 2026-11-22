#pragma once

#include "tau/reflection_fwd.h"

namespace tau::reflection
{
    void reflect_transform(ctx_t& ctx);
    void reflect_camera(ctx_t& ctx);
    void reflect_light(ctx_t& ctx);
    void reflect_view_profile(ctx_t& ctx);
    void reflect_profile_volume(ctx_t& ctx);
    void reflect_physics(ctx_t& ctx);
    void reflect_mesh_renderer(ctx_t& ctx);
    void reflect_graphics_settings(ctx_t& ctx);
} // namespace tau::reflection
