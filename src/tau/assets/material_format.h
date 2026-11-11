#pragma once

#include "tau/defines.h"

namespace tau
{
    constexpr u32_t TAU_MATERIAL_MAGIC = 0x54414d54; // "TMAT"
    constexpr u32_t TAU_MATERIAL_VERSION = 2;

    // references: guid + path at cook time
    struct material_header_t
    {
        u32_t magic = TAU_MATERIAL_MAGIC;
        u32_t version = TAU_MATERIAL_VERSION;
        u64_t shader_guid = 0;
        u32_t shader_path_length = 0;
        u32_t texture_count = 0;
        u32_t sampler_count = 0;
        u32_t property_count = 0;
    };

    struct cooked_texture_bind_t
    {
        u32_t name_hash = 0;
        u32_t path_length = 0;
        u64_t guid = 0;
    };

    struct cooked_sampler_bind_t
    {
        u32_t name_hash;
        u32_t sampler_value;
    };

    struct cooked_property_t
    {
        u32_t name_hash;
        u32_t data_size;
    };
} // namespace tau