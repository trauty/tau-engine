#pragma once

#include "tau/math.h"

namespace tau
{
    struct world_t;
    struct editor_context_t;
} // namespace tau

namespace tau::editor::viewport_input
{
    struct ray_t
    {
        vec3_t origin;
        vec3_t direction;
    };

    ray_t ray_through(const editor_context_t& ctx, vec2_t pixel);
    bool mouse_pixel(const editor_context_t& ctx, vec2_t& out);
    bool to_pixel(const editor_context_t& ctx, vec3_t world, vec2_t& out);
    bool mouse_ray(const editor_context_t& ctx, ray_t& out);

    // RMB: camera, LMB: handles, then selection
    void update(world_t& world, editor_context_t& ctx);

    void frame_selection(world_t& world, editor_context_t& ctx);
} // namespace tau::editor::viewport_input
