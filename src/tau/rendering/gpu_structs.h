#pragma once

// GPU records, defined once for C++ and Slang
// Slang: only tau_gpu.slang (a module without a `module` line, so everything is public) includes this,
// tau_globals re-exports it; including it in a second module would make distinct, incompatible types
// shaders compile with scalar layout: a struct's C++ size equals its Slang size only if C++ adds no padding,
// so the sizes in TAU_GPU_STRUCT_SIZES pin the field offsets too (C++ static_asserts, the cooker checks Slang)
// matrices are row-major on the GPU and shaders write mul(M, v): float4x4 is built from a column-major mat4

#ifdef __cplusplus
    #include "tau/math.h"

    #include <cstdint>

namespace tau::gpu
{
    using uint = u32_t;
    using float2 = vec2;
    using float3 = vec3;
    using float4 = vec4;
    using float3x4 = mat3x4;

    struct alignas(16) float4x4
    {
        vec4 rows[4];

        float4x4() = default;
        float4x4(const mat4& m)
        {
            for (int r = 0; r < 4; r++) { rows[r] = {m[0][r], m[1][r], m[2][r], m[3][r]}; }
        }
    };
#endif

    struct PushConstants
    {
        uint64_t object_buffer;
        uint64_t material_buffer;
        uint custom_data;
        uint texture_id;
    };

    // what the draw path reads: one per object, fetched by the vertex shader through the object buffer address
    // everything culling needs lives in CullData, indexed alike so object i and cull input i are the same object
    struct ObjectData
    {
        float4x4 model_matrix;
        float4x4 normal_matrix;
        uint64_t vertex_buffer;
        uint64_t index_buffer;
        uint material_offset;
        uint entity;
        uint _pad1;
        uint _pad2;
    };

    // kept apart from the 160 byte record so several objects fit per cache line
    struct CullData
    {
        float4 bounding_sphere; // xyz = world centre, w = radius
        uint index_count;
        uint pipeline_index;
        uint flags;           // bit0 = shadow caster
        uint draw_bin_offset; // start of this pipeline's run in the indirect buffer
    };

    // one tile of a shadow atlas: a cascade and a punctual tile are the same record, only split_far is cascade specific
    struct ShadowTile
    {
        float4x4 view_proj;
        float4 uv_offset_scale; // xy = atlas uv offset, zw = atlas uv scale
        float texel_world_size; // world units per shadow texel, scales the normal offset
        float normal_bias;      // in shadow texels
        float depth_bias;       // in light space ndc depth
        float split_far;        // view space distance at which this cascade stops
    };

    struct DirectionalLight
    {
        float4 direction_intensity; // xyz = dir, w = intensity
        float4 color;               // xyz = color, w = padding
    };

    struct PointLight
    {
        float4 position_radius; // xyz = pos, w = radius
        float4 color_intensity; // xyz = color, w = intensity
        float4 shadow_params;   // x = the punctual shadow tile this light owns, negative when it casts none
    };

    struct SpotLight
    {
        float4 position_range;      // xyz = pos, w = range
        float4 direction_intensity; // xyz = dir, w = intensity
        float4 color_inner_cos;     // xyz = color, w = inner_cosine
        float4 outer_cos;           // x = outer_cosine, yzw = padding
        float4 shadow_params;       // x = the punctual shadow tile this light owns, negative when it casts none
    };

    struct GlobalData
    {
        float4x4 view;
        float4x4 projection;
        float4x4 view_proj;
        float4 camera_pos;
        float4 frustum_planes[6];
        float4x4 inv_view_proj; // reconstruct the world position from depth

        uint64_t dir_light_buffer;
        uint64_t point_light_buffer;
        uint64_t spot_light_buffer;
        uint64_t cluster_counts_buffer;  // one uint per cluster
        uint64_t cluster_indices_buffer; // TAU_MAX_LIGHTS_PER_CLUSTER uints per cluster
        uint64_t shadow_cascade_buffer;

        float time;
        uint dir_light_count;
        uint point_light_count;
        uint spot_light_count;
        uint object_count;
        uint active_pipeline_count;
        uint cull_flags; // bit0 = shadow only view

        uint debug_view;

        uint shadow_atlas_id; // bindless id of the cascade atlas
        uint shadow_cascade_count;
        float shadow_fade_start; // view distance where shadows start fading out
        float shadow_fade_end;   // and where they are fully faded

        float cluster_z_near;
        float cluster_z_far;
        uint cluster_tiles_x;
        uint cluster_tiles_y;

        uint64_t punctual_shadow_buffer;
        uint punctual_shadow_atlas_id;
        uint punctual_shadow_count;
    };

    struct OverlayItem
    {
        float4x4 model;
        float4 color;
        uint64_t vertices;
        uint64_t indices;
        uint mode; // 0 mesh, 1 lines
        float line_width;
        float2 viewport_size;
    };

    struct OverlayLine
    {
        float4 a;
        float4 b;
        float4 color;
    };

#define TAU_GPU_STRUCT_SIZES(X)                                                                                        \
    X(PushConstants, 24)                                                                                               \
    X(ObjectData, 160)                                                                                                 \
    X(CullData, 32)                                                                                                    \
    X(ShadowTile, 96)                                                                                                  \
    X(DirectionalLight, 32)                                                                                            \
    X(PointLight, 48)                                                                                                  \
    X(SpotLight, 80)                                                                                                   \
    X(GlobalData, 496)                                                                                                 \
    X(OverlayItem, 112)                                                                                                \
    X(OverlayLine, 48)

#ifdef __cplusplus
    #define TAU_GPU_SIZE_CHECK(T, n)                                                                                   \
        static_assert(sizeof(T) == n, #T " is not " #n " bytes: C++ padding or a changed record, see gpu_structs.h");
    TAU_GPU_STRUCT_SIZES(TAU_GPU_SIZE_CHECK)
    #undef TAU_GPU_SIZE_CHECK
} // namespace tau::gpu
#endif
