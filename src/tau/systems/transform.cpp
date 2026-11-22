#include "transform.h"

#include "tau/components/transform.h"
#include "tau/defines.h"
#include "tau/ecs.h"
#include "tau/ecs_fwd.h"
#include "tau/log.h"
#include "tau/math.h"
#include "tau/profiling.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace tau::transform_system
{
    namespace
    {
        constexpr u32_t NO_ROW = ~0u;

        // per registry, a parent's row before its children's (holes allowed); the hot data
        struct table_t
        {
            std::vector<ecs::entity_t> entity;
            std::vector<u32_t> parent;
            std::vector<mat4> world;
            std::vector<mat4> local;
            std::vector<u8_t> dirty;
            std::vector<ecs::entity_t> changed;
            // lowest dirty row, NO_ROW: nothing to propagate
            u32_t first_dirty = NO_ROW;

            // created or reparented since the last update, placed then
            std::vector<ecs::entity_t> pending;
            // rows of destroyed transforms
            u32_t holes = 0;

            // rebuild scratch, kept for its capacity
            std::vector<ecs::entity_t> stack;
            std::vector<ecs::entity_t> next_entity;
            std::vector<u32_t> next_parent;
            std::vector<mat4> next_world;
            std::vector<mat4> next_local;
        };

        void mark_pending(ecs::registry_t& reg, ecs::entity_t entity)
        { reg.ctx().emplace<table_t>().pending.push_back(entity); }

        void append_row(table_t& table, ecs::entity_t entity, u32_t parent, const mat4& world, const mat4& local)
        {
            table.entity.push_back(entity);
            table.parent.push_back(parent);
            table.world.push_back(world);
            table.local.push_back(local);
            table.dirty.push_back(0);
        }

        void clear_row(table_t& table, u32_t row)
        {
            table.entity[row] = ecs::NULL_ENTITY;
            table.parent[row] = NO_ROW;
            table.holes++;
        }

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

            mark_pending(reg, child);
            child_transform.is_dirty = true;
        }

        static void on_construct(ecs::registry_t& reg, ecs::entity_t entity)
        {
            transform_t& transform = reg.get<transform_t>(entity);
            transform.parent = ecs::NULL_ENTITY;
            transform.first_child = ecs::NULL_ENTITY;
            transform.next_sibling = ecs::NULL_ENTITY;
            transform.prev_sibling = ecs::NULL_ENTITY;
            transform.row = NO_ROW;
            transform.is_dirty = true;
            transform.has_shown = reg.all_of<shown_pose_t>(entity);

            link(reg, entity, transform, ecs::NULL_ENTITY, ecs::NULL_ENTITY);
            mark_pending(reg, entity);
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

            table_t* table = reg.ctx().find<table_t>();
            if (table && transform.row < table->entity.size()) { clear_row(*table, transform.row); }
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

        static void on_shown_construct(ecs::registry_t& reg, ecs::entity_t entity)
        {
            if (transform_t* transform = reg.try_get<transform_t>(entity)) { transform->has_shown = true; }
        }

        static void on_shown_destroy(ecs::registry_t& reg, ecs::entity_t entity)
        {
            if (transform_t* transform = reg.try_get<transform_t>(entity)) { transform->has_shown = false; }
        }

        // new hierarchy order; rows that existed keep their matrices, new ones are built in the compare pass
        static void rebuild_order(ecs::registry_t& reg, table_t& table)
        {
            ZoneScoped;

            auto& transforms = reg.storage<transform_t>();

            table.next_entity.clear();
            table.next_parent.clear();
            table.next_world.clear();
            table.next_local.clear();

            // headroom for the rows appended until the next re-sort, so churn does not reallocate
            const size_t capacity = transforms.size() + transforms.size() / 4 + 64;
            table.next_entity.reserve(capacity);
            table.next_parent.reserve(capacity);
            table.next_world.reserve(capacity);
            table.next_local.reserve(capacity);
            table.dirty.reserve(capacity);

            const roots_t* roots = reg.ctx().find<roots_t>();
            for (ecs::entity_t root = roots ? roots->first : ecs::NULL_ENTITY; root != ecs::NULL_ENTITY;
                 root = transforms.get(root).next_sibling)
            {
                table.stack.clear();
                table.stack.push_back(root);

                // preorder, siblings in order
                while (!table.stack.empty())
                {
                    const ecs::entity_t entity = table.stack.back();
                    table.stack.pop_back();

                    transform_t& transform = transforms.get(entity);
                    const u32_t row = static_cast<u32_t>(table.next_entity.size());
                    const u32_t old_row = transform.row;

                    table.next_entity.push_back(entity);
                    table.next_parent.push_back(
                        transform.parent != ecs::NULL_ENTITY ? transforms.get(transform.parent).row : NO_ROW);
                    if (old_row != NO_ROW)
                    {
                        table.next_world.push_back(table.world[old_row]);
                        table.next_local.push_back(table.local[old_row]);
                    }
                    else
                    {
                        table.next_world.emplace_back();
                        table.next_local.emplace_back();
                        transform.is_dirty = true;
                    }
                    transform.row = row;

                    ecs::entity_t last = transform.first_child;
                    while (last != ecs::NULL_ENTITY && transforms.get(last).next_sibling != ecs::NULL_ENTITY)
                    {
                        last = transforms.get(last).next_sibling;
                    }
                    for (ecs::entity_t child = last; child != ecs::NULL_ENTITY;
                         child = transforms.get(child).prev_sibling)
                    {
                        table.stack.push_back(child);
                    }
                }
            }

            if (table.next_entity.size() != transforms.size())
            {
                TAU_LOG_WARN("TRANSFORM", "{} transforms not reachable from a root",
                             transforms.size() - table.next_entity.size());
                for (auto [entity, transform] : transforms.each())
                {
                    if (transform.row >= table.next_entity.size() || table.next_entity[transform.row] != entity)
                    {
                        transform.row = NO_ROW;
                    }
                }
            }

            table.entity.swap(table.next_entity);
            table.parent.swap(table.next_parent);
            table.world.swap(table.next_world);
            table.local.swap(table.next_local);
            table.dirty.assign(table.entity.size(), 0);
            table.first_dirty = NO_ROW;
            table.pending.clear();
            table.holes = 0;
        }

        // the entity and everything below it to the end, the old rows become holes
        static void relocate(ecs::registry_t& reg, table_t& table, ecs::entity_t root)
        {
            auto& transforms = reg.storage<transform_t>();

            table.stack.clear();
            table.stack.push_back(root);
            while (!table.stack.empty())
            {
                const ecs::entity_t entity = table.stack.back();
                table.stack.pop_back();

                transform_t& transform = transforms.get(entity);
                const u32_t parent =
                    transform.parent != ecs::NULL_ENTITY ? transforms.get(transform.parent).row : NO_ROW;
                const u32_t old_row = transform.row;
                if (old_row != NO_ROW)
                {
                    const mat4 world = table.world[old_row];
                    const mat4 local = table.local[old_row];
                    clear_row(table, old_row);
                    append_row(table, entity, parent, world, local);
                }
                else
                {
                    append_row(table, entity, parent, mat4{}, mat4{});
                    transform.is_dirty = true;
                }
                transform.row = static_cast<u32_t>(table.entity.size() - 1);

                for (ecs::entity_t child = transform.first_child; child != ecs::NULL_ENTITY;
                     child = transforms.get(child).next_sibling)
                {
                    table.stack.push_back(child);
                }
            }
        }

        static void reserve(table_t& table, size_t capacity)
        {
            table.entity.reserve(capacity);
            table.parent.reserve(capacity);
            table.world.reserve(capacity);
            table.local.reserve(capacity);
            table.dirty.reserve(capacity);
        }

        // holes squeezed out in place, the order kept; linear over the table, then over the transforms
        static void compact(ecs::registry_t& reg, table_t& table)
        {
            ZoneScoped;

            const u32_t rows = static_cast<u32_t>(table.entity.size());
            table.next_parent.resize(rows);
            u32_t* remap = table.next_parent.data();

            u32_t kept = 0;
            for (u32_t row = 0; row < rows; row++)
            {
                if (table.entity[row] == ecs::NULL_ENTITY)
                {
                    remap[row] = NO_ROW;
                    continue;
                }
                remap[row] = kept;
                const u32_t parent = table.parent[row];
                table.entity[kept] = table.entity[row];
                table.parent[kept] = parent == NO_ROW ? NO_ROW : remap[parent];
                table.world[kept] = table.world[row];
                table.local[kept] = table.local[row];
                kept++;
            }
            table.entity.resize(kept);
            table.parent.resize(kept);
            table.world.resize(kept);
            table.local.resize(kept);
            table.dirty.resize(kept);

            for (auto [entity, transform] : reg.storage<transform_t>().each())
            {
                if (transform.row != NO_ROW) { transform.row = remap[transform.row]; }
            }
            table.holes = 0;
        }

        // keeps a parent's row before its children's
        static void place(ecs::registry_t& reg, table_t& table, ecs::entity_t entity)
        {
            auto& transforms = reg.storage<transform_t>();
            transform_t& transform = transforms.get(entity);

            if (transform.parent != ecs::NULL_ENTITY && transforms.get(transform.parent).row == NO_ROW)
            {
                // a new parent takes its whole subtree along, this one included
                place(reg, table, transform.parent);
                return;
            }

            const u32_t parent = transform.parent != ecs::NULL_ENTITY ? transforms.get(transform.parent).row : NO_ROW;
            if (transform.row != NO_ROW && (parent == NO_ROW || parent < transform.row))
            {
                table.parent[transform.row] = parent;
                return;
            }
            relocate(reg, table, entity);
        }

        static void place_pending(ecs::registry_t& reg, table_t& table)
        {
            ZoneScoped;

            for (const ecs::entity_t entity : table.pending)
            {
                if (reg.valid(entity) && reg.all_of<transform_t>(entity)) { place(reg, table, entity); }
            }
            table.pending.clear();
        }

        // locals (or the shown pose) against what the local matrix was built from, in storage order
        static void compare(ecs::registry_t& reg, table_t& table)
        {
            ZoneScoped;

            auto& shown = reg.storage<shown_pose_t>();

            for (auto [entity, transform] : reg.storage<transform_t>().each())
            {
                if (transform.row == NO_ROW) { continue; }

                const shown_pose_t* pose = transform.has_shown ? &shown.get(entity) : nullptr;
                const vec3& position = pose ? pose->position : transform.local_position;
                const quat& rotation = pose ? pose->rotation : transform.local_rotation;
                const vec3& scale = transform.local_scale;

                if (!transform.is_dirty && same_bits(position, transform.built_position) &&
                    std::memcmp(&rotation, transform.built_rotation, sizeof(quat)) == 0 &&
                    same_bits(scale, transform.built_scale))
                {
                    continue;
                }

                transform.built_position = position;
                std::memcpy(transform.built_rotation, &rotation, sizeof(quat));
                transform.built_scale = scale;
                transform.is_dirty = false;
                table.local[transform.row] = trs(position, rotation, scale);
                table.dirty[transform.row] = 1;
                table.first_dirty = std::min(table.first_dirty, transform.row);
            }
        }

        // a row is rebuilt when it or its parent changed, in hierarchy order
        static void propagate(table_t& table)
        {
            ZoneScoped;

            table.changed.clear();
            if (table.first_dirty == NO_ROW) { return; }

            const u32_t count = static_cast<u32_t>(table.entity.size());
            for (u32_t row = table.first_dirty; row < count; row++)
            {
                const u32_t parent = table.parent[row];
                if (!table.dirty[row] && (parent == NO_ROW || !table.dirty[parent])) { continue; }

                table.dirty[row] = 1;
                table.world[row] = parent == NO_ROW ? table.local[row] : table.world[parent] * table.local[row];
                table.changed.push_back(table.entity[row]);
            }

            std::fill(table.dirty.begin() + table.first_dirty, table.dirty.end(), u8_t{0});
            table.first_dirty = NO_ROW;
        }

        static void update(ecs::registry_t& reg)
        {
            ZoneScoped;

            destroy_orphans(reg);

            table_t& table = reg.ctx().emplace<table_t>();
            const size_t rows = table.entity.size();
            // bulk changes (loads): a full re-sort for locality; else compact the holes, place the rest
            if (table.pending.size() * 4 > rows) { rebuild_order(reg, table); }
            else
            {
                // compacting is linear and keeps the order; a table without many holes grows instead
                const bool full = rows + table.pending.size() > table.entity.capacity();
                if (table.holes * 4 > rows || (full && table.holes * 8 >= rows)) { compact(reg, table); }
                else if (full) { reserve(table, rows + rows / 2 + 64); }
                if (!table.pending.empty()) { place_pending(reg, table); }
            }

            compare(reg, table);
            propagate(table);
        }

        static mat4 world_matrix(const ecs::registry_t& reg, ecs::entity_t entity)
        {
            const transform_t& transform = reg.get<transform_t>(entity);
            const table_t* table = reg.ctx().find<table_t>();
            if (table && transform.row < table->world.size()) { return table->world[transform.row]; }
            return compose_world(reg, entity);
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

    mat4 world_matrix(const ecs::registry_t& reg, ecs::entity_t entity)
    { return internal_t::world_matrix(reg, entity); }

    vec3 get_world_position(const ecs::registry_t& reg, ecs::entity_t entity)
    { return translation_of(world_matrix(reg, entity)); }

    vec3 get_world_scale(const ecs::registry_t& reg, ecs::entity_t entity)
    { return scale_of(world_matrix(reg, entity)); }

    quat get_world_rotation(const ecs::registry_t& reg, ecs::entity_t entity)
    { return rotation_of(world_matrix(reg, entity)); }

    void set_world_position(ecs::registry_t& reg, ecs::entity_t entity, vec3 target_world_pos)
    {
        transform_t& t = reg.get<transform_t>(entity);

        if (t.get_parent() == ecs::NULL_ENTITY)
        {
            set_local_position(reg, entity, target_world_pos);
            return;
        }

        set_local_position(reg, entity, transform_point(inverse(world_matrix(reg, t.get_parent())), target_world_pos));
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

    std::span<const ecs::entity_t> changed(const ecs::registry_t& reg)
    {
        const table_t* table = reg.ctx().find<table_t>();
        return table ? std::span<const ecs::entity_t>(table->changed) : std::span<const ecs::entity_t>();
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
        reg.ctx().emplace<table_t>();
        reg.on_construct<transform_t>().connect<&internal_t::on_construct>();
        reg.on_destroy<transform_t>().connect<&internal_t::on_destroy>();
        reg.on_construct<shown_pose_t>().connect<&internal_t::on_shown_construct>();
        reg.on_destroy<shown_pose_t>().connect<&internal_t::on_shown_destroy>();
    }

    void update(ecs::registry_t& reg) { internal_t::update(reg); }
} // namespace tau::transform_system