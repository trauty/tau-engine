#include "tau/asset_meta.h"
#include "tau/asset_registry.h"
#include "tau/assets/scene.h"
#include "tau/ecs_fwd.h"
#include "tau/engine.h"
#include "tau/game.h"
#include "tau/log.h"
#include "tau/project.h"
#include "tau/serialization.h"
#include "tau/vfs.h"
#include "tau/world.h"

#include <SDL3/SDL_main.h>
#include <filesystem>

#ifdef TAU_STATIC_LINK
extern "C" void tau_game_register_types(tau::world_t* world);
extern "C" void tau_game_init(tau::world_t* world);
extern "C" void tau_game_update(tau::world_t* world);
extern "C" void tau_game_shutdown(tau::world_t* world);
#else
    #include "tau/os.h"
    #if TAU_PLATFORM_WIN
        #include <windows.h>
    #elif TAU_PLATFORM_LINUX
        #include <dlfcn.h>
    #endif

game_register_types_func game_register_types = nullptr;
game_init_func game_init = nullptr;
game_update_func game_update = nullptr;
game_shutdown_func game_shutdown = nullptr;
tau::os::lib_handle_t game_lib = nullptr;
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
    if (have_project)
    {
        namespace fs = std::filesystem;
        const fs::path cooked = project.cooked_assets_dir;
        tau::vfs::mount("engine://assets/", (cooked / "engine").generic_string() + "/");
        tau::vfs::mount("game://assets/", (cooked / "game").generic_string() + "/");
        tau::asset_meta::load_guid_map((cooked / "guid_map.json").generic_string());
    }

    tau::engine::create_scene();
    tau::world_t& cur_world = tau::engine::get_active_world();
    cur_world.init();

    // game types before the scene loads, else its components are unknown and dropped
#ifdef TAU_STATIC_LINK
    tau_game_register_types(&cur_world);
    tau::game::run_on_load(cur_world);
#else
    if (have_project)
    {
        std::string lib_name = project.lib_path.string();

        game_lib = tau::os::load_lib(lib_name.c_str());

        if (!game_lib)
        {
    #if TAU_PLATFORM_WIN
            TAU_LOG_FATAL("ENGINE", "Failed to load library with name: {}; Error: {}", lib_name, GetLastError());
    #elif TAU_PLATFORM_LINUX
            TAU_LOG_FATAL("ENGINE", "Failed to load library with name: {}; Error: {}", lib_name, dlerror());
    #endif
            return -1;
        }

        game_register_types =
            (game_register_types_func)tau::os::get_proc_address(game_lib, "tau_game_register_types_internal");
        game_init = (game_init_func)tau::os::get_proc_address(game_lib, "tau_game_init_internal");
        game_update = (game_update_func)tau::os::get_proc_address(game_lib, "tau_game_update_internal");
        game_shutdown = (game_shutdown_func)tau::os::get_proc_address(game_lib, "tau_game_shutdown_internal");

        if (!game_register_types || !game_init || !game_update || !game_shutdown)
        {
            TAU_LOG_FATAL("ENGINE", "Could not find one or more game logic functions inside game lib");
            return -1;
        }

        const char* lib_stamp = tau::game::library_stamp(game_lib);
        if (!lib_stamp || std::string_view(lib_stamp) != tau::engine::build_stamp())
        {
            TAU_LOG_FATAL("ENGINE",
                          "The game library was built against a different engine build (library: {}, runtime: {}). "
                          "Build the engine and the project again.",
                          lib_stamp ? lib_stamp : "none", tau::engine::build_stamp());
            return -1;
        }

        game_register_types(&cur_world);
        tau::game::run_on_load(cur_world);
    }
    else
    {
        TAU_LOG_WARN("ENGINE",
                     "No project file given; running an empty world. Usage: tau-runtime <path-to-.tauproject>");
    }
#endif

#ifdef TAU_STARTUP_SCENE
    const std::string baked_startup_scene = TAU_STARTUP_SCENE;
#else
    const std::string baked_startup_scene;
#endif
    std::string startup_scene_ref =
        (have_project && !project.startup_scene.empty()) ? project.startup_scene : baked_startup_scene;

    if (!startup_scene_ref.empty())
    {
        std::string scene_path = startup_scene_ref;
        if (scene_path.find("://") == std::string::npos)
        {
            std::string_view guid_path = tau::asset_meta::get_path_for_guid(startup_scene_ref);
            scene_path = !guid_path.empty() ? std::string(guid_path) : ("game://assets/" + startup_scene_ref);
        }

        tau::asset_registry_t& registry = tau::engine::get_asset_registry();
        tau::asset_handle_t scene_handle = registry.load_sync<tau::scene_t>(scene_path);

        if (tau::scene_t* scene = registry.get<tau::scene_t>(scene_handle))
        {
            tau::serialization::instantiate_scene(cur_world, *scene);
            TAU_LOG_INFO("ENGINE", "Loaded startup scene: {}", scene_path);
        }
        else
        {
            TAU_LOG_ERROR("ENGINE", "Failed to load startup scene: {}", scene_path);
        }

        registry.release<tau::scene_t>(scene_handle);
    }

    tau::engine::get_active_world().register_update_system(
        [](tau::world_t& world)
        {
#ifdef TAU_STATIC_LINK
            tau_game_update(&world);
#else
            if (game_update) { game_update(&world); }
#endif
        });

    // with the scene loaded, as when the editor starts Play
    cur_world.begin_play();
#ifdef TAU_STATIC_LINK
    tau_game_init(&cur_world);
#else
    if (game_init) { game_init(&cur_world); }
#endif

    tau::engine::run();

#ifdef TAU_STATIC_LINK
    tau_game_shutdown(&tau::engine::get_active_world());
#else
    if (game_shutdown) { game_shutdown(&tau::engine::get_active_world()); }
#endif
    tau::engine::get_active_world().end_play();

    tau::engine::shutdown();

#ifndef TAU_STATIC_LINK
    if (game_lib) { tau::os::free_lib(game_lib); }
#endif

    return 0;
}
