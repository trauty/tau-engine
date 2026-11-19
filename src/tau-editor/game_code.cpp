#include "game_code.h"

#include "imgui/imgui.h"
#include "tau-editor/commands.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/events.h"
#include "tau-editor/project_manager.h"
#include "tau-editor/scene_io.h"
#include "tau/engine.h"
#include "tau/game.h"
#include "tau/game_library.h"
#include "tau/log.h"
#include "tau/os.h"
#include "tau/project.h"
#include "tau/reflection.h"
#include "tau/serialization.h"
#include "tau/world.h"

#include "json/json.hpp"
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace tau::editor::game_code
{
    namespace
    {
        namespace fs = std::filesystem;

        using editor_init_fn_t = void (*)(world_t*, editor_context_t*, ImGuiContext*);

        struct loaded_t
        {
            game_library_t lib;
            fs::path copy;
            editor_init_fn_t editor_init = nullptr;
        };

        fs::path g_source_path;
        fs::path g_trigger_path;
        fs::file_time_type g_loaded_trigger_time;

        game_library_t g_lib;
        fs::path g_lib_copy;
        std::vector<game_library_t> g_retired;

        os::process_cancel_t g_build_cancel;
        std::future<bool> g_recompile;

        // a rebuild stopped Play to reload, so Play starts again after it
        bool g_replay_after_reload = false;

        // runs xmake from the project's own directory
        bool run_xmake(const std::string& project_dir, const std::vector<std::string>& args, const char* what)
        {
            auto forward = [](std::string_view raw)
            {
                const std::string line = log::strip_ansi(raw);
                if (line.empty()) { return; }

                auto has = [&line](const char* marker) { return line.find(marker) != std::string::npos; };
                if (has("error:") || has(": error")) { TAU_LOG_ERROR("BUILD", "{}", line); }
                else if (has("warning:") || has(": warning")) { TAU_LOG_WARN("BUILD", "{}", line); }
                else
                {
                    TAU_LOG_INFO("BUILD", "{}", line);
                }
            };

            const os::process_result_t run = os::run_process("xmake", args, project_dir, forward, &g_build_cancel);
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

        bool build_project(const fs::path& project_file, const std::string& engine_dir)
        {
            project_t project;
            if (!project_t::load(project_file, project))
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
                              "{} finds the engine the old way. Replace its engine lookup with the tau_engine_dir "
                              "option from the engine's template/xmake.lua.",
                              (project.project_dir / "xmake.lua").string());
                return false;
            }

            // records this editor's engine in the project, so terminal and IDE builds use it too
            TAU_LOG_INFO("EDITOR", "Configuring project in {}", proj);
            if (!run_xmake(proj, {"f", "-P", ".", "-y", "-m", mode, "--tau_engine_dir=" + engine_dir}, "configure"))
            {
                return false;
            }

            TAU_LOG_INFO("EDITOR", "Building {}", target);
            if (!run_xmake(proj, {"build", "-P", ".", target}, "build")) { return false; }

            TAU_LOG_INFO("EDITOR", "Project built successfully");
            return true;
        }

        // loads a copy, so the next build can overwrite the original while this one runs
        bool open(loaded_t& out)
        {
            std::error_code trigger_ec;
            g_loaded_trigger_time = fs::last_write_time(g_trigger_path, trigger_ec);

            static int reload_counter = 0;
            out.copy =
                g_source_path.parent_path() / (g_source_path.stem().string() + "-loaded-" +
                                               std::to_string(reload_counter++) + g_source_path.extension().string());

            std::error_code ec;
            fs::copy_file(g_source_path, out.copy, fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                TAU_LOG_WARN("EDITOR", "Could not copy game logic lib '{}': {}", g_source_path.string(), ec.message());
                return false;
            }

            if (!game_library::open(out.copy.string(), out.lib))
            {
                fs::remove(out.copy, ec);
                return false;
            }

            out.editor_init = (editor_init_fn_t)os::get_proc_address(out.lib.handle, "tau_editor_init");
            return true;
        }

        // the world it ran in was destroyed and reflection reset before this
        void release(editor_context_t& ctx)
        {
            std::erase_if(ctx.custom_panels, [](panel_draw_func panel)
                          { return os::module_base_of(reinterpret_cast<const void*>(panel)) == g_lib.module; });
            events::remove_from_module(g_lib.module);

            if (!game_library::close(g_lib))
            {
                g_retired.push_back(g_lib);
                g_lib = {};
                return;
            }

            std::error_code ec;
            fs::remove(g_lib_copy, ec);
            g_lib = {};
        }

        // the active world must be new: the library's types, then the scene, then its on load code
        void start(editor_context_t& ctx, const loaded_t& lib, const nlohmann::json* scene)
        {
            g_lib = lib.lib;
            g_lib_copy = lib.copy;

            world_t& world = engine::get_active_world();
            reflection::register_types();
            reflection::run_registrations(*world.reflection_ctx);

            if (scene) { serialization::deserialize_scene(world, *scene); }

            game::run_hooks(game::hook_e::LOAD, world);
            scene_io::report_unknown_keys(world, ctx);

            if (lib.editor_init)
            {
                ctx.custom_panels.clear();
                lib.editor_init(&world, &ctx, ImGui::GetCurrentContext());
            }
        }

        // in Edit only: a reload keeps what saving the scene keeps
        void reload(editor_context_t& ctx)
        {
            TAU_LOG_INFO("EDITOR", "Build system signaled completion. Attempting hot reload...");

            loaded_t lib;
            if (!open(lib))
            {
                TAU_LOG_ERROR("EDITOR", "Kept the previous game code, the rebuilt library could not be loaded");
                return;
            }

            const nlohmann::json scene = serialization::serialize_scene(engine::get_active_world());

            engine::reset_world();
            reflection::shutdown();
            release(ctx);

            start(ctx, lib, &scene);

            TAU_LOG_INFO("EDITOR", "Successfully hot-reloaded game lib");
        }

        bool ready(const std::future<bool>& future)
        { return future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
    } // namespace

    void init(editor_context_t&)
    {
        using commands::command_e;
        using commands::request_t;

        commands::set(command_e::RECOMPILE,
                      {.label = "Recompile",
                       .enabled = [](const editor_context_t& ctx) { return ctx.project_loaded && !ctx.recompiling; },
                       .run =
                           [](world_t&, editor_context_t& ctx, const request_t&)
                       {
                           ctx.recompiling = true;
                           ctx.build_cancelling = false;
                           g_recompile = start_build(ctx.project_file);
                       }});
        commands::set(command_e::CANCEL_BUILD,
                      {.label = "Cancel",
                       .enabled = [](const editor_context_t& ctx) { return ctx.recompiling && !ctx.build_cancelling; },
                       .run =
                           [](world_t&, editor_context_t& ctx, const request_t&)
                       {
                           ctx.build_cancelling = true;
                           cancel_build();
                       }});
    }

    std::future<bool> start_build(const std::filesystem::path& project_file)
    {
        g_build_cancel.reset();
        const std::string engine_dir = project_manager::engine_dir();
        return std::async(std::launch::async,
                          [project_file, engine_dir] { return build_project(project_file, engine_dir); });
    }

    void cancel_build() { g_build_cancel.cancel(); }

    bool build_cancelled() { return g_build_cancel.requested(); }

    void unload(editor_context_t& ctx)
    {
        engine::reset_world();
        if (g_lib.handle)
        {
            reflection::shutdown();
            release(ctx);
        }
        ctx.reload_pending = false;
        g_replay_after_reload = false;
    }

    bool load(const project_t& project, editor_context_t& ctx)
    {
        g_source_path = project.lib_path;
        g_trigger_path = project.trigger_path;

        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(g_source_path.parent_path(), ec))
        {
            if (entry.path().string().find("-loaded-") != std::string::npos) { fs::remove(entry.path(), ec); }
        }

        loaded_t lib;
        if (!open(lib))
        {
            TAU_LOG_ERROR("EDITOR", "Could not load the game lib (check the build output for errors)");
            return false;
        }

        start(ctx, lib, nullptr);
        TAU_LOG_INFO("EDITOR", "Successfully loaded game lib");
        return true;
    }

    void update(editor_context_t& ctx)
    {
        static std::chrono::steady_clock::time_point last_check = std::chrono::steady_clock::now();
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

        if (now - last_check > std::chrono::milliseconds(250))
        {
            last_check = now;

            std::error_code ec;
            const fs::file_time_type trigger_time = fs::last_write_time(g_trigger_path, ec);

            if (!ec && trigger_time > g_loaded_trigger_time && !ctx.reload_pending)
            {
                ctx.reload_pending = true;
                if (ctx.cur_mode != editor_mode_e::EDIT && !ctx.replay_on_reload)
                {
                    TAU_LOG_INFO("EDITOR", "Game library rebuilt, it reloads when Play stops");
                }
            }
        }

        if (ctx.reload_pending && g_lib.handle)
        {
            if (ctx.cur_mode == editor_mode_e::EDIT)
            {
                ctx.reload_pending = false;
                reload(ctx);

                if (g_replay_after_reload)
                {
                    g_replay_after_reload = false;
                    commands::push(commands::command_e::PLAY);
                }
            }
            else if (ctx.replay_on_reload && !g_replay_after_reload)
            {
                g_replay_after_reload = true;
                commands::push(commands::command_e::STOP);
            }
        }

        if (ctx.recompiling && ready(g_recompile))
        {
            const bool ok = g_recompile.get();
            ctx.recompiling = false;
            if (build_cancelled()) { TAU_LOG_WARN("EDITOR", "Recompile cancelled"); }
            else if (!ok) { TAU_LOG_ERROR("EDITOR", "Recompile failed, see console output"); }
            ctx.build_cancelling = false;
        }
    }
} // namespace tau::editor::game_code
