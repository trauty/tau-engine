#include "project_open.h"

#include "tau-editor/asset_cook.h"
#include "tau-editor/asset_files.h"
#include "tau-editor/asset_watch.h"
#include "tau-editor/commands.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/events.h"
#include "tau-editor/game_code.h"
#include "tau-editor/project_manager.h"
#include "tau-editor/scene_io.h"
#include "tau/asset.h"
#include "tau/assets/scene.h"
#include "tau/engine.h"
#include "tau/log.h"
#include "tau/os.h"
#include "tau/project.h"
#include "tau/vfs.h"

#include <chrono>
#include <future>
#include <string>

namespace tau::editor::project_open
{
    namespace
    {
        namespace fs = std::filesystem;

        // build the library, cook the assets, then open, the first two off the UI thread
        enum class gate_state_e
        {
            PICKING,
            UPDATING,
            BUILDING,
            COOKING,
        };

        gate_state_e g_gate = gate_state_e::PICKING;
        fs::path g_pending;
        std::string g_update_question;

        std::future<bool> g_build;
        std::future<bool> g_cook;
        std::string g_cook_error;
        os::process_cancel_t g_cook_cancel;

        bool ready(const std::future<bool>& future)
        { return future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }

        bool has_game_library(const fs::path& project_file)
        {
            project_t project;
            return project_t::load(project_file, project) && fs::exists(project.lib_path);
        }

        // before anything loads: cooked data may predate this engine (a format bump breaks materials until recooked)
        // a failure is reported and opening carries on, what still fails to load draws as the fallback
        void start_cook(const fs::path& project_file, editor_context_t& ctx)
        {
            g_pending = project_file;
            g_gate = gate_state_e::COOKING;
            ctx.project_loaded = false;

            asset_watch::flush();
            asset_cook::set_project(project_file.string());
            g_cook_cancel.reset();
            g_cook =
                std::async(std::launch::async, [] { return asset_cook::cook_project(g_cook_error, &g_cook_cancel); });
        }

        // always builds: an up to date project costs a moment, a library left from an older engine build crashes
        void start_build(const fs::path& project_file, editor_context_t& ctx)
        {
            g_pending = project_file;
            g_gate = gate_state_e::BUILDING;
            ctx.project_loaded = false;
            project_manager::report_status("Building " + project_file.filename().string() + "...");
            g_build = game_code::start_build(project_file);
        }

        // a startup scene named by its cooked file opens its source
        fs::path startup_scene_source(const project_t& project)
        {
            fs::path rel = asset_files::rel_of(project::scene_path(project.startup_scene));
            const asset_type_t* scene_type = assets::type_by_id(get_type_id<scene_t>());
            if (scene_type && rel.extension() == scene_type->cooked_extension)
            {
                rel.replace_extension(std::string(scene_type->source_extensions.front()));
            }
            return rel.empty() ? fs::path{} : project.assets_dir / rel;
        }

        bool open(const fs::path& project_file, editor_context_t& ctx)
        {
            if (!fs::exists(project_file))
            {
                TAU_LOG_ERROR("EDITOR", "Project file not found: {}", project_file.string());
                return false;
            }

            project_t project;
            if (!project_t::load(project_file, project))
            {
                TAU_LOG_ERROR("EDITOR", "Failed to load project file: {}", project_file.string());
                return false;
            }

            ctx.project_file = project_file.string();
            // saving from the inspector reimports through the cooker, which needs to know the project
            asset_cook::set_project(ctx.project_file);

            // shaders and game code are edited outside the editor, watching is how it hears about them
            asset_watch::start(project.assets_dir, asset_cook::engine_assets_dir());

            // assets were brought up to date before this, see start_cook

            // engine:// stays on the editor's own cooked tree where vfs::init put it
            // the project's engine asset copy only refreshes on a game build, the editor's own tree always matches
            project::mount(project, false);
            vfs::mount("src://", project.assets_dir.generic_string() + "/");

            // the previous project's world goes while its library is loaded
            game_code::unload(ctx);

            ctx.selection.clear();
            ctx.inspected_asset.clear();
            ctx.project_dir = project.project_dir.string();
            ctx.project_assets_dir = project.assets_dir.string();
            ctx.project_cooked_dir = project.cooked_assets_dir.string();
            scene_io::set_path(ctx, {});
            events::emit(events::event_e::PROJECT_OPENED, engine::get_active_world(), ctx);
            assets::collect();

            if (!game_code::load(project, ctx)) { return false; }

            if (!project.startup_scene.empty())
            {
                const fs::path scene = startup_scene_source(project);
                if (!scene.empty() && fs::is_regular_file(scene))
                {
                    scene_io::load(engine::get_active_world(), ctx, scene.string());
                }
                else
                {
                    TAU_LOG_WARN("EDITOR", "Startup scene not found: {}", scene.string());
                }
            }

            project_manager::add_recent(project_file.string());
            TAU_LOG_INFO("EDITOR", "Opened project: {}", project_file.string());
            return true;
        }

        void draw_building(editor_context_t& ctx)
        {
            if (project_manager::draw_waiting("building project", "Building " + g_pending.filename().string() + " ...",
                                              game_code::build_cancelled()))
            {
                game_code::cancel_build();
            }

            if (!ready(g_build)) { return; }

            const bool built = g_build.get();
            g_gate = gate_state_e::PICKING;
            if (game_code::build_cancelled()) { project_manager::report_status("Build cancelled"); }
            else if (built) { start_cook(g_pending, ctx); }
            // the engine stamp check still refuses the previous library if it no longer fits
            else if (has_game_library(g_pending))
            {
                TAU_LOG_WARN("EDITOR", "The build failed, opening with the previous game library");
                start_cook(g_pending, ctx);
            }
            else
            {
                project_manager::report_status("Build failed, see console output");
            }
        }

        void draw_updating(editor_context_t& ctx)
        {
            const auto answer =
                project_manager::draw_question("updating project", g_update_question, "Update and open", "Cancel");
            if (answer == project_manager::answer_e::YES)
            {
                if (project_t::set_engine_version(g_pending, engine::version())) { start_build(g_pending, ctx); }
                else
                {
                    g_gate = gate_state_e::PICKING;
                    project_manager::report_status("The project file could not be updated");
                }
            }
            else if (answer == project_manager::answer_e::NO)
            {
                g_gate = gate_state_e::PICKING;
                project_manager::report_status("Opening cancelled");
            }
        }

        void draw_cooking(editor_context_t& ctx)
        {
            if (project_manager::draw_waiting(
                    "cooking assets", "Bringing " + g_pending.filename().string() + "'s cooked assets up to date ...",
                    g_cook_cancel.requested()))
            {
                g_cook_cancel.cancel();
            }

            if (!ready(g_cook)) { return; }

            const bool cooked = g_cook.get();
            g_gate = gate_state_e::PICKING;

            // a cancelled cook leaves the project closed, half cooked assets would only show fallbacks
            if (g_cook_cancel.requested())
            {
                TAU_LOG_WARN("EDITOR", "Cancelled cooking {}", g_pending.filename().string());
                project_manager::report_status("Cooking cancelled");
                return;
            }

            if (!cooked)
            {
                TAU_LOG_ERROR("EDITOR", "Could not bring the project's cooked assets up to date: {}", g_cook_error);
            }

            if (open(g_pending, ctx)) { ctx.project_loaded = true; }
            else
            {
                project_manager::report_status("The project failed to open, see console output");
            }
        }
    } // namespace

    void init(editor_context_t&)
    {
        commands::set(commands::command_e::OPEN_PROJECT,
                      {.label = "Open Project",
                       .discards = commands::discards_e::EVERYTHING,
                       .run = [](world_t&, editor_context_t& ctx, const commands::request_t& request)
                       { project_open::request(request.path, ctx); }});
    }

    // a project opens only in an editor of the engine version it is made for
    void request(const std::filesystem::path& project_file, editor_context_t& ctx)
    {
        project_t project;
        if (!project_t::load(project_file, project))
        {
            project_manager::report_status("The project file could not be read, see console output");
            return;
        }

        const std::string engine_version = major_minor(engine::version());
        const int order =
            project.engine_version.empty() ? -1 : compare_major_minor(project.engine_version, engine_version);
        if (order == 0)
        {
            start_build(project_file, ctx);
            return;
        }

        if (order > 0)
        {
            TAU_LOG_ERROR("EDITOR", "{} is made for tau {}, this editor is tau {}", project_file.filename().string(),
                          project.engine_version, engine_version);
            project_manager::report_status("The project needs a tau " + project.engine_version +
                                           " editor, this one is " + engine_version);
            return;
        }

        g_pending = project_file;
        g_gate = gate_state_e::UPDATING;
        ctx.project_loaded = false;
        g_update_question = project.engine_version.empty()
                                ? project_file.filename().string() + " names no engine version. Make it a tau " +
                                      engine_version + " project?"
                                : project_file.filename().string() + " is made for tau " + project.engine_version +
                                      ". Update it to tau " + engine_version +
                                      "? Its engine version changes in the project file.";
    }

    void draw_gate(editor_context_t& ctx)
    {
        switch (g_gate)
        {
        case gate_state_e::BUILDING: draw_building(ctx); break;
        case gate_state_e::UPDATING: draw_updating(ctx); break;
        case gate_state_e::COOKING: draw_cooking(ctx); break;
        case gate_state_e::PICKING:
        {
            std::string chosen;
            if (project_manager::draw(ctx, chosen, nullptr)) { request(chosen, ctx); }
            break;
        }
        }
    }

    void draw_manager(editor_context_t& ctx)
    {
        if (!ctx.show_project_manager) { return; }

        std::string chosen;
        if (project_manager::draw(ctx, chosen, &ctx.show_project_manager))
        {
            ctx.show_project_manager = false;
            commands::push(commands::command_e::OPEN_PROJECT, chosen);
        }
    }

    void cancel()
    {
        game_code::cancel_build();
        g_cook_cancel.cancel();
    }
} // namespace tau::editor::project_open
