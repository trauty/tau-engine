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

    // non-zero: every update advances by exactly this, not the wall clock (repeatable runs), 0 = wall clock
    TAU_ENGINE_API void set_frame_step(f64 seconds);
} // namespace tau::time