#pragma once

#include "tau/asset.h"
#include "tau/ecs_fwd.h"

#include <string>

namespace tau
{
    struct world_t;
    struct editor_context_t;
    struct prefab_t;
} // namespace tau

namespace tau::editor::prefab_stage
{
    void init(editor_context_t& ctx);

    bool is_open();
    asset_t<prefab_t> prefab();
    ecs::entity_t root(world_t& world);
    bool is_dirty();

    // OPEN_PREFAB and CLOSE_PREFAB run these
    bool open(world_t& world, editor_context_t& ctx, const std::string& vfs_path, u64_t select = 0);
    void close(world_t& world, editor_context_t& ctx);

    bool save(world_t& world);

    bool on_disk_change(world_t& world, const std::string& vfs_path);

    void draw_bar(world_t& world, editor_context_t& ctx);
} // namespace tau::editor::prefab_stage
