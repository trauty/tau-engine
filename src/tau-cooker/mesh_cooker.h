#pragma once

#include "tau-cooker/model_desc.h"
#include "tau/asset.h"

#include <span>
#include <string>

namespace tau::cooker::mesh
{
    struct submesh_source_t
    {
        const model::primitive_desc_t* primitive = nullptr;
        asset_guid_t material_guid = 0;
        std::string material_path;
        std::string material_name;
    };

    bool write_mesh(const model::mesh_desc_t& mesh, std::span<const submesh_source_t> submeshes,
                    const std::string& output_path);
} // namespace tau::cooker::mesh
