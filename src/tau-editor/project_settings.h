#pragma once

namespace tau
{
    struct world_t;
    struct editor_context_t;
} // namespace tau

// settings stored in the open project's .tauproject
namespace tau::editor::project_settings { void draw(world_t& world, editor_context_t& ctx); }
