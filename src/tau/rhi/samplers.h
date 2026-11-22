#pragma once

#include "tau/defines.h"
#include "tau/rhi/shader_shared.h"

namespace tau
{
    // the slots themselves are shared with the shaders; see shader_shared.h
    enum class sampler_type_e : u32_t
    {
        LINEAR_REPEAT = TAU_SAMPLER_LINEAR_REPEAT,
        LINEAR_CLAMP = TAU_SAMPLER_LINEAR_CLAMP,
        NEAREST_REPEAT = TAU_SAMPLER_NEAREST_REPEAT,
        NEAREST_CLAMP = TAU_SAMPLER_NEAREST_CLAMP,
        SHADOW_CMP = TAU_SAMPLER_SHADOW_CMP, // depth comparison sampler, drives hardware pcf
        COUNT = TAU_SAMPLER_COUNT,
    };
} // namespace tau