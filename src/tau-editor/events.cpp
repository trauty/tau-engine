#include "events.h"

#include "tau/os.h"

#include <array>
#include <vector>

namespace tau::editor::events
{
    namespace {
        std::array<std::vector<listener_fn_t>, static_cast<std::size_t>(event_e::COUNT)> g_listeners; } // namespace

    void on(event_e event, listener_fn_t listener)
    {
        if (listener) { g_listeners[static_cast<std::size_t>(event)].push_back(listener); }
    }

    void emit(event_e event, world_t& world, editor_context_t& ctx)
    {
        const std::vector<listener_fn_t> listeners = g_listeners[static_cast<std::size_t>(event)];
        for (const listener_fn_t listener : listeners) { listener(world, ctx); }
    }

    void remove_from_module(const void* module)
    {
        if (!module) { return; }

        for (std::vector<listener_fn_t>& listeners : g_listeners)
        {
            std::erase_if(listeners, [module](listener_fn_t listener)
                          { return os::module_base_of(reinterpret_cast<const void*>(listener)) == module; });
        }
    }
} // namespace tau::editor::events
