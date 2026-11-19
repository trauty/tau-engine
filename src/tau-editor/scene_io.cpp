#include "scene_io.h"

#include "tau-editor/asset_watch.h"
#include "tau-editor/commands.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/history.h"
#include "tau-editor/prefab_stage.h"
#include "tau/asset.h"
#include "tau/asset_sources.h"
#include "tau/log.h"
#include "tau/serialization.h"
#include "tau/window.h"
#include "tau/world.h"

#include <SDL3/SDL_dialog.h>
#include <fstream>
#include <json/json.hpp>

namespace tau::editor::scene_io
{
    namespace
    {
        constexpr SDL_DialogFileFilter SCENE_FILTER = {"Tau Scene", "scene"};

        // past the unsaved prompt already, OPEN_SCENE asked before the dialog
        void SDLCALL open_scene_callback(void*, const char* const* filelist, int)
        {
            if (filelist && filelist[0])
            {
                commands::push({.command = commands::command_e::LOAD_SCENE, .path = filelist[0], .confirmed = true});
            }
        }

        void SDLCALL save_scene_callback(void*, const char* const* filelist, int)
        {
            if (filelist && filelist[0]) { commands::push(commands::command_e::SAVE_SCENE_TO, filelist[0]); }
        }

        const char* dialog_dir(const editor_context_t& ctx)
        { return ctx.project_dir.empty() ? nullptr : ctx.project_dir.c_str(); }

        void save_all(world_t& world, editor_context_t& ctx)
        {
            history::save_assets();

            if (prefab_stage::is_open())
            {
                prefab_stage::save(world);
                return;
            }
            if (!ctx.current_scene_path.empty()) { save(world, ctx, ctx.current_scene_path); }
            else if (history::is_dirty(history::OPEN_SCENE)) { commands::push(commands::command_e::SAVE_SCENE_AS); }
        }

        bool editing(const editor_context_t& ctx) { return ctx.project_loaded && ctx.cur_mode == editor_mode_e::EDIT; }
    } // namespace

    void init(editor_context_t&)
    {
        using commands::command_e;
        using commands::discards_e;

        commands::set(command_e::NEW_SCENE, {.label = "New Scene",
                                             .discards = discards_e::SCENE,
                                             .enabled = &editing,
                                             .run = [](world_t& world, editor_context_t& ctx,
                                                       const commands::request_t&) { clear(world, ctx); }});
        commands::set(command_e::OPEN_SCENE, {.label = "Open Scene...",
                                              .discards = discards_e::SCENE,
                                              .enabled = &editing,
                                              .run = [](world_t&, editor_context_t& ctx, const commands::request_t&)
                                              {
                                                  SDL_ShowOpenFileDialog(open_scene_callback, nullptr,
                                                                         window::get_window(), &SCENE_FILTER, 1,
                                                                         dialog_dir(ctx), false);
                                              }});
        commands::set(command_e::LOAD_SCENE,
                      {.label = "Open Scene",
                       .discards = discards_e::SCENE,
                       .enabled = &editing,
                       .run = [](world_t& world, editor_context_t& ctx, const commands::request_t& request)
                       { load(world, ctx, request.path); }});
        commands::set(command_e::SAVE, {.label = "Save",
                                        .shortcut = keymap::action_e::SAVE,
                                        .enabled = &editing,
                                        .run = [](world_t& world, editor_context_t& ctx, const commands::request_t&)
                                        { save_all(world, ctx); }});
        commands::set(command_e::SAVE_SCENE_AS,
                      {.label = "Save Scene As...",
                       .enabled = [](const editor_context_t& ctx) { return editing(ctx) && !prefab_stage::is_open(); },
                       .run =
                           [](world_t&, editor_context_t& ctx, const commands::request_t&)
                       {
                           SDL_ShowSaveFileDialog(save_scene_callback, nullptr, window::get_window(), &SCENE_FILTER, 1,
                                                  dialog_dir(ctx));
                       }});
        commands::set(command_e::SAVE_SCENE_TO,
                      {.label = "Save Scene",
                       .enabled = [](const editor_context_t& ctx) { return editing(ctx) && !prefab_stage::is_open(); },
                       .run = [](world_t& world, editor_context_t& ctx, const commands::request_t& request)
                       { save(world, ctx, request.path); }});
    }

    void set_path(editor_context_t& ctx, const std::string& path)
    {
        ctx.current_scene_path = path;
        asset_watch::set_open_scene(path);
    }

    bool save(world_t& world, editor_context_t& ctx, const std::string& path)
    {
        nlohmann::json scene_json = serialization::serialize_scene(world);
        // new path = new asset, existing path keeps its guid
        scene_json["guid"] = assets::guid_text(asset_sources::guid_for_writing(path));

        std::ofstream file(path);
        if (!file.is_open())
        {
            TAU_LOG_ERROR("EDITOR", "Cannot write the scene to {}", path);
            return false;
        }
        file << scene_json.dump(4);
        file.close();

        set_path(ctx, path);
        history::mark_saved(history::OPEN_SCENE);
        TAU_LOG_INFO("EDITOR", "Scene saved to {}", path);

        // cooked .tauscene is runtime only, cooks in the background
        const std::filesystem::path source = path;
        asset_watch::cook_saved({&source, 1});
        return true;
    }

    bool load(world_t& world, editor_context_t& ctx, const std::string& path)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            TAU_LOG_ERROR("EDITOR", "Failed to open scene file '{}'", path);
            return false;
        }

        const nlohmann::json scene_json = nlohmann::json::parse(file, nullptr, false);
        if (scene_json.is_discarded())
        {
            TAU_LOG_ERROR("EDITOR", "Failed to parse scene file '{}'", path);
            return false;
        }

        serialization::clear_scene(world);
        serialization::deserialize_scene(world, scene_json);
        assets::collect();

        set_path(ctx, path);
        ctx.selection.clear();
        history::drop(history::OPEN_SCENE);

        TAU_LOG_INFO("EDITOR", "Scene loaded from {}", path);
        return true;
    }

    void clear(world_t& world, editor_context_t& ctx)
    {
        serialization::clear_scene(world);
        assets::collect();

        set_path(ctx, {});
        ctx.selection.clear();
        history::drop(history::OPEN_SCENE);

        TAU_LOG_INFO("EDITOR", "New scene created");
    }
} // namespace tau::editor::scene_io
