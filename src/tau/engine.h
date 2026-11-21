#pragma once

#include "defines.h"
#include "tau/world.h"

#include <functional>

union SDL_Event;

namespace tau::engine
{
    using event_callback_t = std::function<bool(const SDL_Event&)>;
    using ui_callback_t = std::function<void()>;
    using post_render_callback_t = std::function<void()>;

#ifdef NDEBUG
    inline constexpr bool DEBUG_BUILD = false;
#else
    inline constexpr bool DEBUG_BUILD = true;
#endif

    struct engine_config_t
    {
        // names the window and the per game user:// folder
        std::string name = "tau";
        i32 window_width = 1280;
        i32 window_height = 720;
        // off: uncapped presentation, for timing runs
        bool vsync = true;
        // validation layers with synchronization validation
        bool validation = DEBUG_BUILD;
        // no hardware ray tracing even where the GPU has it: the minimum spec
        bool force_compat = false;
        // no window and no SDL video, offscreen rendering only (needs no display)
        bool headless = false;
    };

    TAU_ENGINE_API bool init(const engine_config_t& config);
    TAU_ENGINE_API void run();
    TAU_ENGINE_API bool shutdown();

    TAU_ENGINE_API void exit();

    TAU_ENGINE_API std::string get_game_name();

    // a game library must be built against the same, see tau::game::library_stamp
    TAU_ENGINE_API const char* build_stamp();
    TAU_ENGINE_API const char* version();

    // ends Play in the old world and destroys it, so its game library must still be loaded
    TAU_ENGINE_API world_t& reset_world();
    TAU_ENGINE_API world_t& get_active_world();

    TAU_ENGINE_API void set_event_callback(event_callback_t cb);
    TAU_ENGINE_API void set_ui_callback(ui_callback_t cb);
    TAU_ENGINE_API void set_post_render_callback(post_render_callback_t cb);
} // namespace tau::engine