#pragma once

#include <filesystem>

namespace tau { struct editor_context_t; }

namespace tau::editor::project_open
{
    void init(editor_context_t& ctx);

    void request(const std::filesystem::path& project_file, editor_context_t& ctx);

    // no project open: the picker or the open in progress, between ImGui::NewFrame and Render
    void draw_gate(editor_context_t& ctx);
    // the project manager window over an open project
    void draw_manager(editor_context_t& ctx);

    // a running build or cook would hold up the exit
    void cancel();
} // namespace tau::editor::project_open
