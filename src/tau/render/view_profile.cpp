#include "tau/render/view_profile.h"

#include "tau/assets/data.h"
#include "tau/components/reflect.h"
#include "tau/reflection.h"

#include <cmath>

namespace tau::view_profile
{
    f32 exposure(const view_profile_t& profile)
    {
        const f32 compensation = std::exp2(profile.compensation);
        if (profile.exposure_mode == exposure_mode_e::AUTO) { return compensation; }
        // the saturation based sensitivity: a scene at ev100 maps its maximum to 1
        return compensation / (1.2f * std::exp2(profile.ev100));
    }
} // namespace tau::view_profile

namespace tau::reflection
{
    namespace
    {
        bool is_manual(const view_profile_t& p) { return p.exposure_mode == exposure_mode_e::MANUAL; }
        bool has_vignette(const view_profile_t& p) { return p.vignette > 0.0f; }
    } // namespace

    void reflect_view_profile(ctx_t& ctx)
    {
        enumeration<exposure_mode_e>(ctx, "Exposure Mode");
        enumeration<tonemap_e>(ctx, "Tonemap");

        data_type<view_profile_t>(ctx, "View Profile")
            .partial()
            .field<&view_profile_t::exposure_mode>("Exposure")
            .field<&view_profile_t::ev100>("EV100")
            .visible_if<&is_manual>()
            .field<&view_profile_t::compensation>("Compensation (EV)")
            .field<&view_profile_t::tonemap>("Tonemap")
            .field<&view_profile_t::contrast>("Contrast")
            .field<&view_profile_t::saturation>("Saturation")
            .field<&view_profile_t::vignette>("Vignette")
            .field<&view_profile_t::vignette_radius>("Vignette Radius")
            .visible_if<&has_vignette>()
            .field<&view_profile_t::grain>("Grain");
    }
} // namespace tau::reflection
