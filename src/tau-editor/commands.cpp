#include "commands.h"

#include "imgui.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/history.h"
#include "tau-editor/prefab_stage.h"
#include "tau-editor/scene_io.h"
#include "tau/vfs.h"

#include <array>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

namespace tau::editor::commands
{
    namespace
    {
        constexpr std::size_t COUNT = static_cast<std::size_t>(command_e::COUNT);

        std::array<command_t, COUNT> g_defs;

        std::mutex g_queue_mutex;
        std::vector<request_t> g_queue;

        // waits on the unsaved prompt
        std::optional<request_t> g_parked;

        const command_t& def_of(command_e command) { return g_defs[static_cast<std::size_t>(command)]; }

        bool has_unsaved(discards_e discards)
        {
            switch (discards)
            {
            case discards_e::NOTHING: return false;
            case discards_e::SCENE: return history::is_dirty(history::OPEN_SCENE);
            case discards_e::EVERYTHING: return history::any_dirty();
            }
            return false;
        }

        void run_one(world_t& world, editor_context_t& ctx, const request_t& request)
        {
            if (request.command >= command_e::COUNT || !enabled(ctx, request.command)) { return; }
            const command_t& def = def_of(request.command);
            if (!def.run) { return; }

            if (def.discards != discards_e::NOTHING && !request.confirmed)
            {
                prefab_stage::close(world, ctx);
                if (has_unsaved(def.discards))
                {
                    g_parked = request;
                    return;
                }
            }

            def.run(world, ctx, request);
        }

        // false when something stayed unsaved
        bool save_unsaved(world_t& world, editor_context_t& ctx, discards_e discards)
        {
            bool ok = discards != discards_e::EVERYTHING || history::save_assets();
            if (history::is_dirty(history::OPEN_SCENE))
            {
                if (ctx.current_scene_path.empty())
                {
                    push(command_e::SAVE_SCENE_AS);
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
    } // namespace

    void set(command_e command, const command_t& def)
    {
        if (command < command_e::COUNT) { g_defs[static_cast<std::size_t>(command)] = def; }
    }

    bool enabled(const editor_context_t& ctx, command_e command)
    {
        if (command >= command_e::COUNT) { return false; }
        const command_t& def = def_of(command);
        return def.run && (!def.enabled || def.enabled(ctx));
    }

    void push(request_t request)
    {
        const std::lock_guard lock(g_queue_mutex);
        g_queue.push_back(std::move(request));
    }

    void push(command_e command, std::string path, u64_t guid)
    { push(request_t{.command = command, .path = std::move(path), .guid = guid}); }

    void run_pending(world_t& world, editor_context_t& ctx)
    {
        std::vector<request_t> batch;
        {
            const std::lock_guard lock(g_queue_mutex);
            batch.swap(g_queue);
        }
        for (const request_t& request : batch) { run_one(world, ctx, request); }
    }

    void poll_shortcuts(const editor_context_t& ctx)
    {
        for (std::size_t i = 0; i < COUNT; i++)
        {
            const command_e command = static_cast<command_e>(i);
            const command_t& def = g_defs[i];
            if (def.shortcut != keymap::action_e::COUNT && enabled(ctx, command) && keymap::pressed(def.shortcut))
            {
                push(command);
            }
        }
    }

    void menu_item(const editor_context_t& ctx, command_e command, const char* label)
    {
        const command_t& def = def_of(command);
        const std::string shortcut = def.shortcut != keymap::action_e::COUNT ? keymap::label(def.shortcut) : "";
        if (ImGui::MenuItem(label ? label : def.label, shortcut.empty() ? nullptr : shortcut.c_str(), false,
                            enabled(ctx, command)))
        {
            push(command);
        }
    }

    bool button(const editor_context_t& ctx, command_e command, const char* label, bool small)
    {
        const char* text = label ? label : def_of(command).label;
        ImGui::BeginDisabled(!enabled(ctx, command));
        const bool clicked = small ? ImGui::SmallButton(text) : ImGui::Button(text);
        ImGui::EndDisabled();

        if (clicked) { push(command); }
        return clicked;
    }

    void draw_unsaved_prompt(world_t& world, editor_context_t& ctx)
    {
        if (!g_parked) { return; }

        constexpr const char* POPUP = "Unsaved Changes";
        if (!ImGui::IsPopupOpen(POPUP)) { ImGui::OpenPopup(POPUP); }

        // imgui centres a modal only the first time
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
        if (!ImGui::BeginPopupModal(POPUP, nullptr, flags)) { return; }

        const discards_e discards = def_of(g_parked->command).discards;
        std::vector<std::string> scene;
        std::vector<std::string> project_assets;
        std::vector<std::string> engine_assets;
        for (const history::doc_t doc : history::dirty_docs())
        {
            if (doc.kind == history::doc_kind_e::SCENE) { scene.push_back(doc_label(ctx, doc)); }
            else if (doc.kind == history::doc_kind_e::ASSET && discards == discards_e::EVERYTHING)
            {
                const std::string path = history::asset_path(doc);
                const std::optional<vfs::asset_path_parts_t> parts = vfs::split_asset_path(path);
                (parts && parts->ns == vfs::ENGINE_NS ? engine_assets : project_assets).push_back(path);
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

        bool go_on = false;
        bool close = false;

        if (ImGui::Button("Save"))
        {
            go_on = save_unsaved(world, ctx, discards);
            close = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save"))
        {
            go_on = true;
            close = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { close = true; }

        if (close)
        {
            if (go_on)
            {
                g_parked->confirmed = true;
                push(std::move(*g_parked));
            }
            g_parked.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
} // namespace tau::editor::commands
