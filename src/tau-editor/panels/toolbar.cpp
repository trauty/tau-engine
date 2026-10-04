#include "toolbar.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "tau/ecs_fwd.h"
#include "tau/rendering/renderer_types.h"

namespace tau::editor::panels
{
    void draw_toolbar(world_t& world, editor_context_t& ctx)
    {
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 32.0f), ImVec2(FLT_MAX, 32.0f));

        ImGuiWindowClass window_class;
        window_class.DockNodeFlagsOverrideSet =
            ImGuiDockNodeFlags_NoResize | (ImGuiDockNodeFlags)ImGuiDockNodeFlags_NoTabBar;
        ImGui::SetNextWindowClass(&window_class);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar;
        ImGui::Begin("Toolbar", nullptr, flags);

        if (ctx.cur_mode == editor_mode_e::EDIT)
        {
            if (ImGui::Button("Play")) { ctx.cur_mode = editor_mode_e::PLAY; }
        }
        else if (ctx.cur_mode == editor_mode_e::PLAY)
        {
            if (ImGui::Button("Stop")) { ctx.cur_mode = editor_mode_e::EDIT; }

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
