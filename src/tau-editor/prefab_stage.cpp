#include "prefab_stage.h"

#include "imgui.h"
#include "tau-editor/asset_watch.h"
#include "tau-editor/commands.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/events.h"
#include "tau-editor/handle_math.h"
#include "tau-editor/history.h"
#include "tau-editor/keymap.h"
#include "tau-editor/prefab_edit.h"
#include "tau-editor/viewport_input.h"
#include "tau/asset.h"
#include "tau/assets/prefab.h"
#include "tau/guid.h"
#include "tau/guid_json.h"
#include "tau/log.h"
#include "tau/prefab.h"
#include "tau/rhi/context.h"
#include "tau/serialization.h"
#include "tau/systems/guid.h"
#include "tau/world.h"

#include <cmath>
#include <filesystem>
#include <json/json.hpp>
#include <vector>

namespace tau::editor::prefab_stage
{
    namespace
    {
        struct state_t
        {
            bool open = false;
            asset_t<prefab_t> prefab;
            nlohmann::json base;
            nlohmann::json written;
            u64_t root = 0;
            u64_t base_state = 0;
            bool frame_pending = false;

            nlohmann::json scene;
            std::vector<asset_handle_t> scene_assets;
            selection_t selection;
            editor_camera_t camera;
        };

        state_t g;
        editor_context_t* g_ctx = nullptr;

        history::doc_t doc() { return {history::doc_kind_e::STAGE, g.prefab.guid()}; }

        std::string file_name() { return std::filesystem::path(assets::path_of(g.prefab)).filename().string(); }

        void stage_roots(std::vector<asset_handle_t>& out)
        {
            if (!g.open) { return; }
            out.insert(out.end(), g.scene_assets.begin(), g.scene_assets.end());
            out.push_back(g.prefab);
        }

        void load_entities(world_t& world, const nlohmann::json& data)
        {
            serialization::clear_scene(world);

            // nested prefabs come in as instances, under the guids this prefab gave them
            const nlohmann::json expanded = prefab::expand(data, prefab::asset_lookup());
            std::vector<serialization::entity_state_t> batch;
            for (const nlohmann::json& entity : expanded["entities"])
            {
                batch.push_back({guid::from_json(entity["id"]), &entity});
            }
            serialization::apply_entities(world, batch);

            g.root = guid::from_json(data["entities"][0]["id"]);
        }

        void frame(world_t& world, editor_context_t& ctx)
        {
            std::vector<ecs::entity_t> all;
            for (const ecs::entity_t entity : world.registry.view<ecs::entity_t>()) { all.push_back(entity); }
            viewport_input::frame_entities(world, ctx, all);
        }

        void reset() { g = {}; }

        bool editing(const editor_context_t& ctx) { return ctx.project_loaded && ctx.cur_mode == editor_mode_e::EDIT; }
    } // namespace

    void init(editor_context_t& ctx)
    {
        g_ctx = &ctx;
        assets::add_roots(&stage_roots);
        events::on(events::event_e::PROJECT_OPENED, [](world_t&, editor_context_t&) { reset(); });

        commands::set(commands::command_e::OPEN_PREFAB,
                      {.label = "Open Prefab",
                       .enabled = &editing,
                       .run = [](world_t& world, editor_context_t& ctx, const commands::request_t& request)
                       { open(world, ctx, request.path, request.guid); }});
        commands::set(
            commands::command_e::CLOSE_PREFAB,
            {.label = "Close Prefab",
             .shortcut = keymap::action_e::CLOSE_PREFAB,
             .enabled = [](const editor_context_t& ctx) { return editing(ctx) && g.open; },
             .run = [](world_t& world, editor_context_t& ctx, const commands::request_t&) { close(world, ctx); }});
    }

    bool is_open() { return g.open; }

    asset_t<prefab_t> prefab() { return g.prefab; }

    ecs::entity_t root(world_t& world) { return g.open ? guid_system::find(world.registry, g.root) : ecs::NULL_ENTITY; }

    bool is_dirty() { return g.open && history::is_dirty(doc()); }

    bool open(world_t& world, editor_context_t& ctx, const std::string& vfs_path, u64_t select)
    {
        if (assets::split_sub_asset(vfs_path))
        {
            TAU_LOG_WARN("PREFAB", "{} belongs to its model and is read-only: change the model, or make a variant",
                         vfs_path);
            return false;
        }

        const asset_t<prefab_t> handle = assets::load<prefab_t>(vfs_path);
        const prefab_t* prefab = assets::get(handle);
        if (!prefab || !prefab->data.contains("entities") || prefab->data["entities"].empty())
        {
            TAU_LOG_ERROR("PREFAB", "Cannot open {}, it did not load or holds no entities", vfs_path);
            return false;
        }

        if (g.open && g.prefab == handle)
        {
            if (select != 0) { ctx.selection.set(select); }
            return true;
        }
        if (g.open) { close(world, ctx); }

        g.scene = serialization::serialize_scene(world);
        g.scene_assets.clear();
        serialization::collect_asset_refs(world, g.scene_assets);
        g.selection = ctx.selection;
        g.camera = ctx.editor_camera;

        g.open = true;
        g.prefab = handle;
        g.base = prefab->data;
        g.written = nullptr;
        load_entities(world, g.base);

        history::set_entity_doc(doc());
        history::drop(doc());
        history::set_floor(true);
        g.base_state = history::doc_state(doc());

        ctx.inspected_asset.clear();
        g.frame_pending = true;
        ctx.selection.set(select != 0 && guid_system::find(world.registry, select) != ecs::NULL_ENTITY ? select
                                                                                                       : g.root);
        ctx.scroll_to_selection = true;

        TAU_LOG_INFO("PREFAB", "Opened {}", vfs_path);
        return true;
    }

    void close(world_t& world, editor_context_t& ctx)
    {
        if (!g.open) { return; }

        nlohmann::json data;
        if (history::doc_state(doc()) != g.base_state)
        {
            const ecs::entity_t stage_root = guid_system::find(world.registry, g.root);
            if (stage_root != ecs::NULL_ENTITY)
            {
                data = prefab_edit::data_of(world, stage_root, g.base.value("guid", nlohmann::json{}));
            }
        }

        serialization::clear_scene(world);
        serialization::deserialize_scene(world, g.scene);

        history::drop(doc());
        history::set_floor(false);
        history::set_entity_doc(history::OPEN_SCENE);
        ctx.selection = g.selection;
        ctx.editor_camera = g.camera;

        const asset_t<prefab_t> handle = g.prefab;
        const nlohmann::json written = std::move(g.written);
        const std::string name = std::filesystem::path(assets::path_of(handle)).stem().string();
        g = {};

        if (!data.is_null())
        {
            prefab_edit::replace_data(world, handle, data, "Edit Prefab " + name);
            if (written == data) { history::mark_saved({history::doc_kind_e::ASSET, handle.guid()}); }
        }
    }

    bool save(world_t& world)
    {
        if (!g.open) { return false; }

        const ecs::entity_t stage_root = guid_system::find(world.registry, g.root);
        if (stage_root == ecs::NULL_ENTITY) { return false; }

        nlohmann::json data = prefab_edit::data_of(world, stage_root, g.base.value("guid", nlohmann::json{}));
        std::filesystem::path source;
        if (!prefab_edit::write_data(g.prefab, data, source)) { return false; }

        asset_watch::cook_saved({&source, 1});
        history::mark_saved(doc());
        g.written = std::move(data);
        return true;
    }

    bool on_disk_change(world_t& world, const std::string& vfs_path)
    {
        if (!g.open || assets::find(vfs_path) != g.prefab.handle) { return false; }
        editor_context_t& ctx = *g_ctx;

        if (is_dirty())
        {
            TAU_LOG_WARN("PREFAB", "{} changed on disk, kept the unsaved edits of the open prefab", vfs_path);
            return true;
        }
        if (!assets::reload(g.prefab.handle)) { return true; }
        history::drop({history::doc_kind_e::ASSET, g.prefab.guid()});

        const prefab_t* prefab = assets::get(g.prefab);
        if (!prefab || !prefab->data.contains("entities") || prefab->data["entities"].empty())
        {
            TAU_LOG_ERROR("PREFAB", "{} holds no entities anymore, closing it", vfs_path);
            close(world, ctx);
            return true;
        }

        g.base = prefab->data;
        g.written = nullptr;
        load_entities(world, g.base);
        history::drop(doc());
        g.base_state = history::doc_state(doc());
        std::erase_if(ctx.selection.guids,
                      [&](u64_t guid) { return guid_system::find(world.registry, guid) == ecs::NULL_ENTITY; });

        TAU_LOG_INFO("PREFAB", "Reloaded the open prefab {}", vfs_path);
        return true;
    }

    void draw_bar(world_t& world, editor_context_t& ctx)
    {
        if (!g.open) { return; }

        if (g.frame_pending)
        {
            g.frame_pending = false;
            frame(world, ctx);
        }

        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 6.0f, ImGui::GetCursorPosY() + 4.0f));
        commands::button(ctx, commands::command_e::CLOSE_PREFAB, "< Scene", true);
        if (ImGui::IsItemHovered())
        {
            const std::string key = keymap::label(keymap::action_e::CLOSE_PREFAB);
            ImGui::SetTooltip(key.empty() ? "Back to the scene, the edits stay as an unsaved prefab"
                                          : "Back to the scene, the edits stay as an unsaved prefab (%s)",
                              key.c_str());
        }

        ImGui::SameLine();
        ImGui::TextDisabled(">");
        ImGui::SameLine();
        ImGui::TextUnformatted((file_name() + (is_dirty() ? "*" : "")).c_str());
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", assets::path_of(g.prefab).c_str()); }

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
    }
} // namespace tau::editor::prefab_stage
