#pragma once

#include "tau-editor/editor_context.h"
#include "tau/world.h"

namespace tau::editor::panels
{
    // drag payload: asset vfs path, null terminated
    constexpr const char* ASSET_PAYLOAD = "TAU_ASSET";

    void draw_asset_browser(world_t& world, editor_context_t& ctx);
} // namespace tau::editor::panels
