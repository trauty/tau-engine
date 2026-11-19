#include "toolbar.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "tau-editor/commands.h"
#include "tau-editor/keymap.h"
#include "tau-editor/prefab_stage.h"
#include "tau-editor/settings.h"
#include "tau/ecs_fwd.h"
#include "tau/rendering/renderer_types.h"

namespace tau::editor::panels
{
    namespace
    {
        void mode_button(editor_context_t& ctx, handle_mode_e mode, const char* label, const char* tooltip,
                         keymap::action_e action)
        {
            const bool active = ctx.handle_mode == mode;
            if (active) { ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)); }

            if (ImGui::Button(label)) { ctx.handle_mode = mode; }
            if (active) { ImGui::PopStyleColor(); }
            if (ImGui::IsItemHovered())
            {
                const std::string key = keymap::label(action);
                ImGui::SetTooltip(key.empty() ? "%s" : "%s (%s)", tooltip, key.c_str());
            }

            ImGui::SameLine();
        }
    } // namespace

    void draw_toolbar(world_t& world, editor_context_t& ctx)
    {
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 32.0f), ImVec2(FLT_MAX, 32.0f));

        ImGuiWindowClass window_class;
        window_class.DockNodeFlagsOverrideSet =
            ImGuiDockNodeFlags_NoResize | (ImGuiDockNodeFlags)ImGuiDockNodeFlags_NoTabBar;
        ImGui::SetNextWindowClass(&window_class);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar;
        ImGui::Begin("Toolbar", nullptr, flags);

        mode_button(ctx, handle_mode_e::NONE, "Select", "Select only", keymap::action_e::SELECT_TOOL);
        mode_button(ctx, handle_mode_e::MOVE, "Move", "Move", keymap::action_e::MOVE_TOOL);
        mode_button(ctx, handle_mode_e::ROTATE, "Rotate", "Rotate", keymap::action_e::ROTATE_TOOL);
        mode_button(ctx, handle_mode_e::SCALE, "Scale", "Scale", keymap::action_e::SCALE_TOOL);

        if (ImGui::Button(ctx.handles_local ? "Local" : "World")) { ctx.handles_local = !ctx.handles_local; }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Handle axes: the entity's own, or the world's"); }

        ImGui::SameLine();
        if (ImGui::Button("Snap")) { ImGui::OpenPopup("SnapSettings"); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Snap steps, used while Ctrl is held during a drag"); }
        if (ImGui::BeginPopup("SnapSettings"))
        {
            bool edited = false;
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("Move", &ctx.snap_move, 0.05f, 0.001f, 1000.0f, "%.3f");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("Rotate", &ctx.snap_rotate_deg, 0.5f, 0.1f, 180.0f, "%.1f deg");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("Scale", &ctx.snap_scale, 0.01f, 0.001f, 10.0f, "%.3f");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::TextDisabled("Hold Ctrl while dragging");
            if (edited) { settings::save(ctx); }
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        if (ctx.cur_mode == editor_mode_e::EDIT)
        {
            commands::button(ctx, commands::command_e::PLAY);
            if (prefab_stage::is_open() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("Go back to the scene to play it");
            }
        }
        else
        {
            commands::button(ctx, commands::command_e::STOP);

            if (ctx.reload_pending)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Reload pending");
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("The game library was rebuilt, it reloads on Stop."); }
            }
        }

        ImGui::SameLine();
        ImGui::Checkbox("Replay on reload", &ctx.replay_on_reload);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("When the game library is rebuilt during Play, stop, reload and start Play again.");
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);

        ImGui::Checkbox("Post", &ctx.post_processing);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Run the camera's post-process stack and volumes in the viewport.\n"
                              "Off still tonemaps, so the scene stays viewable.");
        }

        ImGui::SameLine();
        ImGui::Checkbox("Colliders", &ctx.show_colliders);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Outline colliders: static green, kinematic blue, dynamic orange, sensors yellow,\n"
                              "gray without a rigidbody.");
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);

#define TAU_DEBUG_VIEW_LABEL(name, value, label) label,
        static const char* const DEBUG_VIEW_NAMES[] = {TAU_DEBUG_VIEW_LIST(TAU_DEBUG_VIEW_LABEL)};
#undef TAU_DEBUG_VIEW_LABEL
        i32 current = static_cast<i32>(ctx.debug_view);
        if (ImGui::Combo("##debug_view", &current, DEBUG_VIEW_NAMES, IM_ARRAYSIZE(DEBUG_VIEW_NAMES)))
        {
            ctx.debug_view = static_cast<tau::renderer::debug_view_e>(current);
        }

        ImGui::End();
    }
} // namespace tau::editor::panels
