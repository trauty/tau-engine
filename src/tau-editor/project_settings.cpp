#include "project_settings.h"

#include "imgui.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/import_edit.h"
#include "tau-editor/panels/inspector.h"
#include "tau/asset.h"
#include "tau/assets/import_settings.h"
#include "tau/log.h"
#include "tau/project.h"
#include "tau/reflection_json.h"

#include <string>

namespace tau::editor::project_settings
{
    namespace
    {
        // working copy of import_defaults while the window is open
        nlohmann::json g_import = nlohmann::json::object();
        std::string g_loaded_for;
        bool g_dirty = false;

        void load(const editor_context_t& ctx)
        {
            g_import = import_edit::project_defaults();
            g_loaded_for = ctx.project_file;
            g_dirty = false;
        }

        void draw_import_defaults(world_t& world)
        {
            ImGui::TextDisabled("Under every asset's own import settings; assets follow a changed default.");

            for (const asset_type_t& type : assets::types())
            {
                if (type.import_type == 0) { continue; }

                const reflection::type_t settings_type = import_settings::type_of(type.id);
                if (!settings_type) { continue; }

                const std::string key(type.key);
                ImGui::PushID(key.c_str());
                if (ImGui::TreeNodeEx(std::string(type.display_name).c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                {
                    const reflection::any_t defaults = settings_type.construct();
                    reflection::any_t value = defaults;
                    if (g_import.contains(key)) { reflection_json::apply(value, g_import[key]); }

                    if (panels::draw_fields(world, value))
                    {
                        nlohmann::json layer = import_settings::meta_object(value, defaults);
                        if (layer.empty()) { g_import.erase(key); }
                        else
                        {
                            g_import[key] = std::move(layer);
                        }
                        g_dirty = true;
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }

        void save(const editor_context_t& ctx)
        {
            if (!project_t::set_import_defaults(ctx.project_file, g_import)) { return; }

            import_edit::set_project_defaults(g_import);
            g_dirty = false;
            TAU_LOG_INFO("PROJECT", "Saved the project settings of {}", ctx.project_file);
        }
    } // namespace

    void draw(world_t& world, editor_context_t& ctx)
    {
        if (!ctx.show_project_settings) { return; }
        if (!ctx.project_loaded)
        {
            ctx.show_project_settings = false;
            return;
        }
        if (g_loaded_for != ctx.project_file) { load(ctx); }

        ImGui::SetNextWindowSize(ImVec2(420.0f, 360.0f), ImGuiCond_FirstUseEver);
        const std::string title = std::string("Project Settings") + (g_dirty ? "*" : "") + "###project_settings";
        if (ImGui::Begin(title.c_str(), &ctx.show_project_settings))
        {
            const bool editing = ctx.cur_mode == editor_mode_e::EDIT;

            ImGui::BeginDisabled(!g_dirty || !editing);
            if (ImGui::Button("Save")) { save(ctx); }
            ImGui::SameLine();
            if (ImGui::Button("Revert")) { load(ctx); }
            ImGui::EndDisabled();

            if (ImGui::CollapsingHeader("Import Defaults", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::BeginDisabled(!editing);
                draw_import_defaults(world);
                ImGui::EndDisabled();
            }
        }
        ImGui::End();
    }
} // namespace tau::editor::project_settings
