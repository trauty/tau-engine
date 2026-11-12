#include "window.h"

#include "tau/defines.h"
#include "tau/engine.h"
#include "tau/events.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

namespace tau::window
{
    namespace { SDL_Window* window; }

    void set_window(SDL_Window* new_window) { window = new_window; }

    SDL_Window* get_window() { return window; }

    void get_window_size(i32* width, i32* height) { SDL_GetWindowSizeInPixels(window, width, height); }

    void set_window_size(i32 width, i32 height) { SDL_SetWindowSize(window, width, height); }

    void on_resized(i32 width, i32 height)
    { tau::events::emit<window_size_changed_event>(tau::engine::get_active_world().registry, width, height); }

    void set_window_position(i32 pos_x, i32 pos_y) { SDL_SetWindowPosition(window, pos_x, pos_y); }

    void shutdown()
    {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        SDL_Quit();
    }
} // namespace tau::window