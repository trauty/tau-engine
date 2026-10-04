#include "tau/game.h"

#include "tau/os.h"

#include <algorithm>
#include <vector>

namespace tau::game
{
    namespace
    {
        // filled from the game library's static initialisers as it loads
        std::vector<on_load_fn>& hooks()
        {
            static std::vector<on_load_fn> list;
            return list;
        }
    } // namespace

    const char* library_stamp(void* lib)
    {
        using stamp_fn = const char* (*)();
        const stamp_fn stamp = reinterpret_cast<stamp_fn>(os::get_proc_address(lib, "tau_game_engine_stamp"));
        return stamp ? stamp() : nullptr;
    }

    void add_on_load(on_load_fn fn)
    {
        if (fn != nullptr) { hooks().push_back(fn); }
    }

    void run_on_load(tau::world_t& world)
    {
        for (on_load_fn fn : hooks()) { fn(world); }
    }

    u32_t remove_on_load_of(void* module_base)
    {
        std::vector<on_load_fn>& list = hooks();
        const auto first =
            std::remove_if(list.begin(), list.end(), [module_base](on_load_fn fn)
                           { return os::module_base_of(reinterpret_cast<const void*>(fn)) == module_base; });
        const u32_t removed = static_cast<u32_t>(list.end() - first);
        list.erase(first, list.end());
        return removed;
    }
} // namespace tau::game
