#pragma once

#include "tau/defines.h"

namespace tau { struct world_t; }

// a game: TAU_REFLECT types + hooks below
namespace tau::game
{
    enum class hook_e : u8_t
    {
        // after every load of game code (first + hot reload), scene loaded:
        // register code pointers: input actions, render features, systems, listeners
        LOAD,
        PLAY,
        STOP,
    };

    using hook_fn = void (*)(tau::world_t& world);

    TAU_ENGINE_API void add_hook(hook_e when, hook_fn fn);
    TAU_ENGINE_API void run_hooks(hook_e when, tau::world_t& world);
    TAU_ENGINE_API u32_t remove_hooks_of(void* module_base);

    struct hook_registrar_t
    {
        hook_registrar_t(hook_e when, hook_fn fn) { add_hook(when, fn); }
    };
} // namespace tau::game

#define TAU_GAME_HOOK_CAT2(a, b) a##b
#define TAU_GAME_HOOK_CAT(a, b) TAU_GAME_HOOK_CAT2(a, b)
#define TAU_GAME_HOOK(when, id)                                                                                        \
    static void TAU_GAME_HOOK_CAT(tau_game_hook_, id)(tau::world_t & world);                                           \
    static const tau::game::hook_registrar_t TAU_GAME_HOOK_CAT(tau_game_hook_reg_, id)(                                \
        tau::game::hook_e::when, &TAU_GAME_HOOK_CAT(tau_game_hook_, id));                                              \
    static void TAU_GAME_HOOK_CAT(tau_game_hook_, id)([[maybe_unused]] tau::world_t & world)

#define TAU_ON_LOAD() TAU_GAME_HOOK(LOAD, __COUNTER__)
#define TAU_ON_PLAY() TAU_GAME_HOOK(PLAY, __COUNTER__)
#define TAU_ON_STOP() TAU_GAME_HOOK(STOP, __COUNTER__)

#define TAU_GAME_ENTRY()                                                                                               \
    static_assert(false, "TAU_GAME_ENTRY and the tau_game_* functions are gone: the engine registers the types, "      \
                         "tau_game_init/shutdown become TAU_ON_PLAY/TAU_ON_STOP, tau_game_update an update system")
