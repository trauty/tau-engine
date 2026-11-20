#pragma once

#include "defines.h"

namespace tau::time
{
    extern f64 time;
    extern f64 dt;
    extern f64 fixed_dt;
    extern f64 fixed_alpha;

    void update();

    TAU_ENGINE_API f64 get_time();
    TAU_ENGINE_API f64 get_dt();
    TAU_ENGINE_API f64 get_fixed_dt();
    TAU_ENGINE_API f64 get_fixed_alpha();
} // namespace tau::time