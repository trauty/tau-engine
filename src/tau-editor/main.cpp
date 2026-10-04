#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl3.h"
#include "imgui_backend.h"
#include "imgui_internal.h"
#include "tau-editor/asset_cook.h"
#include "tau-editor/asset_watch.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/panels/console.h"
#include "tau-editor/panels/hierarchy.h"
#include "tau-editor/panels/inspector.h"
#include "tau-editor/panels/main_menu_bar.h"
#include "tau-editor/panels/toolbar.h"
#include "tau-editor/panels/viewport.h"
#include "tau-editor/project_manager.h"
#include "tau-editor/systems/camera.h"
#include "tau/asset_meta.h"
#include "tau/asset_table.h"
#include "tau/engine.h"
#include "tau/game.h"
#include "tau/hash.h"
#include "tau/input.h"
#include "tau/jobs.h"
#include "tau/log.h"
#include "tau/os.h"
#include "tau/project.h"
#include "tau/reflection.h"
#include "tau/rendering/renderer.h"
#include "tau/rendering/renderer_types.h"
#include "tau/rendering/vulkan.h"
#include "tau/serialization.h"
#include "tau/systems/camera.h"
#include "tau/tween.h"
#include "tau/vfs.h"
#include "tau/window.h"
#include "tau/world.h"

#include "json/json.hpp"
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_stdinc.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <system_error>

#if TAU_PLATFORM_WIN
    #include <windows.h>
#elif TAU_PLATFORM_LINUX
    #include <dlfcn.h>
#endif

std::filesystem::path source_lib_path;
std::filesystem::path trigger_path;
std::string source_lib_name;
std::string cur_temp_lib_name = "";

tau::os::lib_handle_t game_lib = nullptr;

// game libraries that could not be unloaded safely, kept loaded until exit with a warning
std::vector<tau::os::lib_handle_t> g_retired_game_libs;
std::filesystem::file_time_type last_reload_time;

tau::editor_mode_e g_prev_mode = tau::editor_mode_e::EDIT;
// a rebuild stopped Play to reload, so Play starts again after it
bool g_replay_after_reload = false;
// entities as Play started, to find the selection again on Stop
std::vector<tau::ecs::entity_t> g_play_order;

typedef void (*editor_init_func)(tau::world_t*, tau::editor_context_t*, ImGuiContext*);

// runs xmake from the project's own directory, as it writes its configuration where it runs
// one build at a time, so one token stops whichever runs
// reset on the UI thread before a build starts so an immediate Cancel counts
static tau::os::process_cancel_t g_build_cancel;

static bool run_xmake(const std::string& project_dir, const std::vector<std::string>& args, const char* what)
{
    // markers are the ones clang, gcc, msvc and xmake print, so error_handler.cpp stays info
    auto forward = [](std::string_view raw)
    {
        const std::string line = tau::log::strip_ansi(raw);
        if (line.empty()) { return; }

        auto has = [&line](const char* marker) { return line.find(marker) != std::string::npos; };
        if (has("error:") || has(": error")) { TAU_LOG_ERROR("BUILD", "{}", line); }
        else if (has("warning:") || has(": warning")) { TAU_LOG_WARN("BUILD", "{}", line); }
        else
        {
            TAU_LOG_INFO("BUILD", "{}", line);
        }
    };

    const tau::os::process_result_t run = tau::os::run_process("xmake", args, project_dir, forward, &g_build_cancel);
    if (!run.started)
    {
        TAU_LOG_ERROR("EDITOR", "Could not start xmake to {} the project -- is it on PATH?", what);
        return false;
    }

    if (run.cancelled)
    {
        TAU_LOG_WARN("EDITOR", "Cancelled: xmake {} stopped", what);
        return false;
    }

    if (run.exit_code != 0)
    {
        TAU_LOG_ERROR("EDITOR", "xmake could not {} the project (exit {})", what, run.exit_code);
        return false;
    }
    return true;
}

bool build_project(const std::filesystem::path& project_file, const std::string& engine_dir)
{
    tau::project_t project;
    if (!tau::project_t::load(project_file, project))
    {
        TAU_LOG_ERROR("EDITOR", "Failed to load project for build: {}", project_file.string());
        return false;
    }
    const std::string proj = project.project_dir.string();
    const std::string target = project.project_name;

#ifndef TAU_EDITOR_MODE
    #define TAU_EDITOR_MODE "debug"
#endif
    const std::string mode = TAU_EDITOR_MODE;

    if (engine_dir.empty())
    {
        TAU_LOG_ERROR("EDITOR", "Could not find this editor's engine folder to configure the project with");
        return false;
    }

    // xmake rejects an option the project does not declare
    std::ifstream build_script(project.project_dir / "xmake.lua");
    const std::string script((std::istreambuf_iterator<char>(build_script)), std::istreambuf_iterator<char>());
    if (script.find("tau_engine_dir") == std::string::npos)
    {
        TAU_LOG_ERROR("EDITOR",
                      "{} finds the engine the old way. Replace its engine lookup with the tau_engine_dir option "
                      "from the engine's template/xmake.lua.",
                      (project.project_dir / "xmake.lua").string());
        return false;
    }

    // records this editor's engine in the project, so terminal and IDE builds use it too
    TAU_LOG_INFO("EDITOR", "Configuring project in {}", proj);
    // explicit project dir as well as the working directory: with no .xmake of its own xmake walks up
    // to an outer project, e.g. the engine's samples
    if (!run_xmake(proj, {"f", "-P", ".", "-y", "-m", mode, "--tau_engine_dir=" + engine_dir}, "configure"))
    {
        return false;
    }

    TAU_LOG_INFO("EDITOR", "Building {}", target);
    if (!run_xmake(proj, {"build", "-P", ".", target}, "build")) { return false; }

    TAU_LOG_INFO("EDITOR", "Project built successfully");
    return true;
}

struct game_library_t
{
    tau::os::lib_handle_t handle = nullptr;
    std::string loaded_copy;
    game_register_types_func register_types = nullptr;
    game_init_func init = nullptr;
    game_update_func update = nullptr;
    game_shutdown_func shutdown = nullptr;
    editor_init_func editor_init = nullptr;
};

// loading ran the library's static initialisers, which added reflection registrations and on load hooks
static void close_unused_library(const game_library_t& lib)
{
    void* module = tau::os::module_base_of_lib(lib.handle);
    tau::reflection::remove_registrations_of(module);
    tau::game::remove_on_load_of(module);
    tau::os::free_lib(lib.handle);

    std::error_code ec;
    std::filesystem::remove(lib.loaded_copy, ec);
}

// loads a copy, so the next build can overwrite the original while this one runs
static bool open_game_library(game_library_t& out)
{
    std::error_code trigger_ec;
    last_reload_time = std::filesystem::last_write_time(trigger_path, trigger_ec);

    static int reload_counter = 0;
    std::filesystem::path temp_lib_path =
        source_lib_path.parent_path() / (source_lib_path.stem().string() + "-loaded-" +
                                         std::to_string(reload_counter++) + source_lib_path.extension().string());
    out.loaded_copy = temp_lib_path.string();

    std::error_code ec;
    std::filesystem::copy_file(source_lib_name, out.loaded_copy, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        TAU_LOG_WARN("EDITOR", "Could not copy game logic lib '{}': {}", source_lib_name, ec.message());
        return false;
    }

    out.handle = tau::os::load_lib(out.loaded_copy.c_str());
    if (!out.handle)
    {
#if TAU_PLATFORM_WIN
        TAU_LOG_ERROR("EDITOR", "Failed to load library with name: {}; Error: {}", out.loaded_copy, GetLastError());
#elif TAU_PLATFORM_LINUX
        TAU_LOG_ERROR("EDITOR", "Failed to load library with name: {}; Error: {}", out.loaded_copy, dlerror());
#endif
        std::filesystem::remove(out.loaded_copy, ec);
        return false;
    }

    out.register_types =
        (game_register_types_func)tau::os::get_proc_address(out.handle, "tau_game_register_types_internal");
    out.init = (game_init_func)tau::os::get_proc_address(out.handle, "tau_game_init_internal");
    out.update = (game_update_func)tau::os::get_proc_address(out.handle, "tau_game_update_internal");
    out.shutdown = (game_shutdown_func)tau::os::get_proc_address(out.handle, "tau_game_shutdown_internal");
    out.editor_init = (editor_init_func)tau::os::get_proc_address(out.handle, "tau_editor_init");

    if (!out.register_types || !out.init || !out.update || !out.shutdown)
    {
        TAU_LOG_ERROR("EDITOR", "Could not find one or more game logic functions necessary");
        close_unused_library(out);
        return false;
    }

    const char* lib_stamp = tau::game::library_stamp(out.handle);
    if (!lib_stamp || std::string_view(lib_stamp) != tau::engine::build_stamp())
    {
        TAU_LOG_ERROR("EDITOR",
                      "The game library was built against a different engine build (library: {}, editor: {}). "
                      "Build the engine and the project again.",
                      lib_stamp ? lib_stamp : "none", tau::engine::build_stamp());
        close_unused_library(out);
        return false;
    }

    return true;
}

// unloads the game library after removing everything holding its code: input listeners, render features,
// on load hooks, reflection registrations, editor panels. the world it ran in was destroyed and reflection reset
// before this. running jobs would still dangle, then it keeps the library loaded with a warning
static void release_game_library(tau::editor_context_t& ctx)
{
    void* module = tau::os::module_base_of_lib(game_lib);

    const u32_t listeners = tau::input::remove_listeners_of(module);
    const u32_t features = tau::renderer::remove_features_of(module);
    const u32_t hooks = tau::game::remove_on_load_of(module);
    tau::reflection::remove_registrations_of(module);
    const auto panels =
        std::erase_if(ctx.custom_panels, [module](tau::panel_draw_func panel)
                      { return tau::os::module_base_of(reinterpret_cast<const void*>(panel)) == module; });

    ctx.game_register_types = nullptr;
    ctx.game_init = nullptr;
    ctx.game_update = nullptr;
    ctx.game_shutdown = nullptr;

    if (!tau::jobs::wait_idle(5000))
    {
        TAU_LOG_WARN("EDITOR", "Kept the previous game library loaded: jobs were still running after 5 s");
        g_retired_game_libs.push_back(game_lib);
        game_lib = nullptr;
        return;
    }

    tau::os::free_lib(game_lib);
    game_lib = nullptr;

    std::error_code ec;
    std::filesystem::remove(cur_temp_lib_name, ec);

    TAU_LOG_INFO("EDITOR",
                 "Unloaded the previous game library (released {} input listener(s), {} render feature(s), "
                 "{} on-load hook(s), {} panel(s))",
                 listeners, features, hooks, panels);
}

// the active world must be new: the library's types, then the scene, then its on load code
static std::vector<tau::ecs::entity_t> start_game_library(tau::editor_context_t& ctx, const game_library_t& lib,
                                                          const nlohmann::json* scene)
{
    game_lib = lib.handle;
    cur_temp_lib_name = lib.loaded_copy;
    ctx.game_register_types = lib.register_types;
    ctx.game_init = lib.init;
    ctx.game_update = lib.update;
    ctx.game_shutdown = lib.shutdown;

    tau::world_t& world = tau::engine::get_active_world();
    tau::reflection::register_types();
    ctx.game_register_types(&world);
    world.init();

    std::vector<tau::ecs::entity_t> created;
    if (scene) { created = tau::serialization::deserialize_scene(world, *scene); }

    tau::game::run_on_load(world);

    if (lib.editor_init)
    {
        ctx.custom_panels.clear();
        lib.editor_init(&world, &ctx, ImGui::GetCurrentContext());
    }

    return created;
}

// in Edit only: a reload keeps what saving the scene keeps
static void reload_game_library(tau::editor_context_t& ctx)
{
    TAU_LOG_INFO("EDITOR", "Build system signaled completion. Attempting hot reload...");

    game_library_t lib;
    if (!open_game_library(lib))
    {
        TAU_LOG_ERROR("EDITOR", "Kept the previous game code, the rebuilt library could not be loaded");
        return;
    }

    std::vector<tau::ecs::entity_t> order;
    const nlohmann::json scene = tau::serialization::serialize_scene(tau::engine::get_active_world(), &order);
    const std::size_t selected =
        static_cast<std::size_t>(std::find(order.begin(), order.end(), ctx.selected_entity) - order.begin());

    tau::tween::cancel_all();

    // the old world goes while its library is loaded, its pools and listeners run that library's code
    tau::engine::create_scene();
    tau::reflection::shutdown();
    release_game_library(ctx);

    const std::vector<tau::ecs::entity_t> created = start_game_library(ctx, lib, &scene);
    ctx.selected_entity = selected < created.size() ? created[selected] : tau::ecs::NULL_ENTITY;

    TAU_LOG_INFO("EDITOR", "Successfully hot-reloaded game lib");
}

static void begin_play(tau::editor_context_t& ctx)
{
    tau::world_t& world = tau::engine::get_active_world();

    ctx.world_backup = tau::serialization::serialize_scene(world, &g_play_order);
    world.begin_play();
    if (ctx.game_init) { ctx.game_init(&world); }
}

static void end_play(tau::editor_context_t& ctx)
{
    tau::world_t& world = tau::engine::get_active_world();

    if (ctx.game_shutdown) { ctx.game_shutdown(&world); }
    world.end_play();
    tau::tween::cancel_all();

    const std::size_t selected = static_cast<std::size_t>(
        std::find(g_play_order.begin(), g_play_order.end(), ctx.selected_entity) - g_play_order.begin());

    tau::serialization::clear_scene(world);
    const std::vector<tau::ecs::entity_t> created = tau::serialization::deserialize_scene(world, ctx.world_backup);
    ctx.selected_entity = selected < created.size() ? created[selected] : tau::ecs::NULL_ENTITY;
}

bool process_editor_event(const SDL_Event& event, tau::editor_context_t& ctx)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
    ImGuiIO& io = ImGui::GetIO();

    if (ctx.cur_mode == tau::editor_mode_e::EDIT) { return false; }

    bool ignore_mouse =
        io.WantCaptureMouse && (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                                event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_WHEEL);

    bool ignore_keyboard =
        io.WantCaptureKeyboard &&
        (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_TEXT_INPUT);

    return ignore_mouse || ignore_keyboard;
}

using tau::operator""_h;

bool open_project(const std::filesystem::path& project_file, tau::editor_context_t& ctx);

namespace
{
    // opening a project: build its library if missing, cook its assets, then open, the first two off the UI thread
    enum class gate_state_e
    {
        PICKING,
        UPDATING,
        BUILDING,
        COOKING,
    };

    gate_state_e g_gate = gate_state_e::PICKING;
    std::future<bool> g_build_future;
    std::future<bool> g_cook_future;
    std::string g_cook_error;
    tau::os::process_cancel_t g_cook_cancel;
    std::filesystem::path g_pending_project;

    std::future<bool> g_recompile_future;

    bool has_game_library(const std::filesystem::path& project_file)
    {
        tau::project_t project;
        return tau::project_t::load(project_file, project) && std::filesystem::exists(project.lib_path);
    }

    // before anything loads: cooked data may predate this engine (a format bump breaks materials until recooked)
    // a failure is reported and opening carries on, what still fails to load draws as the fallback
    void start_cook(const std::filesystem::path& project_file, tau::editor_context_t& ctx)
    {
        g_pending_project = project_file;
        g_gate = gate_state_e::COOKING;
        ctx.project_loaded = false;

        tau::editor::asset_cook::set_project(project_file.string());
        g_cook_cancel.reset();
        g_cook_future = std::async(std::launch::async,
                                   [] { return tau::editor::asset_cook::cook_project(g_cook_error, &g_cook_cancel); });
    }

    // always builds: an up to date project costs a moment, a library left from an older engine build crashes
    void start_build(const std::filesystem::path& project_file, tau::editor_context_t& ctx)
    {
        g_pending_project = project_file;
        g_gate = gate_state_e::BUILDING;
        ctx.project_loaded = false;
        tau::editor::project_manager::report_status("Building " + project_file.filename().string() + "...");
        const std::string eng = tau::editor::project_manager::engine_dir();
        g_build_cancel.reset();
        g_build_future = std::async(std::launch::async, [pf = project_file, eng] { return build_project(pf, eng); });
    }

    std::string g_update_question;

    // a project opens only in an editor of the engine version it is made for
    void request_open_project(const std::filesystem::path& project_file, tau::editor_context_t& ctx)
    {
        tau::project_t project;
        if (!tau::project_t::load(project_file, project))
        {
            tau::editor::project_manager::report_status("The project file could not be read, see console output");
            return;
        }

        const std::string engine_version = tau::major_minor(tau::engine::version());
        const int order =
            project.engine_version.empty() ? -1 : tau::compare_major_minor(project.engine_version, engine_version);
        if (order == 0)
        {
            start_build(project_file, ctx);
            return;
        }

        if (order > 0)
        {
            TAU_LOG_ERROR("EDITOR", "{} is made for tau {}, this editor is tau {}", project_file.filename().string(),
                          project.engine_version, engine_version);
            tau::editor::project_manager::report_status("The project needs a tau " + project.engine_version +
                                                        " editor, this one is " + engine_version);
            return;
        }

        g_pending_project = project_file;
        g_gate = gate_state_e::UPDATING;
        ctx.project_loaded = false;
        g_update_question = project.engine_version.empty()
                                ? project_file.filename().string() + " names no engine version. Make it a tau " +
                                      engine_version + " project?"
                                : project_file.filename().string() + " is made for tau " + project.engine_version +
                                      ". Update it to tau " + engine_version +
                                      "? Its engine version changes in the project file.";
    }
} // namespace

void update_editor_ui(tau::editor_context_t& ctx)
{
    if (!ctx.project_loaded)
    {
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        if (g_gate == gate_state_e::BUILDING)
        {
            if (tau::editor::project_manager::draw_waiting("building project",
                                                           "Building " + g_pending_project.filename().string() + " ...",
                                                           g_build_cancel.requested()))
            {
                g_build_cancel.cancel();
            }

            if (g_build_future.valid() && g_build_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                const bool built = g_build_future.get();
                g_gate = gate_state_e::PICKING;
                if (g_build_cancel.requested()) { tau::editor::project_manager::report_status("Build cancelled"); }
                else if (built) { start_cook(g_pending_project, ctx); }
                // the engine stamp check still refuses the previous library if it no longer fits
                else if (has_game_library(g_pending_project))
                {
                    TAU_LOG_WARN("EDITOR", "The build failed, opening with the previous game library");
                    start_cook(g_pending_project, ctx);
                }
                else
                {
                    tau::editor::project_manager::report_status("Build failed, see console output");
                }
            }
        }
        else if (g_gate == gate_state_e::UPDATING)
        {
            const auto answer = tau::editor::project_manager::draw_question("updating project", g_update_question,
                                                                            "Update and open", "Cancel");
            if (answer == tau::editor::project_manager::answer_e::YES)
            {
                if (tau::project_t::set_engine_version(g_pending_project, tau::engine::version()))
                {
                    start_build(g_pending_project, ctx);
                }
                else
                {
                    g_gate = gate_state_e::PICKING;
                    tau::editor::project_manager::report_status("The project file could not be updated");
                }
            }
            else if (answer == tau::editor::project_manager::answer_e::NO)
            {
                g_gate = gate_state_e::PICKING;
                tau::editor::project_manager::report_status("Opening cancelled");
            }
        }
        else if (g_gate == gate_state_e::COOKING)
        {
            if (tau::editor::project_manager::draw_waiting("cooking assets",
                                                           "Bringing " + g_pending_project.filename().string() +
                                                               "'s cooked assets up to date ...",
                                                           g_cook_cancel.requested()))
            {
                g_cook_cancel.cancel();
            }

            if (g_cook_future.valid() && g_cook_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                const bool cooked = g_cook_future.get();
                g_gate = gate_state_e::PICKING;

                // a cancelled cook leaves the project closed, half cooked assets would only show fallbacks
                if (g_cook_cancel.requested())
                {
                    TAU_LOG_WARN("EDITOR", "Cancelled cooking {}", g_pending_project.filename().string());
                    tau::editor::project_manager::report_status("Cooking cancelled");
                }
                else
                {
                    if (!cooked)
                    {
                        TAU_LOG_ERROR("EDITOR", "Could not bring the project's cooked assets up to date: {}",
                                      g_cook_error);
                    }

                    if (open_project(g_pending_project, ctx)) { ctx.project_loaded = true; }
                    else
                    {
                        tau::editor::project_manager::report_status("The project failed to open, see console output");
                    }
                }
            }
        }
        else
        {
            std::string chosen_project;
            if (tau::editor::project_manager::draw(ctx, chosen_project, nullptr))
            {
                request_open_project(chosen_project, ctx);
            }
        }

        ImGui::Render();
        tau::editor::imgui::submit(ImGui::GetDrawData());

        VkExtent2D extent = tau::renderer::ctx.render_extent;
        tau::renderer::submit_output_target("EditorViewport"_h, extent);

        auto& ecam = ctx.editor_camera;
        f32 aspect = extent.height ? (f32)extent.width / (f32)extent.height : 1.0f;
        tau::mat4_t view, proj, view_proj;
        tau::camera_system::build_view_projection(ecam.position, ecam.rotation.forward(), ecam.rotation.up(),
                                                  ecam.fov_deg, aspect, ecam.near_plane, ecam.far_plane, view, proj,
                                                  view_proj);
        tau::renderer::submit_color_view("PrimaryView"_h, view, proj, view_proj, ecam.position, "SceneColor"_h,
                                         "SceneDepth"_h, extent, ecam.near_plane, ecam.far_plane);
        return;
    }

    static std::chrono::steady_clock::time_point last_check_time = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_check_time).count() > 250)
    {
        last_check_time = now;

        std::error_code ec;
        auto cur_time = std::filesystem::last_write_time(trigger_path, ec);

        if (!ec && cur_time > last_reload_time && !ctx.reload_pending)
        {
            ctx.reload_pending = true;
            if (ctx.cur_mode != tau::editor_mode_e::EDIT && !ctx.replay_on_reload)
            {
                TAU_LOG_INFO("EDITOR", "Game library rebuilt, it reloads when Play stops");
            }
        }
    }

    if (ctx.reload_pending && game_lib)
    {
        if (ctx.cur_mode == tau::editor_mode_e::EDIT && g_prev_mode == tau::editor_mode_e::EDIT)
        {
            ctx.reload_pending = false;
            reload_game_library(ctx);

            if (g_replay_after_reload)
            {
                g_replay_after_reload = false;
                ctx.cur_mode = tau::editor_mode_e::PLAY;
            }
        }
        else if (ctx.replay_on_reload && !g_replay_after_reload)
        {
            g_replay_after_reload = true;
            ctx.cur_mode = tau::editor_mode_e::EDIT;
        }
    }

    // after a reload, which replaces the world
    tau::world_t& world = tau::engine::get_active_world();

    if (ctx.pending_scene_load)
    {
        ctx.pending_scene_load = false;
        std::ifstream file(ctx.pending_scene_path);
        if (file.is_open())
        {
            nlohmann::json scene_json = nlohmann::json::parse(file, nullptr, false);
            if (scene_json.is_discarded())
            {
                TAU_LOG_ERROR("EDITOR", "Failed to parse scene file '{}'", ctx.pending_scene_path);
            }
            else
            {
                tau::serialization::clear_scene(world);
                tau::serialization::deserialize_scene(world, scene_json);
                ctx.current_scene_path = ctx.pending_scene_path;
                tau::editor::asset_watch::set_open_scene(ctx.current_scene_path);
                TAU_LOG_INFO("EDITOR", "Scene loaded from {}", ctx.pending_scene_path);
            }
        }
        else
        {
            TAU_LOG_ERROR("EDITOR", "Failed to open scene file '{}'", ctx.pending_scene_path);
        }
        ctx.pending_scene_path.clear();
    }

    if (ctx.pending_scene_save)
    {
        ctx.pending_scene_save = false;
        nlohmann::json scene_json = tau::serialization::serialize_scene(world);
        // a scene saved somewhere new is a new asset, over an existing one it keeps its identity
        scene_json["guid"] = tau::asset_meta::guid_for_writing(ctx.pending_scene_path);
        std::ofstream file(ctx.pending_scene_path);
        file << scene_json.dump(4);
        file.close();

        ctx.current_scene_path = ctx.pending_scene_path;
        TAU_LOG_INFO("EDITOR", "Scene saved to {}", ctx.pending_scene_path);
        ctx.pending_scene_path.clear();

        tau::editor::asset_watch::ignore_own_write(ctx.current_scene_path);
        tau::editor::asset_watch::set_open_scene(ctx.current_scene_path);

        // nothing to reload: the world is the scene, the cooked .tauscene is only for the runtime
        std::string cook_err;
        if (!tau::editor::asset_cook::cook_asset(ctx.current_scene_path, cook_err))
        {
            TAU_LOG_ERROR("EDITOR", "Scene saved but could not be reimported: {}", cook_err);
        }
    }

    tau::editor::asset_watch::tick();

    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // bump to override layouts saved in imgui.ini
    ImGuiID dockspace_id = ImGui::GetID("MainDockSpace_v4");

    static bool first_time = true;
    if (first_time)
    {
        first_time = false;

        if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr)
        {
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

            f32 view_h = ImGui::GetMainViewport()->Size.y;

            ImGuiID dock_main = dockspace_id;

            ImGuiID dock_toolbar =
                ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Up, 32.0f / view_h, nullptr, &dock_main);
            constexpr f32 side_ratio = 0.22f;

            // before the bottom split so it spans full height
            ImGuiID dock_inspector =
                ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, side_ratio, nullptr, &dock_main);
            ImGuiID dock_bottom =
                ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, 0.28f, nullptr, &dock_main);
            // ratio of the width left after the inspector
            ImGuiID dock_hierarchy = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left,
                                                                 side_ratio / (1.0f - side_ratio), nullptr, &dock_main);

            ImGuiID dock_viewport = dock_main;

            ImGui::DockBuilderDockWindow("Toolbar", dock_toolbar);
            ImGui::DockBuilderDockWindow("Scene Viewport", dock_viewport);
            ImGui::DockBuilderDockWindow("Hierarchy", dock_hierarchy);
            ImGui::DockBuilderDockWindow("Inspector", dock_inspector);
            ImGui::DockBuilderDockWindow("Console", dock_bottom);

            ImGui::DockBuilderFinish(dockspace_id);
        }
    }

    ImGui::DockSpaceOverViewport(dockspace_id, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);

    // ImGui::ShowDemoWindow();

    tau::editor::panels::draw_main_menu_bar(world, ctx);
    tau::editor::panels::draw_viewport(world, ctx);
    tau::editor::panels::draw_hierarchy(world, ctx);
    tau::editor::panels::draw_inspector(world, ctx);
    tau::editor::panels::draw_toolbar(world, ctx);
    tau::editor::panels::draw_console(world, ctx);

    for (tau::panel_draw_func custom_panel : ctx.custom_panels) { custom_panel(world, ctx); }

    if (ctx.recompile_requested && !ctx.recompiling)
    {
        ctx.recompile_requested = false;
        ctx.recompiling = true;
        ctx.cancel_build_requested = false;
        g_build_cancel.reset();
        const std::filesystem::path pf = ctx.project_file;
        const std::string eng = tau::editor::project_manager::engine_dir();
        g_recompile_future = std::async(std::launch::async, [pf, eng] { return build_project(pf, eng); });
    }
    if (ctx.recompiling && g_recompile_future.valid() &&
        g_recompile_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        const bool ok = g_recompile_future.get();
        ctx.recompiling = false;
        if (g_build_cancel.requested()) { TAU_LOG_WARN("EDITOR", "Recompile cancelled"); }
        else if (!ok) { TAU_LOG_ERROR("EDITOR", "Recompile failed, see console output"); }
        ctx.cancel_build_requested = false;
    }
    if (ctx.recompiling && ctx.cancel_build_requested) { g_build_cancel.cancel(); }

    std::string chosen_project;
    bool commit_project = false;
    if (ctx.show_project_manager)
    {
        commit_project = tau::editor::project_manager::draw(ctx, chosen_project, &ctx.show_project_manager);
    }

    ImGui::Render();
    tau::editor::imgui::submit(ImGui::GetDrawData());

    tau::renderer::submit_output_target("EditorViewport"_h, {ctx.viewport_width, ctx.viewport_height});

    tau::renderer::set_view_debug_view("PrimaryView"_h, ctx.debug_view);
    tau::renderer::set_view_post_processing("PrimaryView"_h, ctx.post_processing);

    if (ctx.cur_mode != g_prev_mode)
    {
        if (g_prev_mode == tau::editor_mode_e::EDIT) { begin_play(ctx); }
        else if (ctx.cur_mode == tau::editor_mode_e::EDIT) { end_play(ctx); }

        g_prev_mode = ctx.cur_mode;
    }

    if (ctx.cur_mode == tau::editor_mode_e::EDIT)
    {
        tau::editor::camera_system::update(ctx.editor_camera, ctx.is_viewport_hovered);

        auto& ecam = ctx.editor_camera;
        f32 aspect = (f32)tau::renderer::ctx.logical_extent.width / (f32)tau::renderer::ctx.logical_extent.height;
        tau::mat4_t view, proj, view_proj;
        tau::camera_system::build_view_projection(ecam.position, ecam.rotation.forward(), ecam.rotation.up(),
                                                  ecam.fov_deg, aspect, ecam.near_plane, ecam.far_plane, view, proj,
                                                  view_proj);

        tau::renderer::submit_color_view("PrimaryView"_h, view, proj, view_proj, ecam.position, "SceneColor"_h,
                                         "SceneDepth"_h, tau::renderer::ctx.render_extent, ecam.near_plane,
                                         ecam.far_plane);
    }
    else
    {
        if (ctx.cur_mode == tau::editor_mode_e::PLAY && ctx.game_update)
        {
            ctx.game_update(&tau::engine::get_active_world());
        }
    }

    if (commit_project)
    {
        ctx.show_project_manager = false;
        request_open_project(chosen_project, ctx);
    }
}

bool open_project(const std::filesystem::path& project_file, tau::editor_context_t& ctx)
{
    if (!std::filesystem::exists(project_file))
    {
        TAU_LOG_ERROR("EDITOR", "Project file not found: {}", project_file.string());
        return false;
    }

    tau::project_t project;
    if (!tau::project_t::load(project_file, project))
    {
        TAU_LOG_ERROR("EDITOR", "Failed to load project file: {}", project_file.string());
        return false;
    }

    source_lib_path = project.lib_path;
    trigger_path = project.trigger_path;
    source_lib_name = project.lib_path.string();
    ctx.project_file = project_file.string();
    // saving from the inspector reimports through the cooker, which needs to know the project
    tau::editor::asset_cook::set_project(ctx.project_file);

    // shaders and game code are edited outside the editor, watching is how it hears about them
    tau::editor::asset_watch::start(project.assets_dir, tau::editor::asset_cook::engine_assets_dir());

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(source_lib_path.parent_path(), ec))
    {
        if (entry.path().string().find("-loaded-") != std::string::npos) { std::filesystem::remove(entry.path(), ec); }
    }

    const std::filesystem::path cooked = project.cooked_assets_dir;

    // assets were brought up to date before this, see start_cook

    // engine:// stays on the editor's own cooked tree where vfs::init put it
    // the project's engine asset copy only refreshes on a game build, the editor's own tree always matches
    tau::vfs::mount("game://assets/", (cooked / "game").generic_string() + "/");
    tau::vfs::mount("src://", project.assets_dir.generic_string() + "/");

    tau::asset_meta::load_guid_map((cooked / "guid_map.json").generic_string());

    if (g_prev_mode != tau::editor_mode_e::EDIT)
    {
        if (ctx.game_shutdown) { ctx.game_shutdown(&tau::engine::get_active_world()); }
        tau::engine::get_active_world().end_play();
        ctx.cur_mode = g_prev_mode = tau::editor_mode_e::EDIT;
    }

    // the previous project's world goes while its library is loaded
    tau::engine::create_scene();
    if (game_lib)
    {
        tau::reflection::shutdown();
        release_game_library(ctx);
    }
    ctx.reload_pending = false;
    g_replay_after_reload = false;

    ctx.project_dir = project.project_dir.string();

    game_library_t lib;
    if (!open_game_library(lib))
    {
        tau::engine::get_active_world().init();
        TAU_LOG_ERROR("EDITOR", "Could not load the game lib (check the build output for errors)");
        return false;
    }

    start_game_library(ctx, lib, nullptr);
    TAU_LOG_INFO("EDITOR", "Successfully loaded game lib");

    if (!project.startup_scene.empty())
    {
        std::string vfs = project.startup_scene;
        if (vfs.find("://") == std::string::npos)
        {
            std::string_view guid_path = tau::asset_meta::get_path_for_guid(project.startup_scene);
            vfs = !guid_path.empty() ? std::string(guid_path) : ("game://assets/" + project.startup_scene);
        }

        std::string rel = vfs;
        const std::string prefix = "game://assets/";
        if (rel.rfind(prefix, 0) == 0) { rel = rel.substr(prefix.size()); }

        // map the cooked artifact back to the source a human edits, extensions come from asset_table
        if (const tau::asset_type_t* scene_type = tau::asset_table::by_key("scene"))
        {
            const std::string cooked_ext(scene_type->cooked_extension);
            const std::string source_ext(scene_type->source_extensions.front());

            if (rel.size() > cooked_ext.size() &&
                rel.compare(rel.size() - cooked_ext.size(), cooked_ext.size(), cooked_ext) == 0)
            {
                rel.replace(rel.size() - cooked_ext.size(), cooked_ext.size(), source_ext);
            }
        }
        std::filesystem::path scene_src = project.assets_dir / rel;

        std::ifstream file(scene_src);
        if (file.is_open())
        {
            nlohmann::json scene_json = nlohmann::json::parse(file, nullptr, false);
            if (!scene_json.is_discarded())
            {
                tau::serialization::deserialize_scene(tau::engine::get_active_world(), scene_json);
                ctx.current_scene_path = scene_src.string();
                tau::editor::asset_watch::set_open_scene(ctx.current_scene_path);
                TAU_LOG_INFO("EDITOR", "Opened startup scene: {}", scene_src.string());
            }
            else
            {
                TAU_LOG_ERROR("EDITOR", "Startup scene is not valid: {}", scene_src.string());
            }
        }
        else
        {
            TAU_LOG_WARN("EDITOR", "Startup scene not found: {}", scene_src.string());
        }
    }

    tau::editor::project_manager::add_recent(project_file.string());
    TAU_LOG_INFO("EDITOR", "Opened project: {}", project_file.string());
    return true;
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

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    tau::engine::create_scene();
    tau::engine::get_active_world().init();

    static tau::editor_context_t editor_ctx;

    if (have_startup_project) { request_open_project(startup_project, editor_ctx); }

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

    // a running build or cook would hold up the exit, so stop it
    g_build_cancel.cancel();
    g_cook_cancel.cancel();

    if (g_prev_mode != tau::editor_mode_e::EDIT && editor_ctx.game_shutdown)
    {
        editor_ctx.game_shutdown(&tau::engine::get_active_world());
    }

    vkDeviceWaitIdle(tau::renderer::ctx.device);

    tau::editor::imgui::shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    tau::engine::shutdown();

    // the current library and any that could not be released are left to the process exit

    return 0;
}