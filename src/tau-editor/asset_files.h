#pragma once

#include "tau/defines.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace tau
{
    struct world_t;
    struct editor_context_t;
} // namespace tau

// project asset sources on disk, each op one undo step
// paths are relative to the assets folder, '/' separated
// deleted items wait in <cooked>/trash for undo, emptied when a project opens
namespace tau::editor::asset_files
{
    void init(editor_context_t& ctx);
    void reset();

    std::filesystem::path source_path(const std::string& rel);
    std::string vfs_path(const std::string& rel);

    std::vector<u8_t> capture(u64_t entry);
    bool apply(world_t& world, u64_t entry, std::span<const u8_t> state);

    // inside a larger undo step: reserve before begin, write after
    u64_t reserve_entry();
    bool write_new_file(u64_t entry, const std::string& rel, const std::string& text);
    // dir/stem.ext, numbered when taken
    std::string unique_path(const std::string& dir, const std::string& stem, const std::string& ext);

    std::string create_folder(world_t& world, const std::string& dir);
    std::string create_material(world_t& world, const std::string& dir);
    std::string create_scene(world_t& world, const std::string& dir);

    bool rename(world_t& world, const std::string& rel, const std::string& new_name);
    bool move(world_t& world, std::span<const std::string> rels, const std::string& dest_dir);
    bool remove(world_t& world, std::span<const std::string> rels);
    std::vector<std::string> duplicate(world_t& world, std::span<const std::string> rels);
} // namespace tau::editor::asset_files
