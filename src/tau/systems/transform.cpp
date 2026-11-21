#include "transform.h"

#include "tau/components/transform.h"
#include "tau/defines.h"
#include "tau/ecs.h"
#include "tau/ecs_fwd.h"
#include "tau/log.h"
#include "tau/math.h"
#include "tau/profiling.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace tau::transform_system
{
    namespace
    {
        // per registry, parents before children
        struct order_t
        {
            std::vector<ecs::entity_t> stack;
            std::vector<ecs::entity_t> sorted;
            bool dirty = true;
        };

        void mark_dirty(ecs::registry_t& reg) { reg.ctx().emplace<order_t>().dirty = true; }

        struct orphans_t
        { std::vector<ecs::entity_t> entities; };

        // top level transforms are linked siblings too
        struct roots_t
        {
            ecs::entity_t first = ecs::NULL_ENTITY;
            ecs::entity_t last = ecs::NULL_ENTITY;
        };

        template <typename T>
        bool same_bits(const T& a, const T& b)
        { return std::memcmp(&a, &b, sizeof(T)) == 0; }

        void build_local(const vec3& position, const quat& rotation, const vec3& scale, mat4& out)
        { out = trs(position, rotation, scale); }
    } // namespace

    struct internal_t
    {
        static void unlink(ecs::registry_t& reg, ecs::entity_t entity, transform_t& transform)
        {
            if (transform.parent != ecs::NULL_ENTITY)
            {
                transform_t& parent_transform = reg.get<transform_t>(transform.parent);
                if (parent_transform.first_child == entity) { parent_transform.first_child = transform.next_sibling; }
            }
            else if (roots_t* roots = reg.ctx().find<roots_t>())
            {
                if (roots->first == entity) { roots->first = transform.next_sibling; }
                if (roots->last == entity) { roots->last = transform.prev_sibling; }
            }
            if (transform.prev_sibling != ecs::NULL_ENTITY)
            {
                reg.get<transform_t>(transform.prev_sibling).next_sibling = transform.next_sibling;
            }
            if (transform.next_sibling != ecs::NULL_ENTITY)
            {
                reg.get<transform_t>(transform.next_sibling).prev_sibling = transform.prev_sibling;
            }

            transform.parent = ecs::NULL_ENTITY;
            transform.next_sibling = ecs::NULL_ENTITY;
            transform.prev_sibling = ecs::NULL_ENTITY;
        }

        // before `before` if it is a sibling, else last
        static void link(ecs::registry_t& reg, ecs::entity_t entity, transform_t& transform, ecs::entity_t parent,
                         ecs::entity_t before)
        {
            transform.parent = parent;

            roots_t* roots = parent == ecs::NULL_ENTITY ? reg.ctx().find<roots_t>() : nullptr;
            if (parent == ecs::NULL_ENTITY && !roots) { return; }

            ecs::entity_t& first = roots ? roots->first : reg.get<transform_t>(parent).first_child;

            transform_t* before_transform = before != ecs::NULL_ENTITY && before != entity && reg.valid(before)
                                                ? reg.try_get<transform_t>(before)
                                                : nullptr;
            // top level without predecessor and not first: orphan about to go
            const bool before_linked =
                before_transform && before_transform->parent == parent &&
                (parent != ecs::NULL_ENTITY || before_transform->prev_sibling != ecs::NULL_ENTITY || first == before);

            if (before_linked)
            {
                transform.next_sibling = before;
                transform.prev_sibling = before_transform->prev_sibling;
                if (before_transform->prev_sibling != ecs::NULL_ENTITY)
                {
                    reg.get<transform_t>(before_transform->prev_sibling).next_sibling = entity;
                }
                else
                {
                    first = entity;
                }
                before_transform->prev_sibling = entity;
                return;
            }

            ecs::entity_t last = roots ? roots->last : first;
            if (!roots && last != ecs::NULL_ENTITY)
            {
                while (reg.get<transform_t>(last).next_sibling != ecs::NULL_ENTITY)
                {
                    last = reg.get<transform_t>(last).next_sibling;
                }
            }

            transform.prev_sibling = last;
            transform.next_sibling = ecs::NULL_ENTITY;
            if (last != ecs::NULL_ENTITY) { reg.get<transform_t>(last).next_sibling = entity; }
            else
            {
                first = entity;
            }
            if (roots) { roots->last = entity; }
        }

        static void set_parent(ecs::registry_t& reg, ecs::entity_t child, ecs::entity_t parent, ecs::entity_t before)
        {
            for (ecs::entity_t e = parent; e != ecs::NULL_ENTITY; e = reg.get<transform_t>(e).parent)
            {
                if (e == child)
                {
                    TAU_LOG_WARN("TRANSFORM", "Can't parent an entity to itself or one of its children");
                    return;
                }
            }

            transform_t& child_transform = reg.get<transform_t>(child);
            unlink(reg, child, child_transform);
            link(reg, child, child_transform, parent, before);

            mark_dirty(reg);
            child_transform.is_dirty = true;
        }

        static void on_construct(ecs::registry_t& reg, ecs::entity_t entity)
        {
            transform_t& transform = reg.get<transform_t>(entity);
            transform.parent = ecs::NULL_ENTITY;
            transform.first_child = ecs::NULL_ENTITY;
            transform.next_sibling = ecs::NULL_ENTITY;
            transform.prev_sibling = ecs::NULL_ENTITY;

            link(reg, entity, transform, ecs::NULL_ENTITY, ecs::NULL_ENTITY);
            mark_dirty(reg);
        }

        // children are destroyed in the next update, destroying them inside entt's signal breaks range destroy
        static void on_destroy(ecs::registry_t& reg, ecs::entity_t entity)
        {
            transform_t& transform = reg.get<transform_t>(entity);
            unlink(reg, entity, transform);

            orphans_t* orphans = reg.ctx().find<orphans_t>();

            for (ecs::entity_t child = transform.first_child; child != ecs::NULL_ENTITY;)
            {
                transform_t& child_transform = reg.get<transform_t>(child);
                if (orphans) { orphans->entities.push_back(child); }

                const ecs::entity_t next = child_transform.next_sibling;
                child_transform.parent = ecs::NULL_ENTITY;
                child_transform.next_sibling = ecs::NULL_ENTITY;
                child_transform.prev_sibling = ecs::NULL_ENTITY;
                child = next;
            }
            transform.first_child = ecs::NULL_ENTITY;

            mark_dirty(reg);
        }

        static void destroy_orphans(ecs::registry_t& reg)
        {
            orphans_t* orphans = reg.ctx().find<orphans_t>();
            if (!orphans) { return; }

            while (!orphans->entities.empty())
            {
                const ecs::entity_t entity = orphans->entities.back();
                orphans->entities.pop_back();
                if (reg.valid(entity)) { reg.destroy(entity); }
            }
        }

        static void rebuild_hierarchy(ecs::registry_t& reg)
        {
            auto view = reg.view<transform_t>();

            order_t& order = reg.ctx().emplace<order_t>();
            order.stack.clear();
            order.sorted.clear();

            for (ecs::entity_t entity : view)
            {
                if (view.get<transform_t>(entity).parent == ecs::NULL_ENTITY) { order.stack.push_back(entity); }
            }

            // iterative dfs flatten
            while (!order.stack.empty())
            {
                ecs::entity_t entity = order.stack.back();
                order.stack.pop_back();

                order.sorted.push_back(entity);

                ecs::entity_t child = view.get<transform_t>(entity).first_child;
                while (child != ecs::NULL_ENTITY)
                {
                    order.stack.push_back(child);
                    child = view.get<transform_t>(child).next_sibling;
                }
            }

            order.dirty = false;
        }

        static void update(ecs::registry_t& reg)
        {
            ZoneScoped;

            destroy_orphans(reg);

            order_t& order = reg.ctx().emplace<order_t>();
            if (order.dirty) { rebuild_hierarchy(reg); }

            auto view = reg.view<transform_t>();
            auto& shown = reg.storage<shown_pose_t>();

            for (ecs::entity_t entity : order.sorted)
            {
                transform_t& transform = view.get<transform_t>(entity);

                const shown_pose_t* pose = shown.contains(entity) ? &shown.get(entity) : nullptr;
                const vec3& position = pose ? pose->position : transform.local_position;
                const quat& rotation = pose ? pose->rotation : transform.local_rotation;

                if (transform.is_dirty || !same_bits(position, transform.built_position) ||
                    !same_bits(rotation, transform.built_rotation) ||
                    !same_bits(transform.local_scale, transform.built_scale))
                {
                    build_local(position, rotation, transform.local_scale, transform.local_mat);

                    transform.built_position = position;
                    transform.built_rotation = rotation;
                    transform.built_scale = transform.local_scale;
                    transform.is_dirty = true;
                }

                if (transform.parent != ecs::NULL_ENTITY)
                {
                    transform_t& parent_transform = view.get<transform_t>(transform.parent);

                    if (transform.is_dirty || parent_transform.is_dirty)
                    {
                        transform.world_mat = parent_transform.world_mat * transform.local_mat;
                        transform.is_dirty = true;
                    }
                }
                else if (transform.is_dirty) { transform.world_mat = transform.local_mat; }
            }

            for (ecs::entity_t entity : order.sorted) { view.get<transform_t>(entity).is_dirty = false; }
        }
    };

    void set_local_position(ecs::registry_t& reg, ecs::entity_t entity, vec3 new_pos)
    {
        transform_t& transform = reg.get<transform_t>(entity);
        transform.local_position = new_pos;
    }

    void set_local_rotation(ecs::registry_t& reg, ecs::entity_t entity, quat new_rot)
    {
        transform_t& transform = reg.get<transform_t>(entity);
        transform.local_rotation = new_rot;
    }

    void set_local_scale(ecs::registry_t& reg, ecs::entity_t entity, vec3 new_scale)
    {
        transform_t& transform = reg.get<transform_t>(entity);
        transform.local_scale = new_scale;
    }

    vec3 get_world_position(ecs::registry_t& reg, ecs::entity_t entity)
    { return translation_of(reg.get<transform_t>(entity).world_mat); }

    vec3 get_world_scale(ecs::registry_t& reg, ecs::entity_t entity)
    { return scale_of(reg.get<transform_t>(entity).world_mat); }

    quat get_world_rotation(ecs::registry_t& reg, ecs::entity_t entity)
    { return rotation_of(reg.get<transform_t>(entity).world_mat); }

    void set_world_position(ecs::registry_t& reg, ecs::entity_t entity, vec3 target_world_pos)
    {
        transform_t& t = reg.get<transform_t>(entity);

        if (t.get_parent() == ecs::NULL_ENTITY)
        {
            set_local_position(reg, entity, target_world_pos);
            return;
        }

        transform_t& parent_t = reg.get<transform_t>(t.get_parent());

        set_local_position(reg, entity, transform_point(inverse(parent_t.world_mat), target_world_pos));
    }

    void set_world_rotation(ecs::registry_t& reg, ecs::entity_t entity, quat world_rot)
    {
        transform_t& t = reg.get<transform_t>(entity);

        if (t.get_parent() == ecs::NULL_ENTITY)
        {
            t.local_rotation = normalize(world_rot);
            return;
        }

        const quat parent_inv = inverse(get_world_rotation(reg, t.get_parent()));
        t.local_rotation = normalize(parent_inv * world_rot);
    }

    void set_parent(ecs::registry_t& reg, ecs::entity_t child, ecs::entity_t parent, ecs::entity_t before)
    { internal_t::set_parent(reg, child, parent, before); }

    void set_parent_keep_world(ecs::registry_t& reg, ecs::entity_t child, ecs::entity_t parent, ecs::entity_t before)
    {
        const vec3 position = get_world_position(reg, child);
        const quat rotation = get_world_rotation(reg, child);
        const vec3 scale = get_world_scale(reg, child);

        internal_t::set_parent(reg, child, parent, before);
        if (reg.get<transform_t>(child).get_parent() != parent) { return; }

        set_world_position(reg, child, position);
        set_world_rotation(reg, child, rotation);

        // per axis, exact under a uniformly scaled parent
        const vec3 parent_scale = parent != ecs::NULL_ENTITY ? get_world_scale(reg, parent) : vec3{1.0f, 1.0f, 1.0f};
        auto ratio = [](f32 a, f32 b) { return std::fabs(b) > 1e-6f ? a / b : a; };
        reg.get<transform_t>(child).local_scale = {ratio(scale.x, parent_scale.x), ratio(scale.y, parent_scale.y),
                                                   ratio(scale.z, parent_scale.z)};
    }

    mat4 compose_world(const ecs::registry_t& reg, ecs::entity_t entity)
    {
        mat4 world;
        for (ecs::entity_t e = entity; e != ecs::NULL_ENTITY;)
        {
            const transform_t& t = reg.get<transform_t>(e);
            world = trs(t.local_position, t.local_rotation, t.local_scale) * world;
            e = t.get_parent();
        }
        return world;
    }

    ecs::entity_t first_root(const ecs::registry_t& reg)
    {
        const roots_t* roots = reg.ctx().find<roots_t>();
        return roots ? roots->first : ecs::NULL_ENTITY;
    }

    void connect(ecs::registry_t& reg)
    {
        reg.ctx().emplace<orphans_t>();
        reg.ctx().emplace<roots_t>();
        reg.ctx().emplace<order_t>();
        reg.on_construct<transform_t>().connect<&internal_t::on_construct>();
        reg.on_destroy<transform_t>().connect<&internal_t::on_destroy>();
    }

    void update(ecs::registry_t& reg) { internal_t::update(reg); }
} // namespace tau::transform_system