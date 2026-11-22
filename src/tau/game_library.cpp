#include "tau/game_library.h"

#include "tau/assets/data.h"
#include "tau/engine.h"
#include "tau/game.h"
#include "tau/input.h"
#include "tau/jobs.h"
#include "tau/log.h"
#include "tau/reflection.h"
#include "tau/render/frame.h"
#include "tau/rhi/context.h"

#include <string_view>

namespace tau::game_library
{
    namespace
    {
        // both generated into every game library by its build, see tau_stamp.lua
        using stamp_fn = const char* (*)();
        using load_vulkan_fn = void (*)(VkInstance instance, VkDevice device);

        void discard(game_library_t& lib)
        {
            reflection::remove_registrations_of(lib.module);
            game::remove_hooks_of(lib.module);
            data::remove_types_of(lib.module);
            os::free_lib(lib.handle);
            lib = {};
        }
    } // namespace

    bool open(const std::string& path, game_library_t& out)
    {
        out = {};
        out.handle = os::load_lib(path.c_str());
        if (!out.handle)
        {
            TAU_LOG_ERROR("ENGINE", "Failed to load the game library {}: {}", path, os::lib_error());
            return false;
        }
        out.module = os::module_base_of_lib(out.handle);

        const auto stamp_of = reinterpret_cast<stamp_fn>(os::get_proc_address(out.handle, "tau_game_engine_stamp"));
        const char* stamp = stamp_of ? stamp_of() : nullptr;
        if (!stamp || std::string_view(stamp) != engine::build_stamp())
        {
            TAU_LOG_ERROR("ENGINE",
                          "The game library was built against a different engine build (library: {}, engine: {}). "
                          "Build the engine and the project again.",
                          stamp ? stamp : "none", engine::build_stamp());
            discard(out);
            return false;
        }

        const auto load_vulkan =
            reinterpret_cast<load_vulkan_fn>(os::get_proc_address(out.handle, "tau_game_load_vulkan"));
        if (!load_vulkan)
        {
            TAU_LOG_ERROR("ENGINE", "{} is not a tau game library, it has no tau_game_load_vulkan", path);
            discard(out);
            return false;
        }
        load_vulkan(renderer::ctx.instance, renderer::ctx.device);
        return true;
    }

    bool close(game_library_t& lib, u32_t timeout_ms)
    {
        if (!lib.handle) { return true; }

        const u32_t listeners = input::remove_listeners_of(lib.module);
        const u32_t features = renderer::frame::remove_producers_of(lib.module);
        const u32_t hooks = game::remove_hooks_of(lib.module);
        reflection::remove_registrations_of(lib.module);

        if (!jobs::wait_idle(timeout_ms))
        {
            TAU_LOG_WARN("ENGINE", "Kept the game library loaded: jobs were still running after {} ms", timeout_ms);
            return false;
        }

        // its decoded values run its destructors
        const u32_t data_types = data::remove_types_of(lib.module);

        os::free_lib(lib.handle);
        lib = {};

        TAU_LOG_INFO("ENGINE",
                     "Unloaded the game library (released {} input listener(s), {} render feature(s), {} hook(s), {} "
                     "data type(s))",
                     listeners, features, hooks, data_types);
        return true;
    }
} // namespace tau::game_library
