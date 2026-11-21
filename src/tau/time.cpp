#include "time.h"

#include "tau/defines.h"

#include <SDL3/SDL_timer.h>

namespace tau::time
{
    f64 time = 0.0;
    f64 dt = 0.0;
    f64 fixed_dt = 0.0;
    f64 fixed_alpha = 0.0;

    static u64_t start_ticks = 0;
    static u64_t last_ticks = 0;
    static f64 frame_step = 0.0;

    void set_frame_step(f64 seconds) { frame_step = seconds; }

    void update()
    {
        const u64_t cur_ticks = SDL_GetTicksNS();

        if (start_ticks == 0)
        {
            start_ticks = cur_ticks;
            last_ticks = cur_ticks;
        }

        constexpr f64 NS_TO_SECONDS = 1.0 / 1000000000.0;

        if (frame_step > 0.0)
        {
            time += frame_step;
            dt = frame_step;
        }
        else
        {
            time = static_cast<f64>(cur_ticks - start_ticks) * NS_TO_SECONDS;
            dt = static_cast<f64>(cur_ticks - last_ticks) * NS_TO_SECONDS;
        }

        last_ticks = cur_ticks;
    }

    f64 get_time() { return time; }
    f64 get_dt() { return dt; }
    f64 get_fixed_dt() { return fixed_dt; }
    f64 get_fixed_alpha() { return fixed_alpha; }
} // namespace tau::time