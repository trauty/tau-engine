#include "world.h"

#include "tau/ecs_fwd.h"
#include "tau/physics/physics_sync.h"
#include "tau/physics/physics_world.h"
#include "tau/profiling.h"
#include "tau/systems/transform.h"

namespace tau
{
    void world_t::init()
    {
        registry.ctx().emplace<physics_world_t*>(&physics);

        transform_system::connect(registry);

        physics.init(registry);
    }

    void world_t::begin_play()
    {
        is_simulating = true;
        for (system_func_t system : init_systems) { system(*this); }
    }

    void world_t::end_play()
    {
        for (system_func_t system : shutdown_systems) { system(*this); }
        is_simulating = false;
    }

    void world_t::update()
    {
        ZoneScoped;
        if (!is_simulating) { return; }

        for (system_func_t system : update_systems) { system(*this); }
    }

    void world_t::fixed_update()
    {
        ZoneScoped;
        if (!is_simulating) { return; }

        physics.create_bodies(registry);

        for (system_func_t system : fixed_update_systems) { system(*this); }

        physics::sync_to_physics(registry, physics);
        physics.update();
        physics::sync_from_physics(registry, physics);
    }

    void world_t::shutdown()
    {
        if (is_simulating) { end_play(); }

        physics.destroy_bodies(registry);
        physics.shutdown();

        registry.clear();
        registry.ctx().clear();
    }

    void world_t::register_init_system(system_func_t system) { init_systems.push_back(system); }
    void world_t::register_update_system(system_func_t system) { update_systems.push_back(system); }
    void world_t::register_fixed_update_system(system_func_t system) { fixed_update_systems.push_back(system); }
    void world_t::register_shutdown_system(system_func_t system) { shutdown_systems.push_back(system); }
} // namespace tau
