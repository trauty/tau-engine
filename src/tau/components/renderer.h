#pragma once

#include "tau/asset.h"

#include <vector>

namespace tau
{
    struct mesh_t;
    struct material_t;

    struct TAU_ENGINE_API mesh_renderer_t
    {
        asset_t<mesh_t> mesh;
        // one per submesh, an empty slot draws the material the mesh brought
        std::vector<asset_t<material_t>> materials;

        bool active = true;
        bool casts_shadow = true;
    };

    TAU_ENGINE_API asset_t<material_t> slot_material(const mesh_renderer_t& renderer, const mesh_t* mesh,
                                                     u32_t submesh);
} // namespace tau
