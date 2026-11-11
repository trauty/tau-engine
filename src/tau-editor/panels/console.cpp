#include "console.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "tau/log.h"

#include <cstring>
#include <string>
#include <vector>

namespace tau::editor::panels
{
    namespace
    {
        // tau::log keeps everything, so the panel pulls rather than being pushed to
        // nothing is lost between frames, and lines from before the first draw are already there
        struct console_state_t
        {
            u64_t cursor = 0;
            std::vector<tau::log::entry_t> lines;

            std::vector<u32_t> visible; // indices into `lines` that pass the filters
            bool visible_dirty = true;

            i32 min_level = static_cast<i32>(tau::log::level_e::LOG_INFO);
            char search[128] = {};
            bool autoscroll = true;
        };

        console_state_t g_console;

        const char* const LEVEL_NAMES[] = {"Trace", "Debug", "Info", "Warn", "Error", "Fatal"};

        ImVec4 level_color(tau::log::level_e level)
        {
            switch (level)
            {
            case tau::log::level_e::LOG_TRACE: return ImVec4(0.55f, 0.55f, 0.58f, 1.0f);
            case tau::log::level_e::LOG_DEBUG: return ImVec4(0.45f, 0.78f, 0.85f, 1.0f);
            case tau::log::level_e::LOG_WARN: return ImVec4(0.92f, 0.76f, 0.30f, 1.0f);
            case tau::log::level_e::LOG_ERROR: return ImVec4(0.94f, 0.42f, 0.38f, 1.0f);
            case tau::log::level_e::LOG_FATAL: return ImVec4(1.00f, 0.32f, 0.55f, 1.0f);
            default: return ImGui::GetStyleColorVec4(ImGuiCol_Text);
            }
        }

        bool passes_filters(const tau::log::entry_t& entry)
        {
            if (static_cast<i32>(entry.level) < g_console.min_level) { return false; }
            if (g_console.search[0] == '\0') { return true; }

            return entry.message.find(g_console.search) != std::string::npos ||
                   entry.category.find(g_console.search) != std::string::npos;
        }

        void rebuild_visible()
        {
            g_console.visible.clear();
            for (u32_t i = 0; i < static_cast<u32_t>(g_console.lines.size()); i++)
            {
                if (passes_filters(g_console.lines[i])) { g_console.visible.push_back(i); }
            }

            g_console.visible_dirty = false;
        }

        void pull_new_lines()
        {
            const size_t before = g_console.lines.size();
            tau::log::read_history(g_console.cursor, g_console.lines);

            if (g_console.lines.size() == before) { return; }

            // only the new tail needs testing unless a filter changed
            if (g_console.visible_dirty)
            {
                rebuild_visible();
                return;
            }

            for (u32_t i = static_cast<u32_t>(before); i < static_cast<u32_t>(g_console.lines.size()); i++)
            {
                if (passes_filters(g_console.lines[i])) { g_console.visible.push_back(i); }
            }
        }
        void draw_lines()
        {
            if (ImGui::BeginChild("##log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_HorizontalScrollbar))
            {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 1.0f));

                // clipped: only rows on screen are laid out, so a full history draws as cheaply as an empty one
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(g_console.visible.size()));

                while (clipper.Step())
                {
                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                    {
                        const tau::log::entry_t& entry = g_console.lines[g_console.visible[row]];

                        ImGui::PushStyleColor(ImGuiCol_Text, level_color(entry.level));
                        ImGui::TextUnformatted(entry.category.c_str());
                        ImGui::SameLine(110.0f);
                        ImGui::TextUnformatted(entry.message.c_str());
                        ImGui::PopStyleColor();
                    }
                }

                ImGui::PopStyleVar();

                if (g_console.autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
            ImGui::EndChild();
        }
    } // namespace

    void draw_log_view()
    {
        pull_new_lines();
        if (g_console.visible_dirty) { rebuild_visible(); }
        draw_lines();
    }

    void draw_console(world_t& world, editor_context_t& ctx)
    {
        (void)world;

        pull_new_lines();
        if (g_console.visible_dirty) { rebuild_visible(); }

        ImGui::Begin("Console");

        ImGui::BeginDisabled(ctx.recompiling);
        if (ImGui::Button("Recompile")) { ctx.recompile_requested = true; }
        ImGui::EndDisabled();

        if (ctx.recompiling)
        {
            ImGui::SameLine();
            ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(160.0f, 0.0f),
                               ctx.cancel_build_requested ? "cancelling..." : "building...");

            ImGui::SameLine();
            ImGui::BeginDisabled(ctx.cancel_build_requested);
            if (ImGui::Button("Cancel")) { ctx.cancel_build_requested = true; }
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::Combo("##level", &g_console.min_level, LEVEL_NAMES, IM_ARRAYSIZE(LEVEL_NAMES)))
        {
            g_console.visible_dirty = true;
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputTextWithHint("##search", "filter", g_console.search, IM_ARRAYSIZE(g_console.search)))
        {
            g_console.visible_dirty = true;
        }

        ImGui::SameLine();
        if (ImGui::Button("Clear"))
        {
            // only what is shown, the log file and the engine's history are the record
            g_console.lines.clear();
            g_console.visible.clear();
        }

        ImGui::SameLine();
        ImGui::Checkbox("Follow", &g_console.autoscroll);

        ImGui::SameLine();
        ImGui::TextDisabled("%zu / %zu", g_console.visible.size(), g_console.lines.size());

        if (g_console.visible_dirty) { rebuild_visible(); }

        ImGui::Separator();

        draw_lines();

        ImGui::End();
    }
} // namespace tau::editor::panels
