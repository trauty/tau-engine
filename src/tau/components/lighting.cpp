#include "tau/components/lighting.h"

#include "tau/components/reflect.h"
#include "tau/reflection.h"

namespace tau::reflection
{
    void reflect_lighting(ctx_t& ctx)
    {
        component<directional_light_t>(ctx, "Directional Light")
            .field<&directional_light_t::color>("Color")
            .field<&directional_light_t::intensity>("Intensity")
            .field<&directional_light_t::casts_shadow>("Casts Shadow");

        component<point_light_t>(ctx, "Point Light")
            .field<&point_light_t::color>("Color")
            .field<&point_light_t::intensity>("Intensity")
            .field<&point_light_t::radius>("Radius")
            .field<&point_light_t::casts_shadow>("Casts Shadow");

        component<spot_light_t>(ctx, "Spot Light")
            .field<&spot_light_t::color>("Color")
            .field<&spot_light_t::intensity>("Intensity")
            .field<&spot_light_t::radius>("Radius")
            .field<&spot_light_t::inner_angle>("Inner Angle")
            .field<&spot_light_t::outer_angle>("Outer Angle")
            .field<&spot_light_t::casts_shadow>("Casts Shadow");
    }
} // namespace tau::reflection
