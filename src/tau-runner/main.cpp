#include "runner.h"
#include "tau/asset.h"
#include "tau/assets/scene.h"
#include "tau/engine.h"
#include "tau/game.h"
#include "tau/game_library.h"
#include "tau/log.h"
#include "tau/project.h"
#include "tau/reflection.h"
#include "tau/rendering/pipelines.h"
#include "tau/serialization.h"
#include "tau/world.h"

#include <SDL3/SDL_main.h>

// tau-runtime's sequence for a project with a game library, plus the runner's hooks
int main(int argc, char* argv[])
{
    tau::project_t project;
    tau::runner::options_t options;
    if (argc < 2 || !tau::project_t::load(argv[1], project) || !tau::runner::parse(argc, argv, 2, options))
    {
        TAU_LOG_FATAL("RUNNER",
                      "Usage: tau-runner <project> [--scene=PATH] [--camera=NAME] [--size=WxH] [--frames=N] "
                      "[--dump-hdr=PREFIX] [--timings] [--no-vsync] [--quit-after=SECONDS] [--headless] [--compat] "
                      "[--validation] [--renderdoc] [--capture-frame=N]");
        return -1;
    }

    const tau::engine::engine_config_t engine_config = {
        .name = project.project_name,
        .vsync = options.vsync,
        .validation = options.validation || tau::engine::DEBUG_BUILD,
        .force_compat = options.compat,
        .headless = options.headless,
        .renderdoc = options.renderdoc,
    };
    if (!tau::engine::init(engine_config)) { return -1; }
    tau::project::mount(project, true);
    tau::renderer::pipelines::freeze();

    tau::world_t& world = tau::engine::get_active_world();

    tau::game_library_t game_lib;
    if (!tau::game_library::open(project.lib_path.string(), game_lib))
    {
        tau::engine::shutdown();
        return -1;
    }

    // game types first, else its components are dropped
    tau::reflection::run_registrations(*world.reflection_ctx);

    const std::string scene_path =
        tau::project::scene_path(options.scene.empty() ? project.startup_scene : options.scene);
    const tau::scene_t* scene = tau::assets::get(tau::assets::load<tau::scene_t>(scene_path));
    if (scene) { tau::serialization::instantiate_scene(world, *scene); }
    tau::assets::collect();

    const bool ready = scene && (options.camera.empty() || tau::runner::select_camera(world, options.camera));
    if (!scene) { TAU_LOG_ERROR("RUNNER", "Failed to load scene: {}", scene_path); }

    if (ready)
    {
        tau::game::run_hooks(tau::game::hook_e::LOAD, world);
        world.begin_play();

        tau::runner::start(options);
        tau::engine::run();
    }

    // world and types run its code, so they go first
    tau::engine::reset_world();
    tau::reflection::shutdown();
    tau::game_library::close(game_lib);

    tau::engine::shutdown();
    return ready && tau::runner::succeeded() ? 0 : 1;
}
