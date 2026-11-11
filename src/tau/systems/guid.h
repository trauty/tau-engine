#pragma once

#include "tau/ecs_fwd.h"


namespace tau::guid_system
{
    TAU_ENGINE_API u64_t ensure(ecs::registry_t& reg, ecs::entity_t entity);
    TAU_ENGINE_API void assign(ecs::registry_t& reg, ecs::entity_t entity, u64_t guid);

    TAU_ENGINE_API u64_t of(const ecs::registry_t& reg, ecs::entity_t entity);
    TAU_ENGINE_API ecs::entity_t find(const ecs::registry_t& reg, u64_t guid);

    void connect(ecs::registry_t& reg);
} // namespace tau::guid_system
