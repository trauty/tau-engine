#include "scene_io.h"

#include "tau-editor/asset_watch.h"
#include "tau-editor/commands.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/history.h"
#include "tau-editor/prefab_stage.h"
#include "tau/asset.h"
#include "tau/asset_sources.h"
#include "tau/assets/prefab.h"
#include "tau/assets/scene.h"
#include "tau/log.h"
#include "tau/prefab.h"
#include "tau/serialization.h"
#include "tau/window.h"
#include "tau/world.h"

#include <SDL3/SDL_dialog.h>
#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <json/json.hpp>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

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

    void report_unknown_keys(const world_t& world, const editor_context_t& ctx)
    {
        namespace fs = std::filesystem;
        if (ctx.project_assets_dir.empty()) { return; }

        std::vector<std::string_view> extensions;
        for (const u64_t type_id : {get_type_id<scene_t>(), get_type_id<prefab_t>()})
        {
            if (const asset_type_t* type = assets::type_by_id(type_id))
            {
                extensions.insert(extensions.end(), type->source_extensions.begin(), type->source_extensions.end());
            }
        }

        const fs::path root = ctx.project_assets_dir;
        const prefab::lookup_t lookup = prefab::asset_lookup();
        std::map<std::tuple<std::string, std::string, std::string>, std::vector<std::string>> files_of;

        std::error_code ec;
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, ec))
        {
            const std::string ext = entry.path().extension().string();
            if (!entry.is_regular_file(ec) || std::find(extensions.begin(), extensions.end(), ext) == extensions.end())
            {
                continue;
            }

            std::ifstream file(entry.path());
            const nlohmann::json json = nlohmann::json::parse(file, nullptr, false);
            if (json.is_discarded()) { continue; }

            const scene_t scene = serialization::scene_from_json(prefab::expand(json, lookup));
            const std::string rel = entry.path().lexically_relative(root).generic_string();
            for (const serialization::unknown_key_t& key : serialization::unknown_keys(world, scene))
            {
                files_of[{key.component, key.field, key.enum_value}].push_back(rel);
            }
        }

        for (const auto& [key, files] : files_of)
        {
            const auto& [component, field, enum_value] = key;
            std::string where = files.front();
            for (std::size_t i = 1; i < std::min<std::size_t>(files.size(), 3); i++) { where += ", " + files[i]; }
            if (files.size() > 3) { where += std::format(" and {} more", files.size() - 3); }

            if (field.empty())
            {
                TAU_LOG_WARN("SCENE", "Saved component '{}' matches no registered type ({}). Renamed or removed?",
                             component, where);
            }
            else if (!enum_value.empty())
            {
                TAU_LOG_WARN("SCENE",
                             "Saved value '{}' of '{}' on '{}' matches no enumerator ({}), it loads by its number",
                             enum_value, field, component, where);
            }
            else
            {
                TAU_LOG_WARN("SCENE",
                             "Saved field '{}' on '{}' matches no registered field ({}). Renamed? "
                             "stable_id{{\"{}\"}} on the new field loads the saved values",
                             field, component, where, field);
            }
        }
    }
} // namespace tau::editor::scene_io
