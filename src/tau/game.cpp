#include "tau/game.h"

#include "tau/os.h"

#include <vector>

namespace tau::game
{
    namespace
    {
        struct hook_t
        {
            hook_e when;
            hook_fn fn;
        };

        std::vector<hook_t>& hooks()
        {
            static std::vector<hook_t> list;
            return list;
        }
    } // namespace

    void add_hook(hook_e when, hook_fn fn)
    {
        if (fn != nullptr) { hooks().push_back({when, fn}); }
    }

    void run_hooks(hook_e when, tau::world_t& world)
    {
        for (const hook_t& hook : hooks())
        {
            if (hook.when == when) { hook.fn(world); }
        }
    }

    u32_t remove_hooks_of(void* module_base)
    {
        return static_cast<u32_t>(
            std::erase_if(hooks(), [module_base](const hook_t& hook)
                          { return os::module_base_of(reinterpret_cast<const void*>(hook.fn)) == module_base; }));
    }
} // namespace tau::game
