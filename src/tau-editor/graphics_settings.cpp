#include "graphics_settings.h"

#include "imgui.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/panels/inspector.h"
#include "tau/reflection.h"
#include "tau/render/settings.h"

#include <string>

namespace tau::editor::graphics_settings
{
    void draw(world_t& world, editor_context_t& ctx)
    {
        if (!ctx.show_graphics_settings) { return; }

        ImGui::SetNextWindowSize(ImVec2(380.0f, 320.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Graphics Settings", &ctx.show_graphics_settings))
        {
            const std::string current(renderer::settings::preset());
            if (ImGui::BeginCombo("Preset", current.c_str()))
            {
                for (const std::string& name : renderer::settings::presets())
                {
                    if (ImGui::Selectable(name.c_str(), name == current)) { renderer::settings::select_preset(name); }
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Field edits last until the preset changes.");
            ImGui::Separator();

            // the engine's context: types live there, not in the editor's own
            reflection::any_t value{*world.reflection_ctx, renderer::settings::get()};
            if (panels::draw_fields(world, value))
            {
                renderer::settings::set(value.cast<renderer::graphics_settings_t>());
            }
        }
        ImGui::End();
    }
} // namespace tau::editor::graphics_settings
