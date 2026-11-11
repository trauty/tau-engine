#include "guid.h"

#include "tau/components/guid.h"
#include "tau/ecs.h"
#include "tau/guid.h"

#include <unordered_map>

namespace tau::guid_system
{
    namespace
    {
        struct guid_index_t
        { std::unordered_map<u64_t, ecs::entity_t> entities; };

        void on_set(ecs::registry_t& reg, ecs::entity_t entity)
        {
            if (guid_index_t* index = reg.ctx().find<guid_index_t>())
            {
                index->entities[reg.get<entity_guid_t>(entity).value] = entity;
            }
        }

        void on_destroy(ecs::registry_t& reg, ecs::entity_t entity)
        {
            guid_index_t* index = reg.ctx().find<guid_index_t>();
            if (!index) { return; }

            const auto it = index->entities.find(reg.get<entity_guid_t>(entity).value);
            if (it != index->entities.end() && it->second == entity) { index->entities.erase(it); }
        }
    } // namespace

    u64_t ensure(ecs::registry_t& reg, ecs::entity_t entity)
    {
        if (const entity_guid_t* guid = reg.try_get<entity_guid_t>(entity); guid && guid->value != 0)
        {
            return guid->value;
        }

        const u64_t guid = guid::generate();
        assign(reg, entity, guid);
        return guid;
    }

    void assign(ecs::registry_t& reg, ecs::entity_t entity, u64_t guid)
    { reg.emplace_or_replace<entity_guid_t>(entity, guid); }

    u64_t of(const ecs::registry_t& reg, ecs::entity_t entity)
    {
        if (!reg.valid(entity)) { return 0; }

        const entity_guid_t* guid = reg.try_get<entity_guid_t>(entity);
        return guid ? guid->value : 0;
    }

    ecs::entity_t find(const ecs::registry_t& reg, u64_t guid)
    {
        const guid_index_t* index = reg.ctx().find<guid_index_t>();
        if (!index || guid == 0) { return ecs::NULL_ENTITY; }

        const auto it = index->entities.find(guid);
        if (it == index->entities.end()) { return ecs::NULL_ENTITY; }

        return of(reg, it->second) == guid ? it->second : ecs::NULL_ENTITY;
    }

    void connect(ecs::registry_t& reg)
    {
        reg.ctx().emplace<guid_index_t>();
        reg.on_construct<entity_guid_t>().connect<&on_set>();
        reg.on_update<entity_guid_t>().connect<&on_set>();
        reg.on_destroy<entity_guid_t>().connect<&on_destroy>();
    }
} // namespace tau::guid_system
