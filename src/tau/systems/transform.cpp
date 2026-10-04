#include "transform.h"

#include "cglm/affine.h"
#include "cglm/mat4.h"
#include "cglm/vec3.h"
#include "tau/components/transform.h"
#include "tau/defines.h"
#include "tau/ecs.h"
#include "tau/ecs_fwd.h"
#include "tau/log.h"
#include "tau/math.h"
#include "tau/profiling.h"

#include <cstring>
#include <vector>

namespace tau::transform_system
{
    namespace
    {
        std::vector<ecs::entity_t> sort_stack;
        std::vector<ecs::entity_t> sort_res;

        bool is_hierarchy_dirty = true;

        struct orphans_t
        { std::vector<ecs::entity_t> entities; };

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

        static void set_parent(ecs::registry_t& reg, ecs::entity_t child, ecs::entity_t parent)
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

            child_transform.parent = parent;

            if (parent != ecs::NULL_ENTITY)
            {
                transform_t& parent_transform = reg.get<transform_t>(parent);

                child_transform.next_sibling = parent_transform.first_child;
                if (parent_transform.first_child != ecs::NULL_ENTITY)
                {
                    reg.get<transform_t>(parent_transform.first_child).prev_sibling = child;
                }
                parent_transform.first_child = child;
            }

            is_hierarchy_dirty = true;
            child_transform.is_dirty = true;
        }

        static void on_construct(ecs::registry_t&, ecs::entity_t) { is_hierarchy_dirty = true; }

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

            is_hierarchy_dirty = true;
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

            sort_stack.clear();
            sort_res.clear();

            for (ecs::entity_t entity : view)
            {
                if (view.get<transform_t>(entity).parent == ecs::NULL_ENTITY) { sort_stack.push_back(entity); }
            }

            // iterative dfs flatten
            while (!sort_stack.empty())
            {
                ecs::entity_t entity = sort_stack.back();
                sort_stack.pop_back();

                sort_res.push_back(entity);

                ecs::entity_t child = view.get<transform_t>(entity).first_child;
                while (child != ecs::NULL_ENTITY)
                {
                    sort_stack.push_back(child);
                    child = view.get<transform_t>(child).next_sibling;
                }
            }

            is_hierarchy_dirty = false;
        }

        static void update(ecs::registry_t& reg)
        {
            ZoneScoped;

            destroy_orphans(reg);

            if (is_hierarchy_dirty) { rebuild_hierarchy(reg); }

            auto view = reg.view<transform_t>();

            for (ecs::entity_t entity : sort_res)
            {
                transform_t& transform = view.get<transform_t>(entity);

                if (transform.is_dirty || !same_bits(transform.local_position, transform.built_position) ||
                    !same_bits(transform.local_rotation, transform.built_rotation) ||
                    !same_bits(transform.local_scale, transform.built_scale))
                {
                    mat4_t t, r, s, trs;
                    glm_translate_make(t, transform.local_position);
                    glm_quat_mat4(transform.local_rotation, r);
                    glm_scale_make(s, transform.local_scale);
                    glm_mat4_mul(r, s, trs);
                    glm_mat4_mul(t, trs, transform.local_mat);

                    transform.built_position = transform.local_position;
                    transform.built_rotation = transform.local_rotation;
                    transform.built_scale = transform.local_scale;
                    transform.is_dirty = true;
                }

                if (transform.parent != ecs::NULL_ENTITY)
                {
                    transform_t& parent_transform = view.get<transform_t>(transform.parent);

                    if (transform.is_dirty || parent_transform.is_dirty)
                    {
                        glm_mat4_mul(parent_transform.world_mat, (vec4*)transform.local_mat, transform.world_mat);
                        transform.is_dirty = true;
                    }
                }
                else if (transform.is_dirty) { glm_mat4_copy(transform.local_mat, transform.world_mat); }
            }

            for (ecs::entity_t entity : sort_res) { view.get<transform_t>(entity).is_dirty = false; }
        }
    };

    void set_local_position(ecs::registry_t& reg, ecs::entity_t entity, vec3_t new_pos)
    {
        transform_t& transform = reg.get<transform_t>(entity);
        glm_vec3_copy(new_pos, transform.local_position);
    }

    void set_local_rotation(ecs::registry_t& reg, ecs::entity_t entity, quat_t new_rot)
    {
        transform_t& transform = reg.get<transform_t>(entity);
        glm_quat_copy(new_rot, transform.local_rotation);
    }

    void set_local_scale(ecs::registry_t& reg, ecs::entity_t entity, vec3_t new_scale)
    {
        transform_t& transform = reg.get<transform_t>(entity);
        glm_vec3_copy(new_scale, transform.local_scale);
    }

    vec3_t get_world_position(ecs::registry_t& reg, ecs::entity_t entity)
    {
        transform_t& t = reg.get<transform_t>(entity);

        vec3_t pos;
        glm_vec3_copy(t.world_mat[3], pos);
        return pos;
    }

    vec3_t get_world_scale(ecs::registry_t& reg, ecs::entity_t entity)
    {
        transform_t& t = reg.get<transform_t>(entity);

        vec3_t scale;
        scale.x = glm_vec3_norm(t.world_mat[0]);
        scale.y = glm_vec3_norm(t.world_mat[1]);
        scale.z = glm_vec3_norm(t.world_mat[2]);

        return scale;
    }

    quat_t get_world_rotation(ecs::registry_t& reg, ecs::entity_t entity)
    {
        transform_t& t = reg.get<transform_t>(entity);
        quat_t rot;

        mat4 temp_mat;
        glm_mat4_copy(t.world_mat, temp_mat);

        glm_vec3_normalize(temp_mat[0]);
        glm_vec3_normalize(temp_mat[1]);
        glm_vec3_normalize(temp_mat[2]);

        glm_mat4_quat(temp_mat, rot);

        return rot;
    }

    void set_world_position(ecs::registry_t& reg, ecs::entity_t entity, vec3_t target_world_pos)
    {
        transform_t& t = reg.get<transform_t>(entity);

        if (t.get_parent() == ecs::NULL_ENTITY)
        {
            set_local_position(reg, entity, target_world_pos);
            return;
        }

        transform_t& parent_t = reg.get<transform_t>(t.get_parent());

        mat4 parent_inv;
        glm_mat4_inv(parent_t.world_mat, parent_inv);

        vec3_t new_local_pos;
        glm_mat4_mulv3(parent_inv, target_world_pos, 1.0f, new_local_pos);

        set_local_position(reg, entity, new_local_pos);
    }

    void set_parent(ecs::registry_t& reg, ecs::entity_t child, ecs::entity_t parent)
    { internal_t::set_parent(reg, child, parent); }

    void connect(ecs::registry_t& reg)
    {
        reg.ctx().emplace<orphans_t>();
        reg.on_construct<transform_t>().connect<&internal_t::on_construct>();
        reg.on_destroy<transform_t>().connect<&internal_t::on_destroy>();
    }

    void update(ecs::registry_t& reg) { internal_t::update(reg); }
} // namespace tau::transform_system