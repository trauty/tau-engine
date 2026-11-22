#include "main_menu_bar.h"

#include "imgui.h"
#include "tau-editor/commands.h"
#include "tau-editor/history.h"
#include "tau-editor/keymap.h"
#include "tau-editor/prefab_stage.h"
#include "tau/rendering/renderdoc.h"

#include <string>

namespace tau::editor::panels
{
    void draw_main_menu_bar(world_t& world, editor_context_t& ctx)
    {
        using commands::command_e;

        if (!ImGui::BeginMainMenuBar()) { return; }

        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Open Project Manager")) { ctx.show_project_manager = true; }

            ImGui::Separator();
            commands::menu_item(ctx, command_e::NEW_SCENE);
            commands::menu_item(ctx, command_e::OPEN_SCENE);

            ImGui::Separator();
            commands::menu_item(ctx, command_e::SAVE);
            commands::menu_item(ctx, command_e::SAVE_SCENE_AS);
            if (prefab_stage::is_open()) { commands::menu_item(ctx, command_e::CLOSE_PREFAB); }

            ImGui::Separator();
            commands::menu_item(ctx, command_e::QUIT, "Exit");
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit"))
        {
            const bool editing = ctx.cur_mode == editor_mode_e::EDIT;
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
            if (ImGui::MenuItem("Project Settings...", nullptr, false, ctx.project_loaded))
            {
                ctx.show_project_settings = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Debug"))
        {
            commands::menu_item(ctx, command_e::CAPTURE_GPU_FRAME,
                                renderer::renderdoc::active() ? nullptr : "Capture GPU Frame (needs RenderDoc)");
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
} // namespace tau::editor::panels
