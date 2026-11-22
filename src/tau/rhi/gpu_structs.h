#pragma once

// GPU records for C++ and Slang
// Slang: included only by tau_gpu.slang (a second include = distinct types)
// scalar layout: equal sizes on both sides pin the offsets
// row-major, shaders write mul(M, v)

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

    // indexed like CullData
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

    struct CullData
    {
        float4 bounding_sphere; // xyz = world centre, w = radius
        uint index_count;
        uint pipeline_index;
        uint flags;           // bit0 = shadow caster
        uint draw_bin_offset; // pipeline's first indirect slot
    };

    // cascade or punctual tile, split_far cascades only
    struct ShadowTile
    {
        float4x4 view_proj;
        float4 uv_offset_scale; // xy = atlas uv offset, zw = atlas uv scale
        float texel_world_size; // world units, scales the normal offset
        float normal_bias;      // in shadow texels
        float depth_bias;       // in light space ndc depth
        float split_far;        // view distance where the cascade ends
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
        float4 shadow_params;   // x = punctual tile, < 0 none
    };

    struct SpotLight
    {
        float4 position_range;      // xyz = pos, w = range
        float4 direction_intensity; // xyz = dir, w = intensity
        float4 color_inner_cos;     // xyz = color, w = inner_cosine
        float4 outer_cos;           // x = outer_cosine, yzw = padding
        float4 shadow_params;       // x = punctual tile, < 0 none
    };

    struct GlobalData
    {
        float4x4 view;
        float4x4 projection;
        float4x4 view_proj;
        float4 camera_pos;
        float4 frustum_planes[6];
        float4x4 inv_view_proj;

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
        float shadow_fade_start; // view distance, fade start
        float shadow_fade_end;   // fade end

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