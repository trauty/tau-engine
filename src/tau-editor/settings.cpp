#include "settings.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/keymap.h"
#include "tau-editor/project_manager.h"
#include "tau/log.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <json/json.hpp>
#include <optional>
#include <string>

namespace tau::editor::settings
{
    namespace
    {
        enum class theme_e : u8_t
        {
            DARK,
            LIGHT,
            CLASSIC,
            COUNT,
        };

        constexpr const char* THEME_IDS[] = {"dark", "light", "classic"};
        constexpr const char* THEME_NAMES[] = {"Dark", "Light", "Classic"};

        struct look_t
        {
            theme_e theme = theme_e::DARK;
            f32 ui_scale = 1.0f;
        };

        look_t look;

        struct capture_t
        {
            keymap::action_e action;
            bool secondary = false;
        };

        std::optional<capture_t> capture;

        constexpr std::size_t ACTION_COUNT = static_cast<std::size_t>(keymap::action_e::COUNT);

        std::string settings_path() { return project_manager::user_dir() + "editor_settings.json"; }

        f64 tidy(f32 value) { return std::round(static_cast<f64>(value) * 1e6) / 1e6; }

        void apply_look()
        {
            ImGuiStyle& style = ImGui::GetStyle();
            switch (look.theme)
            {
            case theme_e::LIGHT: ImGui::StyleColorsLight(&style); break;
            case theme_e::CLASSIC: ImGui::StyleColorsClassic(&style); break;
            default: ImGui::StyleColorsDark(&style); break;
            }

            // keeps platform windows looking like the main one
            style.WindowRounding = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w = 1.0f;
            style.FontScaleMain = look.ui_scale;
        }

        const char* clash(keymap::action_e action, ImGuiKeyChord chord)
        {
            for (std::size_t i = 0; i < ACTION_COUNT; i++)
            {
                const auto other = static_cast<keymap::action_e>(i);
                if (other == action) { continue; }

                const keymap::binding_t& binding = keymap::get(other);
                if (binding.primary == chord || binding.secondary == chord) { return binding.name; }
            }
            return nullptr;
        }

        bool binding_button(keymap::action_e action, bool secondary)
        {
            const keymap::binding_t& binding = keymap::get(action);
            const ImGuiKeyChord chord = secondary ? binding.secondary : binding.primary;
            const bool capturing = capture && capture->action == action && capture->secondary == secondary;
            bool changed = false;

            ImGui::PushID(secondary ? 1 : 0);

            const char* other = chord != 0 ? clash(action, chord) : nullptr;
            if (other) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f)); }

            const std::string text = capturing ? "Press keys..." : chord != 0 ? keymap::chord_name(chord) : "-";
            if (ImGui::Button((text + "##bind").c_str(), ImVec2(-FLT_MIN, 0.0f)))
            {
                capture = capture_t{action, secondary};
            }

            if (other)
            {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Also bound to %s", other); }
            }

            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                keymap::set(action, secondary ? binding.primary : 0, secondary ? 0 : binding.secondary);
                capture.reset();
                changed = true;
            }

            if (capturing)
            {
                for (i32 key = ImGuiKey_Keyboard_BEGIN; key < ImGuiKey_Keyboard_END; key++)
                {
                    if (ImGui::IsLRModKey(static_cast<ImGuiKey>(key)) ||
                        !ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false))
                    {
                        continue;
                    }

                    const ImGuiKeyChord pressed = key | ImGui::GetIO().KeyMods;
                    keymap::set(action, secondary ? binding.primary : pressed, secondary ? pressed : binding.secondary);
                    capture.reset();
                    changed = true;
                    break;
                }
                if (capture && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered())
                {
                    capture.reset();
                }
            }

            ImGui::PopID();
            return changed;
        }

        bool draw_keymap()
        {
            bool changed = false;
            ImGui::TextDisabled("Click a shortcut and press the keys, right click clears it.");

            constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV;
            if (ImGui::BeginTable("##keymap", 4, flags))
            {
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Alternative", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableHeadersRow();

                for (std::size_t i = 0; i < ACTION_COUNT; i++)
                {
                    const auto action = static_cast<keymap::action_e>(i);
                    const keymap::binding_t& binding = keymap::get(action);
                    const keymap::binding_t& fallback = keymap::get_default(action);

                    ImGui::TableNextRow();
                    ImGui::PushID(static_cast<i32>(i));

                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(binding.name);

                    ImGui::TableNextColumn();
                    changed |= binding_button(action, false);
                    ImGui::TableNextColumn();
                    changed |= binding_button(action, true);

                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(binding.primary == fallback.primary &&
                                         binding.secondary == fallback.secondary);
                    if (ImGui::SmallButton("Reset"))
                    {
                        keymap::set(action, fallback.primary, fallback.secondary);
                        changed = true;
                    }
                    ImGui::EndDisabled();

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            if (ImGui::Button("Reset All"))
            {
                keymap::reset_defaults();
                capture.reset();
                changed = true;
            }
            return changed;
        }

        bool draw_appearance()
        {
            bool changed = false;

            i32 theme = static_cast<i32>(look.theme);
            if (ImGui::Combo("Theme", &theme, THEME_NAMES, static_cast<i32>(theme_e::COUNT)))
            {
                look.theme = static_cast<theme_e>(theme);
                apply_look();
                changed = true;
            }

            if (ImGui::SliderFloat("UI Scale", &look.ui_scale, 0.75f, 2.0f, "%.2f")) { apply_look(); }
            changed |= ImGui::IsItemDeactivatedAfterEdit();

            return changed;
        }

        bool draw_editing(editor_context_t& ctx)
        {
            bool changed = false;

            ImGui::SeparatorText("Snapping, while Ctrl is held");
            ImGui::DragFloat("Move", &ctx.snap_move, 0.05f, 0.001f, 1000.0f, "%.3f");
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::DragFloat("Rotate", &ctx.snap_rotate_deg, 0.5f, 0.1f, 180.0f, "%.1f deg");
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::DragFloat("Scale", &ctx.snap_scale, 0.01f, 0.001f, 10.0f, "%.3f");
            changed |= ImGui::IsItemDeactivatedAfterEdit();

            return changed;
        }
    } // namespace

    void load(editor_context_t& ctx)
    {
        keymap::reset_defaults();
        look = {};

        std::ifstream file(settings_path());
        const nlohmann::json data =
            file.is_open() ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json::object();

        if (data.is_object())
        {
            // stores only diffs from defaults
            const nlohmann::json bindings = data.value("keymap", nlohmann::json::object());
            for (std::size_t i = 0; i < ACTION_COUNT; i++)
            {
                const auto action = static_cast<keymap::action_e>(i);
                const nlohmann::json entry = bindings.value(keymap::get_default(action).id, nlohmann::json{});
                if (!entry.is_array()) { continue; }

                auto chord_at = [&](std::size_t index)
                {
                    return index < entry.size() && entry[index].is_string()
                               ? keymap::parse_chord(entry[index].get<std::string>())
                               : 0;
                };
                keymap::set(action, chord_at(0), chord_at(1));
            }

            const std::string theme = data.value("theme", std::string(THEME_IDS[0]));
            for (std::size_t i = 0; i < std::size(THEME_IDS); i++)
            {
                if (theme == THEME_IDS[i]) { look.theme = static_cast<theme_e>(i); }
            }
            look.ui_scale = std::clamp(data.value("ui_scale", 1.0f), 0.75f, 2.0f);

            const nlohmann::json snap = data.value("snap", nlohmann::json::object());
            ctx.snap_move = snap.value("move", ctx.snap_move);
            ctx.snap_rotate_deg = snap.value("rotate_deg", ctx.snap_rotate_deg);
            ctx.snap_scale = snap.value("scale", ctx.snap_scale);
        }
        else
        {
            TAU_LOG_WARN("EDITOR", "{} is not valid JSON, using the default settings", settings_path());
        }

        apply_look();
    }

    void save(const editor_context_t& ctx)
    {
        nlohmann::json bindings = nlohmann::json::object();
        for (std::size_t i = 0; i < ACTION_COUNT; i++)
        {
            const auto action = static_cast<keymap::action_e>(i);
            const keymap::binding_t& binding = keymap::get(action);
            const keymap::binding_t& fallback = keymap::get_default(action);
            if (binding.primary == fallback.primary && binding.secondary == fallback.secondary) { continue; }

            bindings[fallback.id] = {keymap::chord_name(binding.primary), keymap::chord_name(binding.secondary)};
        }

        const nlohmann::json data = {
            {"keymap",   std::move(bindings)                            },
            {"theme",    THEME_IDS[static_cast<std::size_t>(look.theme)]},
            {"ui_scale", tidy(look.ui_scale)                            },
            {"snap",
             {{"move", tidy(ctx.snap_move)},
              {"rotate_deg", tidy(ctx.snap_rotate_deg)},
              {"scale", tidy(ctx.snap_scale)}}                          },
        };

        std::ofstream file(settings_path());
        if (!file.is_open())
        {
            TAU_LOG_ERROR("EDITOR", "Cannot write {}", settings_path());
            return;
        }
        file << data.dump(4) << '\n';
    }

    void draw(editor_context_t& ctx)
    {
        bool changed = false;

        if (ctx.show_settings)
        {
            ImGui::SetNextWindowSize(ImVec2(560.0f, 440.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Settings", &ctx.show_settings))
            {
                if (ImGui::BeginTabBar("##settings_tabs"))
                {
                    if (ImGui::BeginTabItem("Keymap"))
                    {
                        changed |= draw_keymap();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Appearance"))
                    {
                        changed |= draw_appearance();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Editing"))
                    {
                        changed |= draw_editing(ctx);
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }
            ImGui::End();
        }

        if (!ctx.show_settings) { capture.reset(); }
        keymap::set_capturing(capture.has_value());

        if (changed) { save(ctx); }
    }
} // namespace tau::editor::settings
