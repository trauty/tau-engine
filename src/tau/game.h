#pragma once

#include "tau/defines.h"

namespace tau { struct world_t; }

typedef void (*game_register_types_func)(tau::world_t*);
typedef void (*game_init_func)(tau::world_t*);
typedef void (*game_update_func)(tau::world_t*);
typedef void (*game_shutdown_func)(tau::world_t*);

extern "C"
{
    void tau_game_register_types(tau::world_t* world); // only for registering types, no game logic here
    void tau_game_init(tau::world_t* world);
    void tau_game_update(tau::world_t* world);
    void tau_game_shutdown(tau::world_t* world);
}

namespace tau::game
{
    // code run each time the game library loads (first time and every hot reload) to register what points at its code
    // such as input actions, render features, systems and signal listeners
    // tau_game_init runs at every Play, so use TAU_ON_LOAD() { ... }
    using on_load_fn = void (*)(tau::world_t& world);

    // the engine build a game library was compiled against, nullptr for a library from before stamps
    TAU_ENGINE_API const char* library_stamp(void* lib);

    TAU_ENGINE_API void add_on_load(on_load_fn fn);
    TAU_ENGINE_API void run_on_load(tau::world_t& world);
    TAU_ENGINE_API u32_t remove_on_load_of(void* module_base);

    struct on_load_registrar_t
    {
        explicit on_load_registrar_t(on_load_fn fn) { add_on_load(fn); }
    };
} // namespace tau::game

#define TAU_ON_LOAD_CAT2(a, b) a##b
#define TAU_ON_LOAD_CAT(a, b) TAU_ON_LOAD_CAT2(a, b)
#define TAU_ON_LOAD_IMPL(id)                                                                                           \
    static void TAU_ON_LOAD_CAT(tau_on_load_, id)(tau::world_t & world);                                               \
    static const tau::game::on_load_registrar_t TAU_ON_LOAD_CAT(tau_on_load_reg_,                                      \
                                                                id)(&TAU_ON_LOAD_CAT(tau_on_load_, id));               \
    static void TAU_ON_LOAD_CAT(tau_on_load_, id)([[maybe_unused]] tau::world_t & world)
#define TAU_ON_LOAD() TAU_ON_LOAD_IMPL(__COUNTER__)

#ifndef TAU_STATIC_LINK
  // what the entry points below call, so a game compiles with this header alone
    #include "tau/rendering/renderer.h"

    #include <volk.h>

    #define TAU_GAME_ENTRY()                                                                                           \
        extern "C"                                                                                                     \
        {                                                                                                              \
            TAU_GAME_API void tau_game_register_types_internal(tau::world_t* world)                                    \
            {                                                                                                          \
                volkInitialize();                                                                                      \
                volkLoadInstance(tau::renderer::ctx.instance);                                                         \
                volkLoadDevice(tau::renderer::ctx.device);                                                             \
                tau_game_register_types(world);                                                                        \
            }                                                                                                          \
            TAU_GAME_API void tau_game_init_internal(tau::world_t* world) { tau_game_init(world); }                    \
            TAU_GAME_API void tau_game_update_internal(tau::world_t* world) { tau_game_update(world); }                \
            TAU_GAME_API void tau_game_shutdown_internal(tau::world_t* world) { tau_game_shutdown(world); }            \
        }
#else
    #define TAU_GAME_ENTRY()
#endif