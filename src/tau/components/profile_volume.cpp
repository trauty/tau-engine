#include "tau/components/profile_volume.h"

#include "tau/assets/material.h"
#include "tau/components/reflect.h"
#include "tau/reflection.h"

namespace tau::reflection
{
    namespace
    {
        bool is_local(const profile_volume_t& v) { return !v.is_global; }
    } // namespace

    void reflect_profile_volume(ctx_t& ctx)
    {
        component<profile_volume_t>(ctx, "Profile Volume")
            .field<&profile_volume_t::profile>("Profile")
            .field<&profile_volume_t::effect_overrides>("Effect Overrides")
            .field<&profile_volume_t::is_global>("Global")
            .field<&profile_volume_t::extents>("Extents")
            .visible_if<&is_local>()
            .field<&profile_volume_t::blend_distance>("Blend Distance")
            .visible_if<&is_local>()
            .field<&profile_volume_t::weight>("Weight")
            .field<&profile_volume_t::priority>("Priority");
    }
} // namespace tau::reflection
