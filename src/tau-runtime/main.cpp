#include "tau/asset.h"
#include "tau/assets/scene.h"
#include "tau/engine.h"
#include "tau/game.h"
#include "tau/log.h"
#include "tau/project.h"
#include "tau/reflection.h"
#include "tau/serialization.h"
#include "tau/world.h"

#include <SDL3/SDL_main.h>

#ifndef TAU_STATIC_LINK
    #include "tau/game_library.h"
#endif

int main(int argc, char* argv[])
{
    tau::project_t project;
    bool have_project = argc >= 2 && tau::project_t::load(argv[1], project);

#ifndef TAU_STATIC_LINK
    if (argc >= 2 && !have_project)
    {
        TAU_LOG_FATAL("ENGINE", "Failed to load project file: {}", argv[1]);
        return -1;
    }
#endif

    // a standalone game has no project file, its name is baked in instead: it names the window and user:// folder
#ifdef TAU_GAME_NAME
    const char* fallback_name = TAU_GAME_NAME;
#else
    const char* fallback_name = "tau";
#endif
    const char* window_name = have_project ? project.project_name.c_str() : fallback_name;
    if (!tau::engine::init(window_name, 1280, 720)) { return -1; }

    // engine::init() already mounted the cooked tree beside the binary and loaded its guid map
    // a project keeps its own engine asset copy, its root supersedes that one, guid maps merge
    if (have_project) { tau::project::mount(project, true); }

    tau::world_t& world = tau::engine::get_active_world();

#ifndef TAU_STATIC_LINK
    tau::game_library_t game_lib;
    if (have_project)
    {
        if (!tau::game_library::open(project.lib_path.string(), game_lib)) { return -1; }
    }
    else
    {
        TAU_LOG_WARN("ENGINE",
                     "No project file given; running an empty world. Usage: tau-runtime <path-to-.tauproject>");
    }
#endif

    // game types first, else its components are dropped
    tau::reflection::run_registrations(*world.reflection_ctx);

#ifdef TAU_STARTUP_SCENE
    const std::string baked_startup_scene = TAU_STARTUP_SCENE;
#else
    const std::string baked_startup_scene;
#endif
    const std::string scene_path = tau::project::scene_path(
        (have_project && !project.startup_scene.empty()) ? project.startup_scene : baked_startup_scene);

    if (!scene_path.empty())
    {
        if (const tau::scene_t* scene = tau::assets::get(tau::assets::load<tau::scene_t>(scene_path)))
        {
            tau::serialization::instantiate_scene(world, *scene);
            TAU_LOG_INFO("ENGINE", "Loaded startup scene: {}", scene_path);
        }
        else
        {
            TAU_LOG_ERROR("ENGINE", "Failed to load startup scene: {}", scene_path);
        }

        // the entities hold what they use
        tau::assets::collect();
    }

    tau::game::run_hooks(tau::game::hook_e::LOAD, world);
    world.begin_play();

    tau::engine::run();

    // world and types run its code, so they go first
#ifndef TAU_STATIC_LINK
    if (game_lib.handle)
    {
        tau::engine::reset_world();
        tau::reflection::shutdown();
        tau::game_library::close(game_lib);
    }
#endif

    tau::engine::shutdown();
    return 0;
}
