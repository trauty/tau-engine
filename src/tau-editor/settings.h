#pragma once

namespace tau { struct editor_context_t; } // namespace tau

// editor prefs in <engine>/user/editor_settings.json
namespace tau::editor::settings
{
    void load(editor_context_t& ctx);
    void save(const editor_context_t& ctx);

    void draw(editor_context_t& ctx);
} // namespace tau::editor::settings
