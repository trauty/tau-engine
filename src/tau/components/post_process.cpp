#include "tau/components/post_process.h"

#include "tau/assets/material.h"
#include "tau/components/reflect.h"
#include "tau/reflection.h"

namespace tau
{
    void post_process_t::on_added()
    {
        if (final_pass.is_valid()) { return; }

        final_pass = assets::load<material_t>(DEFAULT_FINAL_PASS_PATH);
    }
} // namespace tau

namespace tau::reflection
{
    void reflect_post_process(ctx_t& ctx)
    {
        component<post_process_t>(ctx, "Post Process")
            .field<&post_process_t::enabled>("Enabled")
            .field<&post_process_t::effects>("Effects")
            .field<&post_process_t::final_pass>("Final Pass");

        component<post_volume_t>(ctx, "Post Volume")
            .field<&post_volume_t::overrides>("Overrides")
            .field<&post_volume_t::is_global>("Global")
            .field<&post_volume_t::extents>("Extents")
            .field<&post_volume_t::blend_distance>("Blend Distance")
            .field<&post_volume_t::weight>("Weight")
            .field<&post_volume_t::priority>("Priority");
    }
} // namespace tau::reflection
