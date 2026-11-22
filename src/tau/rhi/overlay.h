#pragma once

#include "tau/asset.h"
#include "tau/defines.h"
#include "tau/math.h"

#include <span>

namespace tau { struct mesh_t; }

namespace tau::renderer
{
    enum class overlay_depth_e : u8_t
    {
        SCENE,
        ON_TOP,
    };

    struct overlay_line_t
    {
        vec3 a;
        vec3 b;
        vec4 color;
    };

    TAU_ENGINE_API void submit_overlay_mesh(u32_t view_hash, asset_t<mesh_t> mesh, const mat4& world, vec4 color,
                                            overlay_depth_e depth);
    TAU_ENGINE_API void submit_overlay_lines(u32_t view_hash, std::span<const overlay_line_t> lines, f32 width_px,
                                             overlay_depth_e depth);

    namespace overlay
    {
        void init();
        void shutdown();
    } // namespace overlay
} // namespace tau::renderer
