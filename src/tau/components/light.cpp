#include "tau/components/light.h"

#include "tau/components/reflect.h"
#include "tau/reflection.h"

namespace tau::reflection
{
    namespace
    {
        bool has_range(const light_t& l) { return l.type != light_type_e::SUN; }
        bool is_spot(const light_t& l) { return l.type == light_type_e::SPOT; }
        bool is_area(const light_t& l) { return l.type == light_type_e::RECT || l.type == light_type_e::TUBE; }
    } // namespace

    void reflect_light(ctx_t& ctx)
    {
        enumeration<light_type_e>(ctx, "Light Type");
        enumeration<light_shadows_e>(ctx, "Light Shadows");

        component<light_t>(ctx, "Light")
            .field<&light_t::type>("Type")
            .field<&light_t::color>("Color")
            .field<&light_t::intensity>("Intensity (lm, sun lux)")
            .field<&light_t::range>("Range")
            .visible_if<&has_range>()
            .field<&light_t::inner_angle>("Inner Angle")
            .visible_if<&is_spot>()
            .field<&light_t::outer_angle>("Outer Angle")
            .visible_if<&is_spot>()
            .field<&light_t::size>("Size")
            .visible_if<&is_area>()
            .field<&light_t::shadows>("Shadows")
            .field<&light_t::volumetric>("Volumetric");
    }
} // namespace tau::reflection
