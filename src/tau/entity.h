#pragma once

#include "tau/components/tag.h"
#include "tau/components/transform.h"
#include "tau/defines.h"
#include "tau/guid.h"
#include "tau/systems/guid.h"
#include "tau/systems/transform.h"
#include "tau/world.h"

#include <string>
#include <string_view>
#include <utility>

namespace tau
{
    struct entity_ref_t;

    // the children in sibling order; the next one is read before a child is handed out, so destroying it is fine
    struct entity_children_t
    {
        struct iterator_t
        {
            world_t* world = nullptr;
            ecs::entity_t current = ecs::NULL_ENTITY;
            ecs::entity_t next = ecs::NULL_ENTITY;

            entity_ref_t operator*() const;
            iterator_t& operator++()
            {
                current = next;
                next = sibling_of(current);
                return *this;
            }
            bool operator==(const iterator_t& other) const { return current == other.current; }

            ecs::entity_t sibling_of(ecs::entity_t entity) const
            {
                if (entity == ecs::NULL_ENTITY) { return ecs::NULL_ENTITY; }
                const transform_t* transform = world->registry.try_get<transform_t>(entity);
                return transform ? transform->get_next_sibling() : ecs::NULL_ENTITY;
            }
        };

        world_t* world = nullptr;
        ecs::entity_t first = ecs::NULL_ENTITY;

        iterator_t begin() const
        {
            iterator_t it{world, first, ecs::NULL_ENTITY};
            it.next = it.sibling_of(first);
            return it;
        }
        iterator_t end() const { return {world, ecs::NULL_ENTITY, ecs::NULL_ENTITY}; }
    };

    // an entity of a world, for code that runs now; never stored in a component (it holds the world pointer):
    // components keep ecs::entity_t, which saves as a guid
    struct entity_ref_t
    {
        world_t* world = nullptr;
        ecs::entity_t id = ecs::NULL_ENTITY;

        bool valid() const { return world != nullptr && world->registry.valid(id); }
        explicit operator bool() const { return valid(); }
        bool operator==(const entity_ref_t& other) const { return world == other.world && id == other.id; }

        template <typename T>
        bool has() const
        { return valid() && world->registry.all_of<T>(id); }

        template <typename T>
        T& get() const
        { return world->registry.get<T>(id); }

        template <typename T>
        T* try_get() const
        { return valid() ? world->registry.try_get<T>(id) : nullptr; }

        // replaces one already there, as the inspector does
        template <typename T, typename... Args>
        T& add(Args&&... args) const
        {
            T& component = world->registry.emplace_or_replace<T>(id, std::forward<Args>(args)...);
            if constexpr (requires { component.on_added(); }) { component.on_added(); }
            return component;
        }

        template <typename T>
        void remove() const
        {
            if (valid()) { world->registry.remove<T>(id); }
        }

        // the children follow at the next transform update
        void destroy() const
        {
            if (valid()) { world->registry.destroy(id); }
        }

        const std::string& name() const
        {
            static const std::string none;
            const tag_t* tag = try_get<tag_t>();
            return tag ? tag->name : none;
        }

        void set_name(std::string_view name) const { add<tag_t>(std::string(name)); }

        // 0 without one
        u64_t guid() const { return valid() ? guid_system::of(world->registry, id) : 0; }

        entity_ref_t parent() const
        {
            const transform_t* transform = try_get<transform_t>();
            return {world, transform ? transform->get_parent() : ecs::NULL_ENTITY};
        }

        // an invalid parent makes it a root
        void set_parent(entity_ref_t new_parent, bool keep_world = false) const
        {
            const ecs::entity_t parent_id = new_parent.valid() ? new_parent.id : ecs::NULL_ENTITY;
            if (keep_world) { transform_system::set_parent_keep_world(world->registry, id, parent_id); }
            else
            {
                transform_system::set_parent(world->registry, id, parent_id);
            }
        }

        entity_children_t children() const
        {
            const transform_t* transform = try_get<transform_t>();
            return {world, transform ? transform->get_first_child() : ecs::NULL_ENTITY};
        }

        // the shown pose as of the last update (needs a transform_t)
        mat4 world_matrix() const { return transform_system::world_matrix(world->registry, id); }
        vec3 world_position() const { return transform_system::get_world_position(world->registry, id); }
        quat world_rotation() const { return transform_system::get_world_rotation(world->registry, id); }

        // a stored id of the same world
        entity_ref_t other(ecs::entity_t entity) const { return {world, entity}; }
    };

    inline entity_ref_t entity_children_t::iterator_t::operator*() const { return {world, current}; }

    namespace entity
    {
        // with a guid (0: a new one), a tag_t and a transform_t
        inline entity_ref_t create(world_t& world, std::string_view name = "Entity", u64_t guid = 0)
        {
            const ecs::entity_t id = world.registry.create();
            guid_system::assign(world.registry, id, guid != 0 ? guid : guid::generate());
            world.registry.emplace<tag_t>(id, std::string(name));
            world.registry.emplace<transform_t>(id);
            return {&world, id};
        }

        inline entity_ref_t find(world_t& world, u64_t guid)
        { return {&world, guid_system::find(world.registry, guid)}; }
    } // namespace entity
} // namespace tau
