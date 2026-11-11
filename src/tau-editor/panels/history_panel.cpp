#include "history_panel.h"

#include "imgui.h"
#include "tau-editor/history.h"

#include <cstddef>
#include <limits>
#include <string>

namespace tau::editor::panels
{
    void draw_history(world_t& world, editor_context_t& ctx)
    {
        if (ImGui::Begin("History"))
        {
            static std::size_t shown_applied = std::numeric_limits<std::size_t>::max();

            const std::size_t applied = history::applied_count();
            const bool moved = applied != shown_applied;
            shown_applied = applied;

            std::size_t jump = applied;

            // click a row: undo/redo up to it
            ImGui::BeginDisabled(ctx.cur_mode != editor_mode_e::EDIT);

            if (ImGui::Selectable("Start", applied == 0)) { jump = 0; }
            if (moved && applied == 0) { ImGui::SetScrollHereY(); }

            for (std::size_t i = 0; i < history::step_count(); i++)
            {
                ImGui::PushID(static_cast<i32>(i));

                const bool undone = i >= applied;
                if (undone) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)); }

                const std::string label(history::step_label(i));
                if (ImGui::Selectable(label.c_str(), i + 1 == applied)) { jump = i + 1; }
                if (moved && i + 1 == applied) { ImGui::SetScrollHereY(); }

                if (undone) { ImGui::PopStyleColor(); }
                ImGui::PopID();
            }

            ImGui::EndDisabled();

            if (jump != applied) { history::go_to(world, ctx, jump); }
        }

        ImGui::End();
    }
} // namespace tau::editor::panels
