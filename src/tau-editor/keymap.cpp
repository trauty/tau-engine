#include "keymap.h"

#include "imgui_internal.h"

#include <array>
#include <utility>

namespace tau::editor::keymap
{
    namespace
    {
        constexpr std::size_t COUNT = static_cast<std::size_t>(action_e::COUNT);

        const std::array<binding_t, COUNT> DEFAULTS = {
            {
             {"undo", "Undo", ImGuiMod_Ctrl | ImGuiKey_Z},
             {"redo", "Redo", ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z},
             {"save", "Save", ImGuiMod_Ctrl | ImGuiKey_S},
             {"delete", "Delete", ImGuiKey_Delete},
             {"duplicate", "Duplicate", ImGuiMod_Ctrl | ImGuiKey_D},
             {"rename", "Rename", ImGuiKey_F2},
             {"frame_selection", "Frame Selection", ImGuiKey_F},
             {"deselect", "Deselect", ImGuiKey_Escape},
             {"select_tool", "Select Tool", ImGuiKey_Q},
             {"move_tool", "Move Tool", ImGuiKey_W},
             {"rotate_tool", "Rotate Tool", ImGuiKey_E},
             {"scale_tool", "Scale Tool", ImGuiKey_R},
             {"close_prefab", "Close Prefab", 0},
             }
        };

        std::array<binding_t, COUNT> bindings = DEFAULTS;
        bool g_capturing = false;

        bool chord_pressed(ImGuiKeyChord chord, bool repeat)
        {
            if (chord == 0) { return false; }
            return ImGui::IsKeyChordPressed(chord, repeat ? ImGuiInputFlags_Repeat : ImGuiInputFlags_None,
                                            ImGuiKeyOwner_Any);
        }
    } // namespace

    const binding_t& get(action_e action) { return bindings[static_cast<std::size_t>(action)]; }
    const binding_t& get_default(action_e action) { return DEFAULTS[static_cast<std::size_t>(action)]; }

    void set(action_e action, ImGuiKeyChord primary, ImGuiKeyChord secondary)
    {
        binding_t& binding = bindings[static_cast<std::size_t>(action)];
        binding.primary = primary;
        binding.secondary = secondary;
    }

    void reset_defaults() { bindings = DEFAULTS; }

    bool pressed(action_e action, bool repeat)
    {
        if (g_capturing || ImGui::GetIO().WantTextInput) { return false; }

        const binding_t& binding = get(action);
        return chord_pressed(binding.primary, repeat) || chord_pressed(binding.secondary, repeat);
    }

    std::string chord_name(ImGuiKeyChord chord) { return chord != 0 ? ImGui::GetKeyChordName(chord) : ""; }

    std::string label(action_e action) { return chord_name(get(action).primary); }

    void set_capturing(bool capturing) { g_capturing = capturing; }

    ImGuiKeyChord parse_chord(std::string_view text)
    {
        constexpr std::pair<std::string_view, ImGuiKeyChord> MODS[] = {
            {"Ctrl+",  ImGuiMod_Ctrl },
            {"Shift+", ImGuiMod_Shift},
            {"Alt+",   ImGuiMod_Alt  },
            {"Super+", ImGuiMod_Super},
        };

        ImGuiKeyChord chord = 0;
        for (const auto& [prefix, mod] : MODS)
        {
            if (text.starts_with(prefix))
            {
                chord |= mod;
                text.remove_prefix(prefix.size());
            }
        }

        for (i32 key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++)
        {
            if (text == ImGui::GetKeyName(static_cast<ImGuiKey>(key))) { return chord | key; }
        }
        return 0;
    }
} // namespace tau::editor::keymap
