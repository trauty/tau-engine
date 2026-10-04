#pragma once

#include "tau/defines.h"

namespace tau
{
    constexpr u32_t TAU_SCENE_MAGIC = 0x4e435354; // "TSCN"
    constexpr u32_t TAU_SCENE_VERSION = 3;

    enum class scene_value_type_e : u8_t
    {
        I32 = 0,
        U32 = 1,
        F32 = 2,
        BOOL = 3,
        STRING = 4,
        VEC3 = 5,
        ASSET_REF = 6,
        ASSET_REF_LIST = 7,
        ENTITY = 8,
        STRUCT = 9,
        LIST = 10,
    };

    // ENTITY values are scene indices
    constexpr u32_t TAU_SCENE_NULL_ENTITY = 0xFFFFFFFF;

    struct scene_header_t
    {
        u32_t magic = TAU_SCENE_MAGIC;
        u32_t version = TAU_SCENE_VERSION;
        u32_t entity_count = 0;
    };
} // namespace tau
