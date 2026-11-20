#pragma once

#include "tau/defines.h"
#include "tau/math.h"

// header, submesh records (each followed by its material path and name), vertex_t[vertex_count],
// u32 indices[index_count], stream records (each followed by its name and data)
namespace tau
{
    constexpr u32_t TAU_MESH_MAGIC = 0x48534d54; // "TMSH"
    constexpr u32_t TAU_MESH_VERSION = 2;

    struct mesh_header_t
    {
        u32_t magic = TAU_MESH_MAGIC;
        u32_t version = TAU_MESH_VERSION;
        u32_t vertex_count = 0;
        u32_t index_count = 0;
        u32_t submesh_count = 0;
        u32_t stream_count = 0;
        vec3_t local_center;
        f32 local_radius = 0.0f;
    };

    struct mesh_submesh_record_t
    {
        u32_t first_index = 0;
        u32_t index_count = 0;
        vec3_t local_center;
        f32 local_radius = 0.0f;

        u64_t material_guid = 0;
        u32_t material_path_length = 0;
        u32_t material_name_length = 0;
    };

    // data the renderer does not read yet, per vertex unless noted
    enum class mesh_stream_e : u32_t
    {
        TANGENT,        // f32 x4
        UV1,            // f32 x2
        COLOR,          // f32 x4
        JOINTS,         // u16 x4
        WEIGHTS,        // f32 x4
        MORPH_POSITION, // f32 x3 delta, one stream per target, named after it
        MORPH_NORMAL,   // f32 x3 delta
        MORPH_WEIGHTS,  // f32 per target, the mesh's default weights
    };

    struct mesh_stream_record_t
    {
        mesh_stream_e kind = mesh_stream_e::TANGENT;
        u32_t element_size = 0;
        u32_t count = 0;
        u32_t name_length = 0;
    };
} // namespace tau
