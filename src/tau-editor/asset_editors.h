#pragma once

#include "tau/asset.h"
#include "tau/ecs_fwd.h"
#include "tau/math.h"

#include <span>
#include <string>
#include <string_view>

namespace tau
{
    struct world_t;
    struct editor_context_t;
} // namespace tau

namespace tau::editor::asset_editors
{
    struct asset_editor_t
    {
        u64_t type_id = 0;
        // tile and label color
        u32_t color = 0xff969696;

        // assets panel "New" entry, returns the made asset's project relative path
        const char* create_label = nullptr;
        std::string (*create)(world_t& world, const std::string& dir) = nullptr;

        // double click in the Assets panel
        void (*open)(world_t& world, editor_context_t& ctx, const std::string& vfs) = nullptr;

        // loaded and held while the inspector shows it
        bool load_when_inspected = false;
        // inspector body below the asset header
        void (*draw_inspector)(world_t& world, editor_context_t& ctx, const std::string& vfs,
                               asset_handle_t handle) = nullptr;
        // under an asset slot that holds one, true when edited
        bool (*draw_inline)(world_t& world, asset_handle_t handle) = nullptr;

        // tile preview: what to load once, then its image (bindless id, BINDLESS_NULL_HANDLE while not ready)
        asset_handle_t (*preview_load)(const std::string& vfs) = nullptr;
        u32_t (*preview_image)(asset_handle_t handle) = nullptr;

        // dragged into the viewport at a world point, or onto an entity in the hierarchy (null: none)
        void (*drop_in_viewport)(world_t& world, editor_context_t& ctx, const std::string& vfs,
                                 const vec3& at) = nullptr;
        void (*drop_on_entity)(world_t& world, editor_context_t& ctx, const std::string& vfs,
                               ecs::entity_t parent) = nullptr;

        // changed on disk, true when reloaded (default assets::reload)
        bool (*reload)(world_t& world, const std::string& vfs) = nullptr;
        // unsaved beyond its history doc
        bool (*unsaved)(const std::string& vfs) = nullptr;
    };

    // one per type, a second add replaces
    void add(const asset_editor_t& editor);

    const asset_editor_t* of(u64_t type_id);
    const asset_editor_t* of_path(std::string_view path);
    std::span<const asset_editor_t> all();

    // texture, mesh, shader, scene; material and prefab register from their own modules
    void register_builtin(editor_context_t& ctx);
} // namespace tau::editor::asset_editors
