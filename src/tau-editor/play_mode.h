#pragma once

namespace tau { struct editor_context_t; }

namespace tau::editor::play_mode
{
    // PLAY and STOP, back to Edit when a project opens
    void init(editor_context_t& ctx);
} // namespace tau::editor::play_mode
