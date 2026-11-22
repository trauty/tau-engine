#include "engine.h"

#include "tau/asset.h"
#include "tau/asset_types.h"
#include "tau/defines.h"
#include "tau/input.h"
#include "tau/jobs.h"
#include "tau/log.h"
#include "tau/profiling.h"
#include "tau/reflection.h"
#include "tau/rendering/renderdoc.h"
#include "tau/rendering/renderer.h"
#include "tau/rendering/renderer_types.h"
#include "tau/time.h"
#include "tau/vfs.h"
#include "tau/window.h"
#include "tau/world.h"

// clang-format off
#include "tau/rendering/vulkan.h"
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <filesystem>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL_timer.h>
#include <memory>
#include <utility>
#include <vector>
// clang-format on

namespace tau::engine
{
    namespace
    {
        std::string game_name;
        std::unique_ptr<world_t> active_world;
        bool is_running = true;
        bool is_suspended = false;

        event_callback_t user_event_cb;
        ui_callback_t user_ui_cb;
        post_render_callback_t user_post_render_cb;
    } // namespace

    bool init(const engine_config_t& config)
    {
        // before vfs::init, which names the per game user:// folder after it
        game_name = config.name;
        tau::vfs::init();

        tau::log::init();
        tau::log::set_level(tau::log::level_e::LOG_DEBUG);

        // a release build has no console to print to, so without this a crash leaves nothing behind
        // named after the process, as the editor and a game can share a bin directory
        if (const char* base = SDL_GetBasePath())
        {
            const std::filesystem::path log_path = std::filesystem::path(base) / "logs" / (config.name + ".log");
            if (!tau::log::to_file(log_path.string()))
            {
                TAU_LOG_WARN("ENGINE", "Could not open a log file at {}", log_path.string());
            }
        }

        TAU_LOG_INFO("ENGINE", "Starting engine...");

        // guid map before any load
        tau::assets::register_engine_types();
        const std::string& cooked_root = tau::vfs::cooked_root();
        if (!cooked_root.empty()) { tau::assets::read_guid_map(cooked_root + "/guid_map.json"); }

        tau::reflection::register_types();
        tau::jobs::init();

        // before SDL video and the Vulkan instance
        // RenderDoc's layer has no Wayland surface: X11 (XWayland), SDL_VIDEODRIVER still wins
        if (renderer::renderdoc::init(config.renderdoc, vfs::resolve("user://captures/" + config.name)))
        {
#ifdef __linux__
            if (!config.headless) { SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11"); }
#endif
        }

        // headless: no display, events still run the loop
        SDL_Init(config.headless ? SDL_INIT_EVENTS : SDL_INIT_VIDEO);

        tau::input::detail::init();

        if (volkInitialize() != VK_SUCCESS)
        {
            TAU_LOG_FATAL("ENGINE", "volk initialization failed");
            return false;
        }

        SDL_Window* window = nullptr;
        std::vector<const char*> extensions;
        if (!config.headless)
        {
            const SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
            window = SDL_CreateWindow(game_name.c_str(), config.window_width, config.window_height, window_flags);
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

            u32 sdl_ext_count = 0;
            const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
            extensions.assign(sdl_extensions, sdl_extensions + sdl_ext_count);
        }
        tau::window::set_window(window);

        renderer::context_config_t renderer_config = {
            .app_name = game_name,
            .validation = config.validation,
            .force_compat = config.force_compat,
            .headless = config.headless,
            .required_instance_exts = std::move(extensions),
            .vsync = config.vsync,
        };

        if (!tau::renderer::init(renderer_config, window))
        {
            TAU_LOG_FATAL("ENGINE", "Failed to initialize renderer, aborting...");
            return false;
        }

        active_world = std::make_unique<world_t>();

        return true;
    }

    void run()
    {
        constexpr f64 fixed_timestep = 1.0 / 60.0; // this should be in a settings file later on
        tau::time::fixed_dt = fixed_timestep;
        f64 accumulator = 0.0;

        tau::time::update();

        while (is_running)
        {
            if (!is_suspended)
            {
                tau::renderer::begin_frame();

                tau::time::update();
                f64 frame_time = tau::time::get_dt();

                if (frame_time >= 0.25) { frame_time = 0.25; }

                accumulator += frame_time;

                tau::input::detail::prepare_update();
            }

            static SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                bool handled_by_user = false;
                if (user_event_cb) { handled_by_user = user_event_cb(event); }

                switch (event.type)
                {
                case SDL_EVENT_QUIT: is_running = false; break;
                case SDL_EVENT_WINDOW_RESIZED: tau::renderer::on_window_resized(); break;

                case SDL_EVENT_WILL_ENTER_BACKGROUND:
                case SDL_EVENT_DID_ENTER_BACKGROUND: is_suspended = true; break;

                case SDL_EVENT_WILL_ENTER_FOREGROUND:
                case SDL_EVENT_DID_ENTER_FOREGROUND:
                    is_suspended = false;
                    tau::time::update();
                    break;

                default: break;
                }

                if (!handled_by_user && !is_suspended) { tau::input::detail::process(event); }
            }

            if (is_suspended)
            {
                SDL_Delay(50);
                continue;
            }

            while (accumulator >= fixed_timestep)
            {
                active_world->fixed_update();
                accumulator -= fixed_timestep;
            }

            if (user_ui_cb) { user_ui_cb(); }

            tau::time::fixed_alpha = accumulator / fixed_timestep;
            active_world->update();

            tau::renderer::render(active_world->registry);

            if (user_post_render_cb) { user_post_render_cb(); }

            FrameMark;
        }
    }

    bool shutdown()
    {
        TAU_LOG_INFO("ENGINE", "Stopping engine.");

        vkDeviceWaitIdle(renderer::ctx.device);

        active_world.reset();
        tau::reflection::shutdown();

        tau::renderer::reset_assets();
        tau::assets::shutdown();
        tau::renderer::shutdown();
        tau::jobs::shutdown();
        tau::window::shutdown();
        tau::log::shutdown();

        tau::vfs::shutdown();

        return 0;
    }

    void exit() { is_running = false; }

    std::string get_game_name() { return game_name; }

    world_t& reset_world()
    {
        // the old one goes first, two live worlds would both hold assets
        active_world.reset();
        active_world = std::make_unique<world_t>();
        return *active_world;
    }

    world_t& get_active_world() { return *active_world; }

    void set_event_callback(event_callback_t cb) { user_event_cb = cb; }
    void set_ui_callback(ui_callback_t cb) { user_ui_cb = cb; }
    void set_post_render_callback(post_render_callback_t cb) { user_post_render_cb = cb; }
} // namespace tau::engine