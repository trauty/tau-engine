#pragma once

#include "tau/reflection_fwd.h"

namespace tau::reflection
{
    void reflect_transform(ctx_t& ctx);
    void reflect_camera(ctx_t& ctx);
    void reflect_lighting(ctx_t& ctx);
    void reflect_physics(ctx_t& ctx);
    void reflect_post_process(ctx_t& ctx);
    void reflect_mesh_renderer(ctx_t& ctx);
} // namespace tau::reflection
