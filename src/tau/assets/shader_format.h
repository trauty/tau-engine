#pragma once

#include "tau/defines.h"
namespace tau
{
    constexpr u32_t TAU_SHADER_MAGIC = 0x44485354; // "TSHD"
    constexpr u32_t TAU_SHADER_VERSION = 2;

    inline constexpr const char* SHADER_ENTRY_COMPUTE = "computeMain";
    inline constexpr const char* SHADER_ENTRY_VERTEX = "vertexMain";
    inline constexpr const char* SHADER_ENTRY_FRAGMENT = "fragmentMain";
    inline constexpr const char* SHADER_ENTRY_GBUFFER = "gbufferMain";

    enum class shader_domain_e : u32_t
    {
        SURFACE,
        POST_PROCESS,
        CUSTOM,
    };

    enum class blend_mode_e : u32_t
    {
        SOLID,
        CUTOUT,
        TRANSPARENT_ALPHA,
        TRANSPARENT_ADD,
        TRANSPARENT_MULT,
    };

    // where an opaque surface shader with gbufferMain draws: always the gbuffer
    // one rule for the renderer's routing (pass_for) and the cooker, which omits its forward fragment code
    constexpr bool draws_into_gbuffer(shader_domain_e domain, blend_mode_e blend, bool has_gbuffer_targets)
    {
        const bool opaque = blend == blend_mode_e::SOLID || blend == blend_mode_e::CUTOUT;
        return domain == shader_domain_e::SURFACE && opaque && has_gbuffer_targets;
    }

    enum class descriptor_type_e : u32_t
    {
        SAMPLER,
        SAMPLED_IMAGE,
        STORAGE_IMAGE,
        UNIFORM_BUFFER,
        STORAGE_BUFFER
    };

    struct shader_descriptor_binding_t
    {
        u32_t name_hash;
        u32_t set;
        u32_t binding;
        descriptor_type_e type;
        u32_t count;
    };

    struct shader_header_t
    {
        u32_t magic = TAU_SHADER_MAGIC;
        u32_t version = TAU_SHADER_VERSION;
        bool is_compute;
        bool has_material_data;
        u32_t vert_spirv_size;
        u32_t frag_spirv_size;
        u32_t comp_spirv_size;
        u32_t gbuffer_spirv_size;
        u32_t target_format_count;
        u32_t gbuffer_target_format_count;
        u32_t descriptor_binding_count;
    };

    struct shader_module_header_t
    {
        char name[64];
        u32_t size;
        shader_domain_e domain;
        blend_mode_e blend_mode;
        bool depth_write;
        bool depth_test;
        bool casts_shadow;
        u32_t member_count;
    };

    enum class shader_member_type_e : u32_t
    {
        UNKNOWN,
        FLOAT,
        FLOAT2,
        FLOAT3,
        FLOAT4,
        INT,
        UINT,
        BOOL,
    };

    enum class shader_member_edit_e : u32_t
    {
        DEFAULT,
        HIDDEN,
        TEXTURE,
        SAMPLER,
        COLOR,
        ENUM,
    };

    constexpr u32_t SHADER_MEMBER_ENUM_NAMES_MAX = 256;
    constexpr u32_t SHADER_MEMBER_TOOLTIP_MAX = 192;

    struct shader_member_header_t
    {
        u32_t name_hash;
        u32_t offset;
        u32_t size;
        shader_member_type_e type;
        char name[64];

        shader_member_edit_e edit;

        f32 range_min;
        f32 range_max;

        char enum_names[SHADER_MEMBER_ENUM_NAMES_MAX];
        char tooltip[SHADER_MEMBER_TOOLTIP_MAX];
    };

} // namespace tau