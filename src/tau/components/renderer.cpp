#include "tau/components/renderer.h"

#include "tau/assets/material.h"
#include "tau/assets/mesh.h"
#include "tau/components/reflect.h"
#include "tau/reflection.h"

namespace tau
{
    asset_t<material_t> slot_material(const mesh_renderer_t& renderer, const mesh_t* mesh, u32_t submesh)
    {
        if (submesh < renderer.materials.size() && renderer.materials[submesh].is_valid())
        {
            return renderer.materials[submesh];
        }
        if (mesh && submesh < mesh->submeshes.size()) { return mesh->submeshes[submesh].material; }
        return {};
    }
} // namespace tau

namespace tau::reflection
{
    void reflect_mesh_renderer(ctx_t& ctx)
    {
        component<mesh_renderer_t>(ctx, "Mesh Renderer")
            .field<&mesh_renderer_t::mesh>("Mesh")
            .field<&mesh_renderer_t::materials>("Materials")
            .field<&mesh_renderer_t::active>("Active")
            .field<&mesh_renderer_t::casts_shadow>("Casts Shadow");
    }
} // namespace tau::reflection
