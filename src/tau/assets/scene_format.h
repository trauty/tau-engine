#pragma once

#include "tau/defines.h"

namespace tau
{
    constexpr u32_t TAU_SCENE_MAGIC = 0x4e435354; // "TSCN"
    constexpr u32_t TAU_SCENE_VERSION = 6;

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
        VEC2 = 11,
        VEC4 = 12,
        QUAT = 13,
        COLOR = 14,
        I64 = 15,
        U64 = 16,
        F64 = 17,
        ENUM = 18,
    };

    enum class scalar_e : u8_t
    {
        NONE,
        BOOL,
        I32,
        U32,
        I64,
        U64,
        F32,
        F64,
    };

    struct pod_layout_t
    {
        scalar_e scalar = scalar_e::NONE;
        u8_t count = 0;
    };

    constexpr u32_t MAX_POD_SIZE = 64;

    constexpr pod_layout_t pod_layout(scene_value_type_e type)
    {
        switch (type)
        {
        case scene_value_type_e::BOOL: return {scalar_e::BOOL, 1};
        case scene_value_type_e::I32: return {scalar_e::I32, 1};
        case scene_value_type_e::U32: return {scalar_e::U32, 1};
        case scene_value_type_e::I64: return {scalar_e::I64, 1};
        case scene_value_type_e::U64: return {scalar_e::U64, 1};
        case scene_value_type_e::F32: return {scalar_e::F32, 1};
        case scene_value_type_e::F64: return {scalar_e::F64, 1};
        case scene_value_type_e::VEC2: return {scalar_e::F32, 2};
        case scene_value_type_e::VEC3: return {scalar_e::F32, 3};
        case scene_value_type_e::VEC4:
        case scene_value_type_e::QUAT:
        case scene_value_type_e::COLOR: return {scalar_e::F32, 4};
        default: return {};
        }
    }

    constexpr u32_t scalar_size(scalar_e scalar)
    {
        switch (scalar)
        {
        case scalar_e::NONE: return 0;
        case scalar_e::BOOL: return 1;
        case scalar_e::I32:
        case scalar_e::U32:
        case scalar_e::F32: return 4;
        case scalar_e::I64:
        case scalar_e::U64:
        case scalar_e::F64: return 8;
        }
        return 0;
    }

    constexpr u32_t pod_size(pod_layout_t layout) { return scalar_size(layout.scalar) * layout.count; }

    // ENTITY values and parents are entity guids, 0 for none

    struct scene_header_t
    {
        u32_t magic = TAU_SCENE_MAGIC;
        u32_t version = TAU_SCENE_VERSION;
        u32_t entity_count = 0;
    };
} // namespace tau
