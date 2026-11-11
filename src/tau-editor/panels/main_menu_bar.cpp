#include "main_menu_bar.h"

#include "imgui.h"
#include "tau-editor/history.h"
#include "tau-editor/keymap.h"
#include "tau-editor/scene_io.h"
#include "tau/engine.h"
#include "tau/window.h"

#include <SDL3/SDL_dialog.h>
#include <filesystem>
#include <string>
#include <vector>

namespace tau::editor::panels
{
    namespace
    {
        void SDLCALL open_scene_callback(void* userdata, const char* const* filelist, int)
        {
            auto* ctx = static_cast<tau::editor_context_t*>(userdata);
            if (filelist && filelist[0])
            {
                ctx->pending_scene_path = filelist[0];
                ctx->pending_scene_load = true;
            }
        }

        void SDLCALL save_scene_callback(void* userdata, const char* const* filelist, int)
        {
            auto* ctx = static_cast<tau::editor_context_t*>(userdata);
            if (filelist && filelist[0])
            {
                ctx->pending_scene_path = filelist[0];
                ctx->pending_scene_save = true;
            }
        }

        void show_open_dialog(editor_context_t& ctx)
        {
            SDL_DialogFileFilter filters[] = {
                {"Tau Scene", "scene"}
            };
            SDL_ShowOpenFileDialog(open_scene_callback, &ctx, tau::window::get_window(), filters, 1,
                                   ctx.project_dir.empty() ? nullptr : ctx.project_dir.c_str(), false);
        }

        void show_save_dialog(editor_context_t& ctx)
        {
            SDL_DialogFileFilter filters[] = {
                {"Tau Scene", "scene"}
            };
            SDL_ShowSaveFileDialog(save_scene_callback, &ctx, tau::window::get_window(), filters, 1,
                                   ctx.project_dir.empty() ? nullptr : ctx.project_dir.c_str());
        }

        bool affects_assets(unsaved_action_e action)
        { return action == unsaved_action_e::OPEN_PROJECT || action == unsaved_action_e::QUIT; }

        bool has_unsaved(unsaved_action_e action)
        { return affects_assets(action) ? history::any_dirty() : history::is_dirty(history::OPEN_SCENE); }

        void perform(world_t& world, editor_context_t& ctx, unsaved_action_e action)
        {
            switch (action)
            {
            case unsaved_action_e::NONE: break;
            case unsaved_action_e::NEW_SCENE: scene_io::clear(world, ctx); break;
            case unsaved_action_e::OPEN_SCENE: show_open_dialog(ctx); break;
            case unsaved_action_e::LOAD_SCENE:
                ctx.pending_scene_path = ctx.unsaved_action_scene;
                ctx.pending_scene_load = true;
                break;
            case unsaved_action_e::OPEN_PROJECT: ctx.confirmed_project = ctx.unsaved_action_project; break;
            case unsaved_action_e::QUIT: engine::exit(); break;
            }
        }

        void save_all(world_t& world, editor_context_t& ctx)
        {
            history::save_assets();
            if (!ctx.current_scene_path.empty()) { scene_io::save(world, ctx, ctx.current_scene_path); }
            else if (history::is_dirty(history::OPEN_SCENE)) { show_save_dialog(ctx); }
        }

        bool save_unsaved(world_t& world, editor_context_t& ctx, unsaved_action_e action)
        {
            bool ok = !affects_assets(action) || history::save_assets();
            if (history::is_dirty(history::OPEN_SCENE))
            {
                if (ctx.current_scene_path.empty())
                {
                    show_save_dialog(ctx);
                    return false;
                }
                ok = scene_io::save(world, ctx, ctx.current_scene_path) && ok;
            }
            return ok;
        }

        std::string doc_label(const editor_context_t& ctx, history::doc_t doc)
        {
            if (doc.kind == history::doc_kind_e::ASSET) { return history::asset_path(doc); }
            return ctx.current_scene_path.empty() ? "Untitled scene"
                                                  : std::filesystem::path(ctx.current_scene_path).filename().string();
        }

        void draw_unsaved_prompt(world_t& world, editor_context_t& ctx)
        {
            if (ctx.unsaved_action == unsaved_action_e::NONE) { return; }

            constexpr const char* POPUP = "Unsaved Changes";
            if (!ImGui::IsPopupOpen(POPUP)) { ImGui::OpenPopup(POPUP); }

            // imgui centres a modal only the first time
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            constexpr ImGuiWindowFlags flags =
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
            if (!ImGui::BeginPopupModal(POPUP, nullptr, flags)) { return; }

            const unsaved_action_e action = ctx.unsaved_action;
            std::vector<std::string> scene;
            std::vector<std::string> project_assets;
            std::vector<std::string> engine_assets;
            for (const history::doc_t doc : history::dirty_docs())
            {
                if (doc.kind == history::doc_kind_e::SCENE) { scene.push_back(doc_label(ctx, doc)); }
                else if (doc.kind == history::doc_kind_e::ASSET && affects_assets(action))
                {
                    const std::string path = history::asset_path(doc);
                    (path.starts_with("engine://") ? engine_assets : project_assets).push_back(path);
                }
            }

            auto group = [](const char* heading, const std::vector<std::string>& names, const char* note)
            {
                if (names.empty()) { return; }
                ImGui::SeparatorText(heading);
                if (note) { ImGui::TextDisabled("%s", note); }
                for (const std::string& name : names) { ImGui::BulletText("%s", name.c_str()); }
            };

            ImGui::TextUnformatted("Unsaved changes:");
            group("Scene", scene, nullptr);
            group("Project assets", project_assets, nullptr);
            group("Engine assets", engine_assets, "Saving writes into the engine's own assets folder.");
            ImGui::Spacing();

            unsaved_action_e run = unsaved_action_e::NONE;
            bool close = false;

            if (ImGui::Button("Save"))
            {
                if (save_unsaved(world, ctx, action)) { run = action; }
                close = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Don't Save"))
            {
                run = action;
                close = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { close = true; }

            if (close)
            {
                ctx.unsaved_action = unsaved_action_e::NONE;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();

            perform(world, ctx, run);
        }
    } // namespace

    void request_action(world_t& world, editor_context_t& ctx, unsaved_action_e action, std::string project)
    {
        ctx.unsaved_action_project = std::move(project);

        if (has_unsaved(action)) { ctx.unsaved_action = action; }
        else
        {
            perform(world, ctx, action);
        }
    }

    void draw_main_menu_bar(world_t& world, editor_context_t& ctx)
    {
        const bool editing = ctx.cur_mode == editor_mode_e::EDIT;

        if (editing && keymap::pressed(keymap::action_e::SAVE)) { save_all(world, ctx); }

        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open Project Manager")) { ctx.show_project_manager = true; }

                ImGui::Separator();

                if (ImGui::MenuItem("New Scene", nullptr, false, editing))
                {
                    request_action(world, ctx, unsaved_action_e::NEW_SCENE);
                }
                if (ImGui::MenuItem("Open Scene...", nullptr, false, editing))
                {
                    request_action(world, ctx, unsaved_action_e::OPEN_SCENE);
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Save", keymap::label(keymap::action_e::SAVE).c_str(), false, editing))
                {
                    save_all(world, ctx);
                }
                if (ImGui::MenuItem("Save Scene As...", nullptr, false, editing)) { show_save_dialog(ctx); }

                ImGui::Separator();

                if (ImGui::MenuItem("Exit")) { request_action(world, ctx, unsaved_action_e::QUIT); }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit"))
            {
                const std::string undo = history::can_undo() ? "Undo " + std::string(history::undo_label()) : "Undo";
                const std::string redo = history::can_redo() ? "Redo " + std::string(history::redo_label()) : "Redo";

                if (ImGui::MenuItem(undo.c_str(), keymap::label(keymap::action_e::UNDO).c_str(), false,
                                    editing && history::can_undo()))
                {
                    history::undo(world, ctx);
                }
                if (ImGui::MenuItem(redo.c_str(), keymap::label(keymap::action_e::REDO).c_str(), false,
                                    editing && history::can_redo()))
                {
                    history::redo(world, ctx);
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Settings...")) { ctx.show_settings = true; }
                ImGui::EndMenu();
            }

            ImGui::EndMainMenuBar();
        }

        draw_unsaved_prompt(world, ctx);
    }
} // namespace tau::editor::panels
