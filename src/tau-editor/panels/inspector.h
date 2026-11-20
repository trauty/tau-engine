#pragma once

#include "tau-editor/editor_context.h"
#include "tau/reflection.h"
#include "tau/world.h"

namespace tau::editor::panels
{
    void draw_inspector(world_t& world, editor_context_t& ctx);
    bool draw_fields(world_t& world, reflection::any_t& value);
} // namespace tau::editor::panels