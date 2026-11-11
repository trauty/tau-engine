#pragma once

namespace tau
{
    struct world_t;
    struct editor_context_t;
} // namespace tau

namespace tau::editor::handles
{
    // input in update, drawing in a render feature
    void init(editor_context_t& ctx);
    void update(world_t& world, editor_context_t& ctx);
} // namespace tau::editor::handles
