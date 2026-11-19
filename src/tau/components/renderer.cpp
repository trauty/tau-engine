#include "tau/components/renderer.h"

#include "tau/assets/material.h"
#include "tau/assets/mesh.h"
#include "tau/components/reflect.h"
#include "tau/reflection.h"

namespace tau::reflection
{
    void reflect_mesh_renderer(ctx_t& ctx)
    {
        component<mesh_renderer_t>(ctx, "Mesh Renderer")
            .field<&mesh_renderer_t::mesh>("Mesh")
            .field<&mesh_renderer_t::material>("Material")
            .field<&mesh_renderer_t::active>("Active")
            .field<&mesh_renderer_t::casts_shadow>("Casts Shadow");
    }
} // namespace tau::reflection
