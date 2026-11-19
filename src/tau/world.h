#pragma once

#include "tau/ecs.h"

#include <string>
#include <vector>

namespace entt { class meta_ctx; }

namespace tau
{
    namespace reflection { using ctx_t = entt::meta_ctx; }
    namespace physics::detail { struct state_t; }

    struct world_t;

    using system_func_t = void (*)(world_t&);

    struct TAU_ENGINE_API world_t
    {
        std::string name;
        ecs::registry_t registry;
        reflection::ctx_t* reflection_ctx = nullptr;
        physics::detail::state_t* physics_state = nullptr;

        std::vector<system_func_t> init_systems;
        std::vector<system_func_t> update_systems;
        std::vector<system_func_t> fixed_update_systems;
        std::vector<system_func_t> shutdown_systems;

        bool is_simulating = false;

        world_t();
        world_t(const world_t&) = delete;
        world_t& operator=(const world_t&) = delete;
        ~world_t();

        // run init/shutdown systems and TAU_ON_PLAY/TAU_ON_STOP, game systems and tweens in between
        void begin_play();
        void end_play();
        void update();
        void fixed_update();

        // register from TAU_ON_LOAD, a reload builds a new world
        void register_init_system(system_func_t system);
        void register_update_system(system_func_t system);
        void register_fixed_update_system(system_func_t system);
        void register_shutdown_system(system_func_t system);
    };
} // namespace tau
