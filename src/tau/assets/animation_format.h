#pragma once

#include "tau/defines.h"

namespace tau
{
    constexpr u32_t TAU_SKELETON_MAGIC = 0x4c4b5354; // "TSKL"
    constexpr u32_t TAU_SKELETON_VERSION = 1;

    struct skeleton_header_t
    {
        u32_t magic = TAU_SKELETON_MAGIC;
        u32_t version = TAU_SKELETON_VERSION;
        u32_t joint_count = 0;
    };

    struct joint_record_t
    {
        // -1 for a root
        i32 parent = -1;
        f32 translation[3] = {};
        f32 rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        f32 scale[3] = {1.0f, 1.0f, 1.0f};
        // column major
        f32 inverse_bind[16] = {};
        u32_t name_length = 0;
    };

    constexpr u32_t TAU_CLIP_MAGIC = 0x504c4354; // "TCLP"
    constexpr u32_t TAU_CLIP_VERSION = 1;

    struct clip_header_t
    {
        u32_t magic = TAU_CLIP_MAGIC;
        u32_t version = TAU_CLIP_VERSION;
        f32 duration = 0.0f;
        u32_t channel_count = 0;
    };

    struct clip_channel_record_t
    {
        u8_t path = 0;
        u8_t interpolation = 0;
        u16_t _pad = 0;
        u32_t key_count = 0;
        u32_t value_count = 0;
        u32_t target_length = 0;
    };
} // namespace tau
