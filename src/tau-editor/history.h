#pragma once

#include "tau/asset.h"
#include "tau/defines.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tau
{
    struct world_t;
    struct editor_context_t;
} // namespace tau

// undo/redo and unsaved state for everything the editor edits
namespace tau::editor::history
{
    enum class doc_kind_e : u8_t
    {
        SCENE,
        ASSET,
        // id 0, keys: asset_files entries (stable through moves)
        FILES,
    };

    struct doc_t
    {
        doc_kind_e kind = doc_kind_e::SCENE;
        u64_t id = 0;

        bool operator==(const doc_t&) const = default;
    };

    constexpr doc_t OPEN_SCENE = {doc_kind_e::SCENE, 0};
    constexpr doc_t PROJECT_FILES = {doc_kind_e::FILES, 0};

    struct target_t
    {
        doc_t doc;
        u64_t key = 0;

        bool operator==(const target_t&) const = default;
    };

    target_t entity_target(u64_t guid);

    using capture_fn_t = std::vector<u8_t> (*)(asset_handle_t handle);
    using apply_fn_t = bool (*)(asset_handle_t handle, std::span<const u8_t> state);
    // writes the asset's source only, saving cooks every written source together
    using write_fn_t = bool (*)(asset_handle_t handle, std::filesystem::path& out_source);

    void reg_editable(u64_t type_id, capture_fn_t capture, apply_fn_t apply, write_fn_t write);
    target_t asset_target(u64_t type_id, asset_handle_t handle);
    doc_t asset_doc(const std::string& vfs_path);

    void init(editor_context_t& ctx);
    void clear();

    void begin(world_t& world, std::string_view label, std::span<const target_t> targets);
    void end(world_t& world);
    void cancel(world_t& world);

    // in place widgets: watch before drawing, report changes
    // step stays open while an item is active
    void watch(world_t& world, target_t target);
    void edited(world_t& world, target_t target, std::string_view label);

    // after the panels: ends in place steps, handles undo/redo keys
    void update(world_t& world, editor_context_t& ctx);

    bool can_undo();
    bool can_redo();
    std::string_view undo_label();
    std::string_view redo_label();
    void undo(world_t& world, editor_context_t& ctx);
    void redo(world_t& world, editor_context_t& ctx);

    std::size_t step_count();
    std::size_t applied_count();
    std::string_view step_label(std::size_t index);
    void go_to(world_t& world, editor_context_t& ctx, std::size_t applied);

    bool is_dirty(doc_t doc);
    bool any_dirty();
    std::vector<doc_t> dirty_docs();
    std::string asset_path(doc_t doc);
    void mark_saved(doc_t doc);
    // sources first, then one cook per tree: project and engine
    bool save_assets();
    bool save_asset(doc_t doc);
    // back to what is on disk, its undo steps go
    bool revert_asset(doc_t doc);
    // closed scene / asset reloaded from disk: steps dropped, counts as saved
    void drop(doc_t doc);

    struct held_t
    {
        target_t target;
        std::vector<u8_t> state;
    };

    // unsaved asset edits, taken before a reload and put back after it
    std::vector<held_t> hold_unsaved_assets(world_t& world);
    void restore_unsaved_assets(world_t& world, const std::vector<held_t>& held);
} // namespace tau::editor::history
