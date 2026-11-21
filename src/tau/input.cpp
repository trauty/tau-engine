#include "input.h"

#include "SDL3/SDL_mouse.h"
#include "tau/containers/flat_map.h"
#include "tau/hash.h"
#include "tau/os.h"
#include "tau/window.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_scancode.h>
#include <algorithm>
#include <atomic>
#include <cstring>

namespace tau::input
{
    struct input_frame_state_t
    {
        bool cur_key[(size_t)key_e::Count];
        bool prev_key[(size_t)key_e::Count];

        bool cur_mouse[(size_t)key_e::Count];
        bool prev_mouse[(size_t)key_e::Count];

        float raw_mouse_x = 0.0f;
        float raw_mouse_y = 0.0f;

        float mouse_x = 0.0f;
        float mouse_y = 0.0f;
        float delta_mouse_x = 0.0f;
        float delta_mouse_y = 0.0f;
        float scroll_delta = 0.0f;

        float viewport_offset_x = 0.0f;
        float viewport_offset_y = 0.0f;
        float viewport_width = 0.0f;
        float viewport_height = 0.0f;
    };

    struct action_listener_t
    {
        listener_id_t id;
        input_state_t triggerState;
        input_callback_t callback;
        void* owner = nullptr; // module that registered it; its code is what the callback runs
    };

    namespace
    {
        input_frame_state_t input_state;
        std::vector<key_e> scancode_map;
        std::vector<std::vector<action_id_t>> keybinds;
        flat_map_t<std::vector<action_listener_t>> listeners;

        void build_lookup_table()
        {
            scancode_map.resize(512, key_e::Unknown);

            scancode_map[SDL_SCANCODE_A] = key_e::A;
            scancode_map[SDL_SCANCODE_B] = key_e::B;
            scancode_map[SDL_SCANCODE_C] = key_e::C;
            scancode_map[SDL_SCANCODE_D] = key_e::D;
            scancode_map[SDL_SCANCODE_E] = key_e::E;
            scancode_map[SDL_SCANCODE_F] = key_e::F;
            scancode_map[SDL_SCANCODE_G] = key_e::G;
            scancode_map[SDL_SCANCODE_H] = key_e::H;
            scancode_map[SDL_SCANCODE_I] = key_e::I;
            scancode_map[SDL_SCANCODE_J] = key_e::J;
            scancode_map[SDL_SCANCODE_K] = key_e::K;
            scancode_map[SDL_SCANCODE_L] = key_e::L;
            scancode_map[SDL_SCANCODE_M] = key_e::M;
            scancode_map[SDL_SCANCODE_N] = key_e::N;
            scancode_map[SDL_SCANCODE_O] = key_e::O;
            scancode_map[SDL_SCANCODE_P] = key_e::P;
            scancode_map[SDL_SCANCODE_Q] = key_e::Q;
            scancode_map[SDL_SCANCODE_R] = key_e::R;
            scancode_map[SDL_SCANCODE_S] = key_e::S;
            scancode_map[SDL_SCANCODE_T] = key_e::T;
            scancode_map[SDL_SCANCODE_U] = key_e::U;
            scancode_map[SDL_SCANCODE_V] = key_e::V;
            scancode_map[SDL_SCANCODE_W] = key_e::W;
            scancode_map[SDL_SCANCODE_X] = key_e::X;
            scancode_map[SDL_SCANCODE_Y] = key_e::Y;
            scancode_map[SDL_SCANCODE_Z] = key_e::Z;

            scancode_map[SDL_SCANCODE_0] = key_e::Num0;
            scancode_map[SDL_SCANCODE_1] = key_e::Num1;
            scancode_map[SDL_SCANCODE_2] = key_e::Num2;
            scancode_map[SDL_SCANCODE_3] = key_e::Num3;
            scancode_map[SDL_SCANCODE_4] = key_e::Num4;
            scancode_map[SDL_SCANCODE_5] = key_e::Num5;
            scancode_map[SDL_SCANCODE_6] = key_e::Num6;
            scancode_map[SDL_SCANCODE_7] = key_e::Num7;
            scancode_map[SDL_SCANCODE_8] = key_e::Num8;
            scancode_map[SDL_SCANCODE_9] = key_e::Num9;

            scancode_map[SDL_SCANCODE_ESCAPE] = key_e::Escape;
            scancode_map[SDL_SCANCODE_RETURN] = key_e::Enter;
            scancode_map[SDL_SCANCODE_TAB] = key_e::Tab;
            scancode_map[SDL_SCANCODE_BACKSPACE] = key_e::Backspace;
            scancode_map[SDL_SCANCODE_SPACE] = key_e::Space;
            scancode_map[SDL_SCANCODE_LSHIFT] = key_e::LeftShift;
            scancode_map[SDL_SCANCODE_RSHIFT] = key_e::RightShift;
            scancode_map[SDL_SCANCODE_LCTRL] = key_e::LeftCtrl;
            scancode_map[SDL_SCANCODE_RCTRL] = key_e::RightCtrl;
            scancode_map[SDL_SCANCODE_LALT] = key_e::LeftAlt;
            scancode_map[SDL_SCANCODE_RALT] = key_e::RightAlt;

            scancode_map[SDL_SCANCODE_LEFT] = key_e::Left;
            scancode_map[SDL_SCANCODE_RIGHT] = key_e::Right;
            scancode_map[SDL_SCANCODE_UP] = key_e::Up;
            scancode_map[SDL_SCANCODE_DOWN] = key_e::Down;

            scancode_map[SDL_SCANCODE_F1] = key_e::F1;
            scancode_map[SDL_SCANCODE_F2] = key_e::F2;
            scancode_map[SDL_SCANCODE_F3] = key_e::F3;
            scancode_map[SDL_SCANCODE_F4] = key_e::F4;
            scancode_map[SDL_SCANCODE_F5] = key_e::F5;
            scancode_map[SDL_SCANCODE_F6] = key_e::F6;
            scancode_map[SDL_SCANCODE_F7] = key_e::F7;
            scancode_map[SDL_SCANCODE_F8] = key_e::F8;
            scancode_map[SDL_SCANCODE_F9] = key_e::F9;
            scancode_map[SDL_SCANCODE_F10] = key_e::F10;
            scancode_map[SDL_SCANCODE_F11] = key_e::F11;
            scancode_map[SDL_SCANCODE_F12] = key_e::F12;
        }

        void dispatch_action(key_e key_t, input_state_t state)
        {
            if ((size_t)key_t >= keybinds.size()) { return; }

            const std::vector<action_id_t>& actions = keybinds[(size_t)key_t];
            for (action_id_t id : actions)
            {
                const std::vector<action_listener_t>* iter = listeners.find(id);
                if (iter != nullptr)
                {
                    input_context_t ctx = {id, state, key_t};
                    for (const action_listener_t& listener : *iter)
                    {
                        if (listener.triggerState == state) { listener.callback(ctx); }
                    }
                }
            }
        }

        void processHoldEvents()
        {
            for (size_t i = 0; i < (size_t)key_e::Count; i++)
            {
                if (input_state.cur_key[i]) { dispatch_action((key_e)i, input_state_t::HOLDING); }
            }
        }
    } // namespace

    bool get_key(key_e key) { return input_state.cur_key[(i32_t)key]; }

    bool get_key_down(key_e key) { return input_state.cur_key[(i32_t)key] && !input_state.prev_key[(i32_t)key]; }

    bool get_key_up(key_e key) { return !input_state.cur_key[(i32_t)key] && input_state.prev_key[(i32_t)key]; }

    bool get_mouse_button(mouse_button_e button) { return input_state.cur_mouse[(i32_t)button]; }

    bool get_mouse_button_down(mouse_button_e button)
    { return input_state.cur_mouse[(i32_t)button] && !input_state.prev_mouse[(i32_t)button]; }

    bool get_mouse_button_up(mouse_button_e button)
    { return !input_state.cur_mouse[(i32_t)button] && input_state.prev_mouse[(i32_t)button]; }

    void get_mouse_position(float* x, float* y)
    {
        *x = input_state.mouse_x;
        *y = input_state.mouse_y;
    }

    float get_mouse_x() { return input_state.mouse_x; }

    float get_mouse_y() { return input_state.mouse_y; }

    vec2 get_mouse_delta() { return {input_state.delta_mouse_x, input_state.delta_mouse_y}; }

    void set_mouse_relative_mode(bool is_relative)
    { SDL_SetWindowRelativeMouseMode(tau::window::get_window(), is_relative); }

    float get_scroll_delta() { return input_state.scroll_delta; }

    void bind_button(const std::string& action_name, key_e key)
    {
        if ((size_t)key >= keybinds.size()) { return; }

        action_id_t id = tau::hash_string(action_name.c_str());

        std::vector<action_id_t>& bindings = keybinds[(size_t)key];
        if (std::find(bindings.begin(), bindings.end(), id) == bindings.end()) { bindings.push_back(id); }
    }

    // never inlined: the return address must be the caller's
    // or a listener registered inside an engine function is filed under that function's caller
    TAU_NOINLINE listener_id_t on_action(const std::string& action_name, input_state_t state, input_callback_t callback)
    {
        action_id_t id = tau::hash_string(action_name.c_str());

        static std::atomic<u32_t> nextId{0};
        listener_id_t listener_id_t = nextId.fetch_add(1, std::memory_order_relaxed);

        listeners[id].push_back({listener_id_t, state, callback, os::module_base_of(TAU_CALLER_ADDRESS())});
        return listener_id_t;
    }

    u32_t remove_listeners_of(void* module_base)
    {
        u32_t removed = 0;
        for (auto [action_id, action_listeners] : listeners)
        {
            const auto first = std::remove_if(action_listeners.begin(), action_listeners.end(),
                                              [module_base](const action_listener_t& listener)
                                              { return listener.owner == module_base; });
            removed += static_cast<u32_t>(action_listeners.end() - first);
            action_listeners.erase(first, action_listeners.end());
        }
        return removed;
    }

    u32_t count_listeners_of(void* module_base)
    {
        u32_t count = 0;
        for (auto [action_id, action_listeners] : listeners)
        {
            for (const action_listener_t& listener : action_listeners)
            {
                if (listener.owner == module_base) { count++; }
            }
        }
        return count;
    }

    void remove_listener(listener_id_t id)
    {
        for (auto [action_id, action_listeners] : listeners)
        {
            auto iter = std::remove_if(action_listeners.begin(), action_listeners.end(),
                                       [id](const action_listener_t& listener) { return listener.id == id; });

            if (iter != action_listeners.end())
            {
                action_listeners.erase(iter, action_listeners.end());
                return;
            }
        }
    }

    void unbind_button(const std::string& action_name, key_e key)
    {
        if ((size_t)key >= keybinds.size()) { return; }

        action_id_t id = tau::hash_string(action_name.c_str());
        std::vector<action_id_t>& bindings = keybinds[(size_t)key];

        bindings.erase(std::remove(bindings.begin(), bindings.end(), id), bindings.end());
    }

    void unbind_all_buttons(const std::string& action_name)
    {
        action_id_t id = tau::hash_string(action_name.c_str());

        for (std::vector<action_id_t>& bindings : keybinds)
        {
            bindings.erase(std::remove(bindings.begin(), bindings.end(), id), bindings.end());
        }
    }

    void set_viewport_offset(float x, float y)
    {
        input_state.viewport_offset_x = x;
        input_state.viewport_offset_y = y;

        input_state.mouse_x = input_state.raw_mouse_x - input_state.viewport_offset_x;
        input_state.mouse_y = input_state.raw_mouse_y - input_state.viewport_offset_y;
    }

    void set_viewport_size(float width, float height)
    {
        input_state.viewport_width = width;
        input_state.viewport_height = height;
    }

    bool is_mouse_in_viewport()
    {
        if (input_state.viewport_width == 0.0f || input_state.viewport_height == 0.0f) { return true; }

        return input_state.mouse_x >= 0.0f && input_state.mouse_x <= input_state.viewport_width &&
               input_state.mouse_y >= 0.0f && input_state.mouse_y <= input_state.viewport_height;
    }

    namespace detail
    {
        void init()
        {
            std::memset(&input_state, 0, sizeof(input_state_t));
            build_lookup_table();
            keybinds.resize((size_t)key_e::Count);
        }

        void prepare_update()
        {
            std::memcpy(input_state.prev_key, input_state.cur_key, sizeof(input_state.cur_key));
            std::memcpy(input_state.prev_mouse, input_state.cur_mouse, sizeof(input_state.cur_mouse));
            input_state.scroll_delta = 0.0f;

            input_state.delta_mouse_x = 0.0f;
            input_state.delta_mouse_y = 0.0f;

            processHoldEvents();
        }

        void process(const SDL_Event& event)
        {
            switch (event.type)
            {
            case SDL_EVENT_KEY_DOWN:
                if (event.key.repeat == 0 && event.key.scancode < scancode_map.size())
                {
                    key_e key_t = scancode_map[event.key.scancode];
                    if (key_t != key_e::Unknown)
                    {
                        input_state.cur_key[(i32_t)key_t] = true;
                        dispatch_action(key_t, input_state_t::PRESSED);
                    }
                }
                break;

            case SDL_EVENT_KEY_UP:
                if (event.key.scancode < scancode_map.size())
                {
                    key_e key = scancode_map[event.key.scancode];
                    if (key != key_e::Unknown)
                    {
                        input_state.cur_key[(i32_t)key] = false;
                        dispatch_action(key, input_state_t::RELEASED);
                    }
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.button > 0 && event.button.button <= 3)
                {
                    input_state.cur_mouse[event.button.button - 1] = true;
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button > 0 && event.button.button <= 3)
                {
                    input_state.cur_mouse[event.button.button - 1] = false;
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                input_state.raw_mouse_x = event.motion.x;
                input_state.raw_mouse_y = event.motion.y;

                input_state.mouse_x = input_state.raw_mouse_x - input_state.viewport_offset_x;
                input_state.mouse_y = input_state.raw_mouse_y - input_state.viewport_offset_y;

                input_state.delta_mouse_x += event.motion.xrel;
                input_state.delta_mouse_y += event.motion.yrel;

                break;

            case SDL_EVENT_MOUSE_WHEEL: input_state.scroll_delta = event.wheel.y; break;
            }
        }
    } // namespace detail
} // namespace tau::input