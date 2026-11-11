#pragma once

#include "tau/asset.h"

namespace tau
{
    struct mesh_t;
    struct material_t;

    struct TAU_ENGINE_API mesh_renderer_t
    {
        asset_t<mesh_t> mesh;
        asset_t<material_t> material;

        bool active = true;
        bool casts_shadow = true;
    };
} // namespace tau