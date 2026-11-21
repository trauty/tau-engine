#pragma once

#include "defines.h"
#include "tau/defines.h"
#include "tau/math.h"

#include <functional>
#include <string>

union SDL_Event;

namespace tau::input
{
    using action_id_t = u32_t;
    using listener_id_t = u32_t;

    enum class key_e : u32_t
    {
        Unknown = 0,

        A,
        B,
        C,
        D,
        E,
        F,
        G,
        H,
        I,
        J,
        K,
        L,
        M,
        N,
        O,
        P,
        Q,
        R,
        S,
        T,
        U,
        V,
        W,
        X,
        Y,
        Z,

        Num0,
        Num1,
        Num2,
        Num3,
        Num4,
        Num5,
        Num6,
        Num7,
        Num8,
        Num9,

        Escape,
        Enter,
        Tab,
        Backspace,
        Space,
        LeftShift,
        RightShift,
        LeftCtrl,
        RightCtrl,
        LeftAlt,
        RightAlt,

        Left,
        Right,
        Up,
        Down,

        F1,
        F2,
        F3,
        F4,
        F5,
        F6,
        F7,
        F8,
        F9,
        F10,
        F11,
        F12,

        Count
    };

    enum class mouse_button_e : uint8_t
    {
        Left = 0,
        Middle,
        Right,
        Count
    };

    enum class input_state_t : uint8_t
    {
        PRESSED,
        RELEASED,
        HOLDING
    };

    struct TAU_ENGINE_API input_context_t
    {
        action_id_t action_id;
        input_state_t state;
        key_e key;
    };

    using input_callback_t = std::function<void(const input_context_t&)>;

    TAU_ENGINE_API bool get_key(key_e key);

    TAU_ENGINE_API bool get_key_down(key_e key);

    TAU_ENGINE_API bool get_key_up(key_e key);

    TAU_ENGINE_API bool get_mouse_button(mouse_button_e button);

    TAU_ENGINE_API bool get_mouse_button_down(mouse_button_e button);

    TAU_ENGINE_API bool get_mouse_button_up(mouse_button_e button);

    TAU_ENGINE_API void get_mouse_position(float* x, float* y);

    TAU_ENGINE_API float get_mouse_x();

    TAU_ENGINE_API float get_mouse_y();

    TAU_ENGINE_API vec2 get_mouse_delta();

    TAU_ENGINE_API void set_mouse_relative_mode(bool is_relative);

    TAU_ENGINE_API float get_scroll_delta();

    TAU_ENGINE_API void bind_button(const std::string& action_name, key_e key);

    TAU_ENGINE_API listener_id_t on_action(const std::string& action_name, input_state_t state, input_callback_t callback);

    TAU_ENGINE_API void remove_listener(listener_id_t id);

    // every listener remembers the module that registered it (see os::module_base_of)
    // a game library's go when it unloads, the editor removes them first and TAU_ON_LOAD registers them again
    TAU_ENGINE_API u32_t remove_listeners_of(void* module_base);
    TAU_ENGINE_API u32_t count_listeners_of(void* module_base);

    TAU_ENGINE_API void unbind_button(const std::string& action_name, key_e key);

    TAU_ENGINE_API void unbind_all_buttons(const std::string& action_name);

    TAU_ENGINE_API void set_viewport_offset(float x, float y);
    TAU_ENGINE_API void set_viewport_size(float width, float height);
    TAU_ENGINE_API bool is_mouse_in_viewport();

    namespace detail
    {
        void init();

        void prepare_update();

        void process(const SDL_Event& event);
    } // namespace detail
} // namespace tau::input
