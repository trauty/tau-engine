#pragma once

#include <string>

namespace tau
{
    struct world_t;
    struct editor_context_t;
} // namespace tau

namespace tau::editor::scene_io
{
    bool save(world_t& world, editor_context_t& ctx, const std::string& path);
    bool load(world_t& world, editor_context_t& ctx, const std::string& path);
    void clear(world_t& world, editor_context_t& ctx);
} // namespace tau::editor::scene_io
