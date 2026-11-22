#pragma once

#include "tau/defines.h"

// a view's image values: on the camera, in profile assets that volumes blend in
// fields arrive with the parts that read them (auto exposure range: part 6, grading and LUT: part 11)
namespace tau
{
    enum class exposure_mode_e : u8_t
    {
        MANUAL,
        AUTO,
    };

    enum class tonemap_e : u8_t
    {
        OFF,
        ACES,
        REINHARD,
    };

    struct TAU_ENGINE_API view_profile_t
    {
        exposure_mode_e exposure_mode = exposure_mode_e::AUTO;
        // manual exposure, EV at ISO 100
        f32 ev100 = 12.0f;
        // in EV, on top of either mode
        f32 compensation = 0.0f;
        tonemap_e tonemap = tonemap_e::ACES;
        f32 contrast = 1.0f;
        f32 saturation = 1.0f;
        // 0: off
        f32 vignette = 0.0f;
        f32 vignette_radius = 0.75f;
        // 0: off
        f32 grain = 0.0f;
    };

    namespace view_profile
    {
        // the linear multiplier on scene light; auto is 2^compensation until part 6 measures the scene
        TAU_ENGINE_API f32 exposure(const view_profile_t& profile);
    } // namespace view_profile
} // namespace tau
