#pragma once

#include <filesystem>
#include <future>

namespace tau
{
    struct editor_context_t;
    struct project_t;
} // namespace tau

// the project's game library: built with xmake, loaded from a copy, hot reloaded when its build trigger moves
namespace tau::editor::game_code
{
    void init(editor_context_t& ctx);

    // configure and build off the UI thread, one build at a time
    std::future<bool> start_build(const std::filesystem::path& project_file);
    void cancel_build();
    bool build_cancelled();

    bool load(const project_t& project, editor_context_t& ctx);
    void unload(editor_context_t& ctx);

    void update(editor_context_t& ctx);
} // namespace tau::editor::game_code
