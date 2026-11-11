#pragma once

namespace tau
{
    struct world_t;
    struct editor_context_t;
} // namespace tau

namespace tau::editor::picking
{
    // render features: entity ids under the editor camera, the selection outline
    void init(editor_context_t& ctx);

    void update(world_t& world, editor_context_t& ctx);
} // namespace tau::editor::picking
