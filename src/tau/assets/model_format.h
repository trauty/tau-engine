#pragma once

#include "tau/defines.h"

namespace tau
{
    constexpr u32_t TAU_MODEL_MAGIC = 0x4c444d54; // "TMDL"
    constexpr u32_t TAU_MODEL_VERSION = 1;

    struct model_header_t
    {
        u32_t magic = TAU_MODEL_MAGIC;
        u32_t version = TAU_MODEL_VERSION;
        u32_t sub_asset_count = 0;
    };

    struct model_sub_asset_record_t
    {
        u64_t type_id = 0;
        u64_t guid = 0;
        u32_t name_length = 0;
        u32_t _pad = 0;
    };
} // namespace tau
