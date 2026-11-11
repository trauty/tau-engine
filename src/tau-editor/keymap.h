#pragma once

#include "imgui.h"
#include "tau/defines.h"

#include <string>
#include <string_view>

// every editor shortcut, by action, so they can be rebound in one place
namespace tau::editor::keymap
{
    enum class action_e : u8_t
    {
        UNDO,
        REDO,
        SAVE,
        DELETE_SELECTION,
        DUPLICATE,
        RENAME,
        FRAME_SELECTION,
        DESELECT,
        SELECT_TOOL,
        MOVE_TOOL,
        ROTATE_TOOL,
        SCALE_TOOL,
        COUNT,
    };

    struct binding_t
    {
        const char* id = "";
        const char* name = "";
        ImGuiKeyChord primary = 0;
        ImGuiKeyChord secondary = 0;
    };

    const binding_t& get(action_e action);
    const binding_t& get_default(action_e action);
    void set(action_e action, ImGuiKeyChord primary, ImGuiKeyChord secondary);
    void reset_defaults();

    // false while a text field has the keyboard or a binding is being captured
    bool pressed(action_e action, bool repeat = false);
    void set_capturing(bool capturing);
    std::string label(action_e action);
    std::string chord_name(ImGuiKeyChord chord);
    ImGuiKeyChord parse_chord(std::string_view text);
} // namespace tau::editor::keymap
