#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl3.h"
#include "imgui_backend.h"
#include "imgui_internal.h"
#include "tau-editor/asset_editors.h"
#include "tau-editor/asset_files.h"
#include "tau-editor/asset_watch.h"
#include "tau-editor/commands.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/game_code.h"
#include "tau-editor/handles.h"
#include "tau-editor/history.h"
#include "tau-editor/material_edit.h"
#include "tau-editor/panels/asset_browser.h"
#include "tau-editor/panels/console.h"
#include "tau-editor/panels/hierarchy.h"
#include "tau-editor/panels/history_panel.h"
#include "tau-editor/panels/inspector.h"
#include "tau-editor/panels/main_menu_bar.h"
#include "tau-editor/panels/toolbar.h"
#include "tau-editor/panels/viewport.h"
#include "tau-editor/picking.h"
#include "tau-editor/play_mode.h"
#include "tau-editor/prefab_edit.h"
#include "tau-editor/prefab_stage.h"
#include "tau-editor/project_manager.h"
#include "tau-editor/project_open.h"
#include "tau-editor/scene_io.h"
#include "tau-editor/settings.h"
#include "tau-editor/systems/camera.h"
#include "tau-editor/viewport_input.h"
#include "tau/asset.h"
#include "tau/engine.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/physics/physics.h"
#include "tau/rendering/renderer.h"
#include "tau/rendering/renderer_types.h"
#include "tau/rendering/vulkan.h"
#include "tau/systems/camera.h"
#include "tau/vfs.h"
#include "tau/window.h"
#include "tau/world.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <filesystem>
#include <string>

using tau::operator""_h;

static bool process_editor_event(const SDL_Event& event, tau::editor_context_t& ctx)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
    ImGuiIO& io = ImGui::GetIO();

    // window close asks when unsaved, signal quit does not
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
    {
        tau::editor::commands::push(tau::editor::commands::command_e::QUIT);
        return true;
    }

    if (ctx.cur_mode == tau::editor_mode_e::EDIT) { return false; }

    bool ignore_mouse =
        io.WantCaptureMouse && (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                                event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_WHEEL);

    bool ignore_keyboard =
        io.WantCaptureKeyboard &&
        (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_TEXT_INPUT);

    return ignore_mouse || ignore_keyboard;
}

static void update_window_title(const tau::editor_context_t& ctx)
{
    static std::string current;

    std::string scene = ctx.current_scene_path.empty()
                            ? std::string("Untitled")
                            : std::filesystem::path(ctx.current_scene_path).filename().string();
    if (tau::editor::prefab_stage::is_open())
    {
        scene = std::filesystem::path(tau::assets::path_of(tau::editor::prefab_stage::prefab())).filename().string() +
                " in " + scene;
    }
    const std::string title = scene + (tau::editor::history::any_dirty() ? "*" : "") + " - tau editor";
    if (title == current) { return; }

    current = title;
    SDL_SetWindowTitle(tau::window::get_window(), title.c_str());
}

static ImGuiID default_dock_layout()
{
    // bump to override layouts saved in imgui.ini
    const ImGuiID dockspace_id = ImGui::GetID("MainDockSpace_v6");
    if (ImGui::DockBuilderGetNode(dockspace_id) != nullptr) { return dockspace_id; }

    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

    f32 view_h = ImGui::GetMainViewport()->Size.y;

    ImGuiID dock_main = dockspace_id;

    ImGuiID dock_toolbar = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Up, 32.0f / view_h, nullptr, &dock_main);
    constexpr f32 side_ratio = 0.22f;

    // before the bottom split so it spans full height
    ImGuiID dock_inspector = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, side_ratio, nullptr, &dock_main);
    ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, 0.28f, nullptr, &dock_main);
    // ratio of the width left after the inspector
    ImGuiID dock_hierarchy =
        ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, side_ratio / (1.0f - side_ratio), nullptr, &dock_main);

    ImGuiID dock_viewport = dock_main;

    ImGui::DockBuilderDockWindow("Toolbar", dock_toolbar);
    ImGui::DockBuilderDockWindow("Scene Viewport", dock_viewport);
    ImGui::DockBuilderDockWindow("Hierarchy", dock_hierarchy);
    ImGui::DockBuilderDockWindow("History", dock_hierarchy);
    ImGui::DockBuilderDockWindow("Inspector", dock_inspector);
    ImGui::DockBuilderDockWindow("Assets", dock_bottom);
    ImGui::DockBuilderDockWindow("Console", dock_bottom);

    // hierarchy in front
    ImGui::DockBuilderGetNode(dock_hierarchy)->SelectedTabId = ImHashStr("#TAB", 4, ImHashStr("Hierarchy"));
    ImGui::DockBuilderGetNode(dock_bottom)->SelectedTabId = ImHashStr("#TAB", 4, ImHashStr("Assets"));

    ImGui::DockBuilderFinish(dockspace_id);

    return dockspace_id;
}

// the editor camera is the primary view outside Play
static void submit_editor_view(tau::editor_context_t& ctx, VkExtent2D extent)
{
    const auto& ecam = ctx.editor_camera;
    const f32 aspect = extent.height ? (f32)extent.width / (f32)extent.height : 1.0f;
    tau::mat4_t view, proj, view_proj;
    tau::camera_system::build_view_projection(ecam.position, ecam.rotation.forward(), ecam.rotation.up(), ecam.fov_deg,
                                              aspect, ecam.near_plane, ecam.far_plane, view, proj, view_proj);
    ctx.view = view;
    ctx.view_proj = view_proj;

    tau::renderer::submit_color_view("PrimaryView"_h, view, proj, view_proj, ecam.position, "SceneColor"_h,
                                     "SceneDepth"_h, tau::renderer::ctx.render_extent, ecam.near_plane, ecam.far_plane);
}

static void update_editor_ui(tau::editor_context_t& ctx)
{
    if (!ctx.project_loaded)
    {
        tau::editor::commands::run_pending(tau::engine::get_active_world(), ctx);

        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        tau::editor::project_open::draw_gate(ctx);
        ImGui::Render();
        tau::editor::imgui::submit(ImGui::GetDrawData());

        const VkExtent2D extent = tau::renderer::ctx.render_extent;
        tau::renderer::submit_output_target("EditorViewport"_h, extent);
        submit_editor_view(ctx, extent);
        return;
    }

    tau::editor::game_code::update(ctx);

    // after a reload, which replaces the world
    tau::world_t& world = tau::engine::get_active_world();

    tau::editor::commands::run_pending(world, ctx);

    update_window_title(ctx);

    tau::editor::asset_watch::tick();

    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    static const ImGuiID dockspace_id = default_dock_layout();
    ImGui::DockSpaceOverViewport(dockspace_id, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);

    tau::editor::commands::poll_shortcuts(ctx);

    tau::editor::panels::draw_main_menu_bar(world, ctx);
    tau::editor::panels::draw_viewport(world, ctx);
    tau::editor::panels::draw_hierarchy(world, ctx);
    tau::editor::panels::draw_history(world, ctx);
    tau::editor::panels::draw_inspector(world, ctx);
    tau::editor::panels::draw_toolbar(world, ctx);
    // asset browser last, so it is in front
    tau::editor::panels::draw_console(world, ctx);
    tau::editor::panels::draw_asset_browser(world, ctx);

    for (tau::panel_draw_func custom_panel : ctx.custom_panels) { custom_panel(world, ctx); }
    tau::editor::commands::draw_unsaved_prompt(world, ctx);

    tau::editor::handles::update(world, ctx);
    tau::editor::viewport_input::update(world, ctx);
    tau::editor::picking::update(world, ctx);
    tau::editor::history::update(world, ctx);
    // after shortcuts: a key captured for a binding must not also fire the old one
    tau::editor::settings::draw(ctx);
    tau::editor::project_open::draw_manager(ctx);

    ImGui::Render();
    tau::editor::imgui::submit(ImGui::GetDrawData());

    tau::renderer::submit_output_target("EditorViewport"_h, {ctx.viewport_width, ctx.viewport_height});

    tau::renderer::set_view_debug_view("PrimaryView"_h, ctx.debug_view);
    tau::renderer::set_view_post_processing("PrimaryView"_h, ctx.post_processing);

    if (ctx.cur_mode == tau::editor_mode_e::EDIT)
    {
        tau::editor::camera_system::update(ctx.editor_camera, ctx.is_viewport_hovered);
        submit_editor_view(ctx, tau::renderer::ctx.logical_extent);
    }
}

int main(i32 argc, char** argv)
{
    tau::log::init();

    // before anything else logs, so the console opens with the whole session
    tau::log::set_history_capacity(4096);

    std::filesystem::path startup_project;
    bool have_startup_project = false;
    if (argc >= 2)
    {
        std::string resolved;
        have_startup_project = tau::editor::project_manager::resolve_project_arg(argv[1], resolved);
        if (have_startup_project) { startup_project = resolved; }
        else
        {
            TAU_LOG_ERROR("EDITOR", "No single .tauproject found at '{}'", argv[1]);
        }
    }

    // the editor keeps its state in its own engine folder
    const std::string user_dir = tau::editor::project_manager::user_dir();
    tau::vfs::mount("user://", user_dir);

    if (!tau::engine::init("tau-editor", 1280, 720)) { return -1; }

    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");

    volkInitialize();
    volkLoadInstance(tau::renderer::ctx.instance);
    volkLoadDevice(tau::renderer::ctx.device);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    static const std::string ini_file = user_dir + "imgui.ini";
    io.IniFilename = ini_file.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.Fonts->AddFontDefaultVector();
    ImGui_ImplSDL3_InitForVulkan(tau::window::get_window());
    tau::editor::imgui::init();
    tau::editor::imgui::init_multiviewport();

    static tau::editor_context_t editor_ctx;
    tau::editor::picking::init(editor_ctx);
    tau::editor::handles::init(editor_ctx);
    tau::editor::history::init(editor_ctx);
    tau::editor::asset_editors::register_builtin(editor_ctx);
    tau::editor::material_edit::init(editor_ctx);
    tau::editor::asset_files::init(editor_ctx);
    tau::editor::prefab_edit::init(editor_ctx);
    tau::editor::prefab_stage::init(editor_ctx);
    tau::editor::scene_io::init(editor_ctx);
    tau::editor::play_mode::init(editor_ctx);
    tau::editor::game_code::init(editor_ctx);
    tau::editor::project_open::init(editor_ctx);
    tau::editor::commands::set(tau::editor::commands::command_e::QUIT,
                               {.label = "Quit",
                                .discards = tau::editor::commands::discards_e::EVERYTHING,
                                .run = [](tau::world_t&, tau::editor_context_t&,
                                          const tau::editor::commands::request_t&) { tau::engine::exit(); }});
    // after the transform update, before the overlay
    tau::renderer::register_renderer_feature("EditorColliders"_h, tau::renderer::FEATURE_ORDER_OVERLAY - 1,
                                             [](tau::renderer::rendergraph_t&, tau::ecs::registry_t& reg)
                                             {
                                                 if (editor_ctx.show_colliders)
                                                 {
                                                     tau::physics::draw_colliders(reg, "PrimaryView"_h);
                                                 }
                                             });
    tau::editor::settings::load(editor_ctx);

    if (have_startup_project) { tau::editor::project_open::request(startup_project, editor_ctx); }

    tau::engine::set_event_callback([&](const SDL_Event& event) { return process_editor_event(event, editor_ctx); });

    tau::engine::set_ui_callback([&]() { update_editor_ui(editor_ctx); });

    tau::engine::set_post_render_callback(
        []()
        {
            ImGuiIO& io = ImGui::GetIO();
            if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
            {
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault(nullptr, nullptr);
            }
        });

    tau::engine::run();

    tau::editor::asset_watch::flush();

    tau::editor::project_open::cancel();

    vkDeviceWaitIdle(tau::renderer::ctx.device);

    tau::editor::imgui::shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    tau::engine::shutdown();

    // the current library and any that could not be released are left to the process exit

    return 0;
}
