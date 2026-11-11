#pragma once

#include "tau/defines.h"

namespace tau { struct world_t; }

namespace tau::physics::detail
{
    struct state_t;

    void create(world_t& world);
    void destroy(world_t& world);

    // create/refresh bodies, contacts() = last step until step
    void begin_step(world_t& world);
    // transforms in, simulate, transforms and contacts out
    void step(world_t& world, f32 dt);
    void end_frame(world_t& world);
    void stop(world_t& world);
} // namespace tau::physics::detail
