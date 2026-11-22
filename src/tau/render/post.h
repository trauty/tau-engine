#pragma once

#include "tau/defines.h"
#include "tau/ecs_fwd.h"
#include "tau/math.h"
#include "tau/render/frame.h"

// post effect chain (camera stack, post volumes) and the final pass into the output
namespace tau::renderer::post
{
    // pins the final pass
    void init();
    void shutdown();

    void build(graph::graph_t& g, const frame::targets_t& t, ecs::registry_t& reg, u32_t view_hash,
               const vec3& view_pos, bool enabled);
} // namespace tau::renderer::post
