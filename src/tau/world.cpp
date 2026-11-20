#include "world.h"

#include "tau/asset.h"
#include "tau/ecs_fwd.h"
#include "tau/game.h"
#include "tau/physics/physics_internal.h"
#include "tau/profiling.h"
#include "tau/reflection.h"
#include "tau/serialization.h"
#include "tau/systems/camera.h"
#include "tau/systems/guid.h"
#include "tau/systems/shadows.h"
#include "tau/systems/transform.h"
#include "tau/time.h"
#include "tau/tween.h"

#include <algorithm>
#include <vector>

namespace tau
{
    namespace
    {
        std::vector<world_t*> g_live_worlds;

        void asset_roots(std::vector<asset_handle_t>& out)
        {
            for (world_t* world : g_live_worlds) { serialization::collect_asset_refs(*world, out); }
        }
    } // namespace

    world_t::world_t() : reflection_ctx(&reflection::get_engine_context())
    {
        transform_system::connect(registry);
        guid_system::connect(registry);

        physics::detail::create(*this);

        g_live_worlds.push_back(this);
        assets::add_roots(&asset_roots);
    }

    world_t::~world_t()
    {
        std::erase(g_live_worlds, this);

        if (is_simulating) { end_play(); }

        physics::detail::destroy(*this);

        registry.clear();
        registry.ctx().clear();
    }

    void world_t::begin_play()
    {
        is_simulating = true;
        for (system_func_t system : init_systems) { system(*this); }
        game::run_hooks(game::hook_e::PLAY, *this);
    }

    void world_t::end_play()
    {
        game::run_hooks(game::hook_e::STOP, *this);
        for (system_func_t system : shutdown_systems) { system(*this); }
        is_simulating = false;
        physics::detail::stop(*this);
        tween::cancel_all(*this);
    }

    void world_t::update()
    {
        ZoneScoped;
        if (is_simulating)
        {
            tween::update(*this, static_cast<f32>(time::get_dt()));
            for (system_func_t system : update_systems) { system(*this); }
            physics::detail::end_frame(*this);
        }

        transform_system::update(registry);
        camera_system::update(registry);
        shadow_system::update(registry);
    }

    void world_t::fixed_update()
    {
        ZoneScoped;
        if (!is_simulating) { return; }

        physics::detail::begin_step(*this);
        for (system_func_t system : fixed_update_systems) { system(*this); }
        physics::detail::step(*this, static_cast<f32>(time::fixed_dt));
    }

    void world_t::register_init_system(system_func_t system) { init_systems.push_back(system); }
    void world_t::register_update_system(system_func_t system) { update_systems.push_back(system); }
    void world_t::register_fixed_update_system(system_func_t system) { fixed_update_systems.push_back(system); }
    void world_t::register_shutdown_system(system_func_t system) { shutdown_systems.push_back(system); }
} // namespace tau
