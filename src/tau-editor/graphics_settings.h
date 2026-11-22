#pragma once

namespace tau
{
    struct world_t;
    struct editor_context_t;
} // namespace tau

// the live graphics settings: preset and values, not saved
namespace tau::editor::graphics_settings { void draw(world_t& world, editor_context_t& ctx); }
