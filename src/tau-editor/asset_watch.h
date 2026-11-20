#pragma once

#include "tau/defines.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace tau::editor::asset_watch
{
    // notices assets changing on disk, cooks them, reloads what was live
    // shaders and game code are authored outside the editor, so polling is how it learns
    // a backend only answers which paths might have changed, everything hard sits above it
    // ReadDirectoryChangesW or inotify could replace it, their overflow path is a full rescan anyway
    struct backend_t
    {
        virtual ~backend_t() = default;

        virtual bool start(const std::filesystem::path& root) = 0;

        // appends every path that may have changed, candidates only
        virtual void collect(std::vector<std::filesystem::path>& out) = 0;
    };

    // walks the tree again and reports entries whose mtime or size moved
    std::unique_ptr<backend_t> make_polling_backend();

    using backend_factory_t = std::unique_ptr<backend_t> (*)();

    // `game_root` is the project's assets, `engine_root` when set the engine's asset sources
    // an engine edit is cooked into the tree engine:// loads from, and project assets built from it recook
    // each root gets its own backend from `make_backend`
    void start(const std::filesystem::path& game_root, const std::filesystem::path& engine_root = {},
               backend_factory_t make_backend = &make_polling_backend);
    void stop();

    // call once per editor frame, polls at most every poll_interval, cooks off thread, reloads on the caller
    void tick();

    // tells the watcher the editor is about to write this file, so the change is not an outside edit
    // without it a material save would cook, see the write, and cook again forever
    void ignore_own_write(const std::filesystem::path& path);

    // editor-saved sources, cooked off thread with the next batch
    // not reloaded, memory already holds them; their dependents are
    void cook_saved(std::span<const std::filesystem::path> sources);

    // sources whose import changed (their .meta), cooked with the next batch and reloaded
    void cook_edited(std::span<const std::filesystem::path> sources);

    void flush();

    // the open scene is cooked but never reloaded, memory holds the newer copy
    void set_open_scene(const std::filesystem::path& path);

    bool is_cooking();
} // namespace tau::editor::asset_watch
