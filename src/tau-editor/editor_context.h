#pragma once

#include "tau-editor/selection.h"
#include "tau-editor/systems/camera.h"
#include "tau/ecs_fwd.h"
#include "tau/math.h"
#include "tau/rhi/renderer_types.h"

#include <json/json.hpp>
#include <vector>

namespace tau
{
    struct world_t;

    enum class editor_mode_e
    {
        EDIT,
        PLAY,
        PAUSED,
    };

    enum class handle_mode_e
    {
        NONE,
        MOVE,
        ROTATE,
        SCALE,
    };

    struct editor_context_t;

    using panel_draw_func = void (*)(tau::world_t& world, editor_context_t& ctx);

    struct editor_context_t
    {
        // changes through the PLAY and STOP commands only
        editor_mode_e cur_mode = editor_mode_e::EDIT;

        tau::editor::editor_camera_t editor_camera;

        tau::editor::selection_t selection;

        bool is_viewport_hovered = false;
        bool is_viewport_focused = false;

        u32_t viewport_width = 0;
        u32_t viewport_height = 0;

        vec2 viewport_screen_pos;
        vec2 viewport_screen_size;
        mat4 view;
        mat4 view_proj;

        handle_mode_e handle_mode = handle_mode_e::MOVE;
        bool handles_local = true;
        // the left button goes to the handles while either is set
        bool handle_hovered = false;
        bool handle_dragging = false;
        f32 snap_move = 0.5f;
        f32 snap_rotate_deg = 15.0f;
        f32 snap_scale = 0.1f;

        bool click_pending = false;
        vec2 click_pos;
        bool click_additive = false;
        bool scroll_to_selection = false;

        tau::renderer::debug_view_e debug_view = tau::renderer::debug_view_e::OFF;

        bool post_processing = true;
        bool show_colliders = false;

        std::vector<panel_draw_func> custom_panels;

        std::string project_dir;
        std::string project_file;
        std::string project_assets_dir;
        std::string project_cooked_dir;

        std::string inspected_asset;
        // set through scene_io::set_path
        std::string current_scene_path;

        bool project_loaded = false;

        // a rebuilt game library waits for Edit, unless replay_on_reload stops Play for it and starts it again
        bool reload_pending = false;
        bool replay_on_reload = false;

        bool recompiling = false;
        bool build_cancelling = false;

        bool show_project_manager = false;
        bool show_settings = false;
        bool show_project_settings = false;
    };
} // namespace tau