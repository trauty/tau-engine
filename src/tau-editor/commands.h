#pragma once

#include "tau-editor/keymap.h"
#include "tau/defines.h"

#include <string>

namespace tau
{
    struct world_t;
    struct editor_context_t;
} // namespace tau

// everything that changes what the editor has open
namespace tau::editor::commands
{
    enum class command_e : u8_t
    {
        PLAY,
        STOP,
        NEW_SCENE,
        // the file dialog, LOAD_SCENE with its answer
        OPEN_SCENE,
        LOAD_SCENE,
        // assets, then the open prefab or scene
        SAVE,
        // the file dialog, SAVE_SCENE_TO with its answer
        SAVE_SCENE_AS,
        SAVE_SCENE_TO,
        // path, guid: the entity to select inside it
        OPEN_PREFAB,
        CLOSE_PREFAB,
        OPEN_PROJECT,
        QUIT,
        RECOMPILE,
        CANCEL_BUILD,
        CAPTURE_GPU_FRAME,
        COUNT,
    };

    // unsaved work a command throws away, asked about first
    enum class discards_e : u8_t
    {
        NOTHING,
        SCENE,
        EVERYTHING,
    };

    struct request_t
    {
        command_e command = command_e::COUNT;
        std::string path;
        u64_t guid = 0;
        bool confirmed = false;
    };

    struct command_t
    {
        const char* label = "";
        keymap::action_e shortcut = keymap::action_e::COUNT;
        discards_e discards = discards_e::NOTHING;

        bool (*enabled)(const editor_context_t& ctx) = nullptr;
        void (*run)(world_t& world, editor_context_t& ctx, const request_t& request) = nullptr;
    };

    void set(command_e command, const command_t& def);
    bool enabled(const editor_context_t& ctx, command_e command);

    // any thread, file dialogs answer from theirs
    void push(request_t request);
    void push(command_e command, std::string path = {}, u64_t guid = 0);

    // start of the frame
    void run_pending(world_t& world, editor_context_t& ctx);
    // pushes the commands whose shortcut was pressed
    void poll_shortcuts(const editor_context_t& ctx);

    // label, shortcut and enabled state from the command, push on click
    void menu_item(const editor_context_t& ctx, command_e command, const char* label = nullptr);
    bool button(const editor_context_t& ctx, command_e command, const char* label = nullptr, bool small = false);

    // modal for a command waiting on unsaved changes, after the panels
    void draw_unsaved_prompt(world_t& world, editor_context_t& ctx);
} // namespace tau::editor::commands
