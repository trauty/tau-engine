#pragma once

#include "defines.h"

struct SDL_Window;

namespace tau::window
{
    struct window_size_changed_event
    {
        i32_t width;
        i32_t height;
    };

    void set_window(SDL_Window* new_window);
    TAU_ENGINE_API SDL_Window* get_window();
    TAU_ENGINE_API void get_window_size(i32* width, i32* height);
    // the resize comes back as a window event
    TAU_ENGINE_API void set_window_size(i32 width, i32 height);
    // engine: the window was resized, tells the world
    void on_resized(i32 width, i32 height);
    TAU_ENGINE_API void set_window_position(i32 pos_x, i32 pos_y);
    void shutdown();
} // namespace tau::window