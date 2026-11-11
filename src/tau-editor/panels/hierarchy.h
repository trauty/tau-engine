#pragma once

#include "tau-editor/editor_context.h"
#include "tau/world.h"

namespace tau::editor::panels
{
    // drag payload: entity guid, stands for the selection if part of it
    constexpr const char* ENTITY_PAYLOAD = "TAU_ENTITIES";

    void draw_hierarchy(world_t& world, editor_context_t& ctx);
} // namespace tau::editor::panels