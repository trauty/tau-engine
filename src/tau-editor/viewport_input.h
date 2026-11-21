#pragma once

#include "tau/ecs_fwd.h"
#include "tau/math.h"

#include <span>

namespace tau
{
    struct world_t;
    struct editor_context_t;
} // namespace tau

namespace tau::editor::viewport_input
{
    struct ray_t
    {
        vec3 origin;
        vec3 direction;
    };

    ray_t ray_through(const editor_context_t& ctx, vec2 pixel);
    bool mouse_pixel(const editor_context_t& ctx, vec2& out);
    bool to_pixel(const editor_context_t& ctx, vec3 world, vec2& out);
    bool mouse_ray(const editor_context_t& ctx, ray_t& out);

    // RMB: camera, LMB: handles, then selection
    void update(world_t& world, editor_context_t& ctx);

    void frame_selection(world_t& world, editor_context_t& ctx);
    void frame_entities(world_t& world, editor_context_t& ctx, std::span<const ecs::entity_t> entities);
} // namespace tau::editor::viewport_input
