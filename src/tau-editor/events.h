#pragma once

#include "tau/defines.h"

namespace tau
{
    struct world_t;
    struct editor_context_t;
} // namespace tau

// modules listen from their init instead of main resetting them by hand
namespace tau::editor::events
{
    enum class event_e : u8_t
    {
        PROJECT_OPENED,
        PLAY_STARTING,
        PLAY_STOPPED,
        COUNT,
    };

    using listener_fn_t = void (*)(world_t& world, editor_context_t& ctx);

    void on(event_e event, listener_fn_t listener);
    void emit(event_e event, world_t& world, editor_context_t& ctx);

    void remove_from_module(const void* module);
} // namespace tau::editor::events
