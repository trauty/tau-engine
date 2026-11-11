#pragma once

#include "tau/defines.h"
#include "tau/os.h"

#include <string>

// registers TAU_REFLECT and hooks on load, close drops them
namespace tau
{
    struct game_library_t
    {
        os::lib_handle_t handle = nullptr;
        void* module = nullptr;
    };

    namespace game_library
    {
        // loads, checks engine build match, hands over the vulkan device
        TAU_ENGINE_API bool open(const std::string& path, game_library_t& out);

        // world and reflection context must be gone first
        // false if jobs still run after timeout_ms: stays loaded
        TAU_ENGINE_API bool close(game_library_t& lib, u32_t timeout_ms = 5000);
    } // namespace game_library
} // namespace tau
