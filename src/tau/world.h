#pragma once

#include "tau/ecs.h"
#include "tau/physics/physics_world.h"

#include <vector>

namespace entt { struct meta_ctx; }

namespace tau
{
    namespace reflection { using ctx_t = entt::meta_ctx; }

    struct world_t;

    using system_func_t = void (*)(world_t&);

    struct TAU_ENGINE_API world_t
    {
        std::string name;
        ecs::registry_t registry;
        reflection::ctx_t* reflection_ctx;
        physics_world_t physics;

        std::vector<system_func_t> init_systems;
        std::vector<system_func_t> update_systems;
        std::vector<system_func_t> fixed_update_systems;
        std::vector<system_func_t> shutdown_systems;

        bool is_simulating = false;

        void init();
        // run the init and shutdown systems, systems only update in between
        void begin_play();
        void end_play();
        void update();
        void fixed_update();
        void shutdown();

        // register from TAU_ON_LOAD, a reload builds a new world
        void register_init_system(system_func_t system);
        void register_update_system(system_func_t system);
        void register_fixed_update_system(system_func_t system);
        void register_shutdown_system(system_func_t system);
    };
} // namespace tau