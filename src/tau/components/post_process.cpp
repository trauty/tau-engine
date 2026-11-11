#include "tau/components/post_process.h"

#include "tau/assets/material.h"

namespace tau
{
    void post_process_t::on_added()
    {
        if (final_pass.is_valid()) { return; }

        final_pass = assets::load<material_t>(DEFAULT_FINAL_PASS_PATH);
    }
} // namespace tau
