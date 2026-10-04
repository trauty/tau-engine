#pragma once

#include "defines.h"
#include "tau/asset_fwd.h"
#include "tau/world.h"

#include <functional>
#include <memory>

union SDL_Event;

namespace tau::engine
{
    using event_callback_t = std::function<bool(const SDL_Event&)>;
    using ui_callback_t = std::function<void()>;
    using post_render_callback_t = std::function<void()>;

    TAU_ENGINE_API bool init(const std::string& game_name, i32 init_window_width, i32 init_window_height);
    TAU_ENGINE_API void run();
    TAU_ENGINE_API bool shutdown();

    TAU_ENGINE_API void exit();

    TAU_ENGINE_API std::string get_game_name();

    // a game library must be built against the same, see tau::game::library_stamp
    TAU_ENGINE_API const char* build_stamp();
    TAU_ENGINE_API const char* version();

    TAU_ENGINE_API void create_scene();
    TAU_ENGINE_API void set_scene(std::unique_ptr<tau::world_t> scene);
    TAU_ENGINE_API world_t& get_active_world();
    TAU_ENGINE_API asset_registry_t& get_asset_registry();

    TAU_ENGINE_API void set_event_callback(event_callback_t cb);
    TAU_ENGINE_API void set_ui_callback(ui_callback_t cb);
    TAU_ENGINE_API void set_post_render_callback(post_render_callback_t cb);
} // namespace tau::engine