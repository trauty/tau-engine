#pragma once

#include "tau-editor/editor_context.h"
#include "tau/world.h"

#include <string>

namespace tau::editor::panels
{
    void draw_main_menu_bar(world_t& world, editor_context_t& ctx);

    void request_action(world_t& world, editor_context_t& ctx, unsaved_action_e action, std::string project = {});
} // namespace tau::editor::panels
