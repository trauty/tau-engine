#include "tau/asset_types.h"
#include "tau/assets/material.h"
#include "tau/assets/mesh.h"
#include "tau/assets/prefab.h"
#include "tau/assets/scene.h"
#include "tau/assets/shader.h"
#include "tau/assets/texture.h"

namespace tau::assets
{
    void register_engine_types()
    {
        reg<mesh_t>({
            .display_name = "Mesh", .cooked_extension = ".taumesh", .source_extensions = {".gltf", ".glb"}
        });
        reg<material_t>({.display_name = "Material",
                         .cooked_extension = ".taumat",
                         .source_extensions = {".mat"},
                         .guid_in_source = true});
        reg<texture_t>({
            .display_name = "Texture", .cooked_extension = ".ktx2", .source_extensions = {".png", ".jpg", ".jpeg"}
        });
        reg<shader_t>({.display_name = "Shader", .cooked_extension = ".taushader", .source_extensions = {".slang"}});
        reg<scene_t>({.display_name = "Scene",
                      .cooked_extension = ".tauscene",
                      .source_extensions = {".scene"},
                      .guid_in_source = true});
        reg<prefab_t>({.display_name = "Prefab",
                       .cooked_extension = ".tauprefab",
                       .source_extensions = {".prefab"},
                       .guid_in_source = true});
    }
} // namespace tau::assets
