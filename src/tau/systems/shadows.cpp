#include "shadows.h"

#include "cglm/clipspace/ortho_lh_zo.h"
#include "cglm/clipspace/persp_lh_zo.h"
#include "cglm/clipspace/view_lh.h"
#include "cglm/vec3.h"
#include "tau/components/lighting.h"
#include "tau/components/renderer.h"
#include "tau/components/transform.h"
#include "tau/containers/flat_map.h"
#include "tau/hash.h"
#include "tau/math.h"
#include "tau/rendering/renderer.h"
#include "tau/rendering/renderer_constants.h"
#include "tau/rendering/renderer_types.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace tau::shadow_system
{
    // how far the cascades reach
    static constexpr f32 SHADOW_DISTANCE = 150.0f;
    // blend between a uniform and a logarithmic split scheme
    static constexpr f32 SPLIT_LAMBDA = 0.75f;
    // how far behind each cascade's sphere the light sits
    static constexpr f32 CASTER_EXTRUSION = 200.0f;

    // both in shadow texels
    static constexpr f32 SHADOW_NORMAL_BIAS = 2.0f;
    static constexpr f32 SHADOW_DEPTH_BIAS = 0.0f; // ndc depth, the raster slope bias covers the rest

    static constexpr f32 SDSM_RANGE_MARGIN = 0.15f;
    static constexpr f32 SDSM_RANGE_STEPS_PER_OCTAVE = 8.0f;
    static constexpr f32 SDSM_MIN_RANGE_RATIO = 1.5f;

    static f32 quantise_range(f32 value, bool round_up)
    {
        if (!(value > 0.0f)) { return value; }

        const f32 steps = std::log2(value) * SDSM_RANGE_STEPS_PER_OCTAVE;
        const f32 snapped = round_up ? std::ceil(steps) : std::floor(steps);
        return std::exp2(snapped / SDSM_RANGE_STEPS_PER_OCTAVE);
    }

    static constexpr f32 PUNCTUAL_NEAR_FRACTION = 0.02f;
    static constexpr f32 PUNCTUAL_MIN_NEAR = 0.05f;
    static constexpr f32 PUNCTUAL_NORMAL_BIAS = 1.5f; // in shadow texels, as for the cascades
    static constexpr f32 PUNCTUAL_DEPTH_BIAS = 0.0015f;

    // +x -x +y -y +z -z
    struct cube_face_t
    {
        vec3_t forward;
        vec3_t up;
    };

    static const cube_face_t CUBE_FACES[renderer::POINT_SHADOW_FACE_COUNT] = {
        {{1.0f, 0.0f, 0.0f},  {0.0f, 1.0f, 0.0f} },
        {{-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f} },
        {{0.0f, 1.0f, 0.0f},  {0.0f, 0.0f, -1.0f}},
        {{0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f} },
        {{0.0f, 0.0f, 1.0f},  {0.0f, 1.0f, 0.0f} },
        {{0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f} },
    };

    static constexpr u32_t PUNCTUAL_TILE_BUDGET = 8;
    static constexpr f32 MOVED_CASTER_SLACK = 5.0f;
    static constexpr f32 LIGHT_MOVE_EPSILON = 0.0005f;

    struct punctual_cache_entry_t
    {
        u32_t slot = 0;
        u32_t tile_count = 0;
        vec3_t position;
        vec3_t direction;
        f32 range = 0.0f;
        f32 outer_angle_rad = 0.0f;
        bool is_spot = false;
    };

    // per registry: entity ids are reused across worlds
    struct shadow_cache_t
    {
        flat_map_t<punctual_cache_entry_t> punctual;
        flat_map_t<vec3_t> caster_positions;
    };

    static shadow_cache_t& cache_of(ecs::registry_t& reg) { return reg.ctx().emplace<shadow_cache_t>(); }

    struct shadow_candidate_t
    {
        ecs::entity_t entity;
        vec3_t position;
        vec3_t direction; // spot lights only
        f32 range;
        f32 outer_angle_rad; // spot lights only
        f32 priority;        // distance from the camera to the light's sphere, nearest first
        bool is_spot;
        u32_t slot = 0;
        u32_t tile_count = 0;
        bool dirty = false;
    };

    static VkRect2D punctual_tile_rect(u32_t slot)
    {
        const u32_t dim = renderer::PUNCTUAL_SHADOW_TILE_DIM;
        const u32_t per_row = renderer::PUNCTUAL_SHADOW_TILES_PER_ROW;

        return VkRect2D{
            .offset = {static_cast<i32_t>((slot % per_row) * dim), static_cast<i32_t>((slot / per_row) * dim)},
            .extent = {dim,                                        dim                                       },
        };
    }

    static vec3_t stable_up(vec3_t forward)
    {
        vec3_t up = {0.0f, 1.0f, 0.0f};
        if (std::fabs(vec3_t::dot(forward, up)) > 0.99f) { up = {0.0f, 0.0f, 1.0f}; }
        return up;
    }

    static f32 padded_half_fov(f32 half_angle_rad)
    {
        const f32 slack = 1.0f + 2.0f / static_cast<f32>(renderer::PUNCTUAL_SHADOW_TILE_DIM);
        return std::atan(std::tan(std::fmin(half_angle_rad, glm_rad(89.0f))) * slack);
    }

    static void slice_bounding_sphere(f32 near_d, f32 far_d, f32 fov_rad, f32 aspect, f32& out_center_z,
                                      f32& out_radius)
    {
        const f32 half_tan = std::tan(fov_rad * 0.5f);
        const f32 k = half_tan * std::sqrt(1.0f + aspect * aspect);
        const f32 k2 = k * k;

        if (k2 >= (far_d - near_d) / (far_d + near_d))
        {
            // the sphere is centred on the far cap
            out_center_z = far_d;
            out_radius = far_d * k;
            return;
        }

        out_center_z = 0.5f * (far_d + near_d) * (1.0f + k2);
        const f32 span = far_d - near_d;
        const f32 sum = far_d + near_d;
        out_radius =
            0.5f * std::sqrt(span * span + 2.0f * (far_d * far_d + near_d * near_d) * k2 + sum * sum * k2 * k2);
    }

    static void update_cascades(ecs::registry_t& reg, const renderer::primary_view_info_t& view_info)
    {
        ecs::entity_t caster = ecs::NULL_ENTITY;
        for (auto [entity, light, transform] : reg.view<directional_light_t, transform_t>().each())
        {
            if (!light.casts_shadow) { continue; }
            caster = entity;
            break;
        }

        renderer::submit_shadow_cascade_light(caster);
        if (caster == ecs::NULL_ENTITY) { return; }

        vec3_t eye = view_info.position;
        vec3_t forward = view_info.forward;

        const transform_t& light_transform = reg.get<transform_t>(caster);

        vec3_t dir;
        glm_vec3_normalize_to(
            (vec3){light_transform.world_mat[2][0], light_transform.world_mat[2][1], light_transform.world_mat[2][2]},
            dir);

        vec3_t up;
        glm_vec3_normalize_to(
            (vec3){light_transform.world_mat[1][0], light_transform.world_mat[1][1], light_transform.world_mat[1][2]},
            up);
        if (std::fabs(glm_vec3_dot(dir, up)) > 0.99f)
        {
            // the light's own up is useless when it points along the light direction
            glm_vec3_copy((vec3){1.0f, 0.0f, 0.0f}, up);
            if (std::fabs(glm_vec3_dot(dir, up)) > 0.99f) { glm_vec3_copy((vec3){0.0f, 0.0f, 1.0f}, up); }
        }

        f32 near_d = view_info.z_near;
        f32 far_d = (view_info.z_far > near_d) ? std::fmin(SHADOW_DISTANCE, view_info.z_far) : SHADOW_DISTANCE;
        if (far_d <= near_d) { return; }

        {
            f32 measured_near = 0.0f;
            f32 measured_far = 0.0f;

            if (renderer::get_primary_depth_bounds(measured_near, measured_far) && measured_far > measured_near)
            {
                measured_near = quantise_range(measured_near * (1.0f - SDSM_RANGE_MARGIN), false);
                measured_far = quantise_range(measured_far * (1.0f + SDSM_RANGE_MARGIN), true);

                near_d = std::fmax(near_d, measured_near);
                far_d = std::fmin(far_d, measured_far);

                if (far_d < near_d * SDSM_MIN_RANGE_RATIO) { far_d = near_d * SDSM_MIN_RANGE_RATIO; }
                far_d = std::fmin(far_d, SHADOW_DISTANCE);

                if (far_d <= near_d) { return; }
            }
        }

        renderer::image_desc_t atlas_desc{};
        atlas_desc.width = renderer::SHADOW_ATLAS_DIM;
        atlas_desc.height = renderer::SHADOW_ATLAS_DIM;
        atlas_desc.format = renderer::ctx.swapchain.depth_format;
        atlas_desc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        atlas_desc.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        atlas_desc.depth_clear = 1.0f; // the cascades are orthographic and stay on standard 0 to 1 depth

        const f32 ratio = far_d / near_d;
        f32 split_near = near_d;

        for (u32_t i = 0; i < renderer::SHADOW_CASCADE_COUNT; i++)
        {
            const f32 t = static_cast<f32>(i + 1) / static_cast<f32>(renderer::SHADOW_CASCADE_COUNT);
            const f32 log_split = near_d * std::pow(ratio, t);
            const f32 uniform_split = near_d + (far_d - near_d) * t;
            const f32 split_far = SPLIT_LAMBDA * log_split + (1.0f - SPLIT_LAMBDA) * uniform_split;

            f32 center_z = 0.0f;
            f32 radius = 0.0f;
            slice_bounding_sphere(split_near, split_far, view_info.fov_rad, view_info.aspect, center_z, radius);
            // quantise the radius so float noise in the split maths cannot wobble the ortho extent
            radius = std::ceil(radius * 16.0f) / 16.0f;

            vec3_t center = {eye.x + forward.x * center_z, eye.y + forward.y * center_z, eye.z + forward.z * center_z};

            const f32 back_off = radius + CASTER_EXTRUSION;
            vec3_t light_eye = {center.x - dir.x * back_off, center.y - dir.y * back_off, center.z - dir.z * back_off};

            mat4_t view;
            glm_lookat_lh(light_eye, center, up, view);

            mat4_t projection;
            glm_ortho_lh_zo(-radius, radius, -radius, radius, 0.0f, back_off + radius, projection);

            // texel snap: nudge the cascade so its texel grid stays put in world space as the camera moves
            // without it every sub texel step rerasterises the silhouettes and the shadow edges crawl
            {
                mat4_t proj_copy = projection;
                mat4_t view_copy = view;
                mat4_t view_proj;
                glm_mat4_mul(proj_copy, view_copy, view_proj);

                const f32 half_dim = static_cast<f32>(renderer::SHADOW_CASCADE_TILE_DIM) * 0.5f;
                // world origin in cascade clip space is just the translation column, w is 1 for ortho
                const f32 origin_x = view_proj.m30 * half_dim;
                const f32 origin_y = view_proj.m31 * half_dim;

                projection.m30 += (std::round(origin_x) - origin_x) / half_dim;
                projection.m31 += (std::round(origin_y) - origin_y) / half_dim;
            }

            flip_clip_y(projection);

            const u32_t tile = renderer::SHADOW_CASCADE_TILE_DIM;
            VkRect2D atlas_rect = {
                .offset = {static_cast<i32_t>((i % 2) * tile), static_cast<i32_t>((i / 2) * tile)},
                .extent = {tile,                               tile                              },
            };

            renderer::submit_shadow_cascade_view("DirShadowCascade"_h + i, view, projection, "ShadowAtlas"_h,
                                                 atlas_desc, atlas_rect, split_far, SHADOW_NORMAL_BIAS,
                                                 SHADOW_DEPTH_BIAS);

            split_near = split_far;
        }
    }

    static bool sphere_in_frustum(const renderer::primary_view_info_t& view_info, vec3_t center, f32 radius)
    {
        if (!view_info.has_frustum_planes) { return true; } // no planes

        for (u32_t i = 0; i < 6; i++)
        {
            const vec4_t& plane = view_info.frustum_planes[i];
            const f32 distance = plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w;
            if (distance < -radius) { return false; }
        }

        return true;
    }

    static void cone_bounding_sphere(vec3_t apex, vec3_t direction, f32 range, f32 half_angle_rad, vec3_t& out_center,
                                     f32& out_radius)
    {
        const f32 cos_half = std::cos(half_angle_rad);

        if (half_angle_rad > glm_rad(45.0f))
        {
            out_center = apex + direction * (range * cos_half);
            out_radius = range * std::sin(half_angle_rad);
            return;
        }

        const f32 offset = range / (2.0f * cos_half * cos_half);
        out_center = apex + direction * offset;
        out_radius = offset;
    }

    struct moved_caster_t
    {
        vec3_t from;
        vec3_t to;
    };

    static void collect_moved_casters(ecs::registry_t& reg, std::vector<moved_caster_t>& out_moved)
    {
        flat_map_t<vec3_t> current;
        flat_map_t<vec3_t>& caster_positions = cache_of(reg).caster_positions;

        for (auto [entity, renderer, transform] : reg.view<mesh_renderer_t, transform_t>().each())
        {
            if (!renderer.active || !renderer.casts_shadow) { continue; }

            const vec3_t position = {transform.world_mat[3][0], transform.world_mat[3][1], transform.world_mat[3][2]};
            const u32_t key = static_cast<u32_t>(entity);
            current[key] = position;

            const vec3_t* previous = caster_positions.find(key);
            if (previous == nullptr)
            {
                out_moved.push_back({position, position});
                continue;
            }

            const vec3_t delta = position - *previous;
            if (vec3_t::dot(delta, delta) > LIGHT_MOVE_EPSILON * LIGHT_MOVE_EPSILON)
            {
                out_moved.push_back({*previous, position});
            }
        }

        for (const auto& [key, position] : caster_positions)
        {
            if (current.find(key) == nullptr) { out_moved.push_back({position, position}); }
        }

        caster_positions = std::move(current);
    }

    static bool light_volume_touched(const shadow_candidate_t& candidate, const std::vector<moved_caster_t>& moved)
    {
        const f32 reach = candidate.range + MOVED_CASTER_SLACK;
        const f32 reach_sq = reach * reach;

        for (const moved_caster_t& caster : moved)
        {
            const vec3_t from = caster.from - candidate.position;
            if (vec3_t::dot(from, from) <= reach_sq) { return true; }

            const vec3_t to = caster.to - candidate.position;
            if (vec3_t::dot(to, to) <= reach_sq) { return true; }
        }

        return false;
    }

    static void update_punctual(ecs::registry_t& reg, const renderer::primary_view_info_t& view_info)
    {
        std::vector<shadow_candidate_t> candidates;

        for (auto [entity, light, transform] : reg.view<point_light_t, transform_t>().each())
        {
            if (!light.casts_shadow || light.radius <= 0.0f) { continue; }

            shadow_candidate_t candidate{};
            candidate.entity = entity;
            candidate.position = {transform.world_mat[3][0], transform.world_mat[3][1], transform.world_mat[3][2]};
            candidate.range = light.radius;
            candidate.is_spot = false;

            if (!sphere_in_frustum(view_info, candidate.position, candidate.range)) { continue; }

            candidates.push_back(candidate);
        }

        for (auto [entity, light, transform] : reg.view<spot_light_t, transform_t>().each())
        {
            if (!light.casts_shadow || light.radius <= 0.0f) { continue; }

            shadow_candidate_t candidate{};
            candidate.entity = entity;
            candidate.position = {transform.world_mat[3][0], transform.world_mat[3][1], transform.world_mat[3][2]};
            candidate.direction =
                vec3_t::normalize({transform.world_mat[2][0], transform.world_mat[2][1], transform.world_mat[2][2]});
            candidate.range = light.radius;
            candidate.outer_angle_rad = glm_rad(light.outer_angle);
            candidate.is_spot = true;

            vec3_t cone_center;
            f32 cone_radius = 0.0f;
            cone_bounding_sphere(candidate.position, candidate.direction, candidate.range, candidate.outer_angle_rad,
                                 cone_center, cone_radius);
            if (!sphere_in_frustum(view_info, cone_center, cone_radius)) { continue; }

            candidates.push_back(candidate);
        }

        shadow_cache_t& cache = cache_of(reg);
        flat_map_t<punctual_cache_entry_t>& punctual_cache = cache.punctual;
        if (candidates.empty())
        {
            punctual_cache.clear();
            cache.caster_positions.clear();
            return;
        }

        for (shadow_candidate_t& candidate : candidates)
        {
            const vec3_t to_light = candidate.position - view_info.position;
            candidate.priority = std::sqrt(vec3_t::dot(to_light, to_light)) - candidate.range;
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const shadow_candidate_t& a, const shadow_candidate_t& b) { return a.priority < b.priority; });

        renderer::image_desc_t atlas_desc{};
        atlas_desc.width = renderer::PUNCTUAL_SHADOW_ATLAS_DIM;
        atlas_desc.height = renderer::PUNCTUAL_SHADOW_ATLAS_DIM;
        atlas_desc.format = renderer::ctx.swapchain.depth_format;
        atlas_desc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        atlas_desc.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        atlas_desc.depth_clear = 1.0f; // perspective tiles, standard 0 to 1 depth like the cascades

        std::vector<moved_caster_t> moved;
        collect_moved_casters(reg, moved);

        bool slot_taken[renderer::MAX_PUNCTUAL_SHADOW_TILES] = {};
        flat_map_t<punctual_cache_entry_t> next_cache;

        auto claim = [&slot_taken](u32_t first, u32_t count)
        {
            for (u32_t i = 0; i < count; i++) { slot_taken[first + i] = true; }
        };

        auto slots_free = [&slot_taken](u32_t first, u32_t count)
        {
            if (first + count > renderer::MAX_PUNCTUAL_SHADOW_TILES) { return false; }
            for (u32_t i = 0; i < count; i++)
            {
                if (slot_taken[first + i]) { return false; }
            }
            return true;
        };

        for (shadow_candidate_t& candidate : candidates)
        {
            candidate.tile_count = candidate.is_spot ? 1u : renderer::POINT_SHADOW_FACE_COUNT;

            const punctual_cache_entry_t* cached = punctual_cache.find(static_cast<u32_t>(candidate.entity));
            if (cached == nullptr) { continue; }
            if (cached->tile_count != candidate.tile_count) { continue; }
            if (!slots_free(cached->slot, candidate.tile_count)) { continue; }

            candidate.slot = cached->slot;
            claim(candidate.slot, candidate.tile_count);

            const vec3_t moved_by = candidate.position - cached->position;
            const vec3_t turned_by = candidate.direction - cached->direction;

            candidate.dirty = vec3_t::dot(moved_by, moved_by) > LIGHT_MOVE_EPSILON * LIGHT_MOVE_EPSILON ||
                              vec3_t::dot(turned_by, turned_by) > LIGHT_MOVE_EPSILON * LIGHT_MOVE_EPSILON ||
                              std::fabs(candidate.range - cached->range) > LIGHT_MOVE_EPSILON ||
                              std::fabs(candidate.outer_angle_rad - cached->outer_angle_rad) > LIGHT_MOVE_EPSILON ||
                              light_volume_touched(candidate, moved);

            next_cache[static_cast<u32_t>(candidate.entity)] = {
                candidate.slot,  candidate.tile_count,      candidate.position, candidate.direction,
                candidate.range, candidate.outer_angle_rad, candidate.is_spot};
        }

        for (shadow_candidate_t& candidate : candidates)
        {
            if (next_cache.contains(static_cast<u32_t>(candidate.entity))) { continue; }

            u32_t first = renderer::MAX_PUNCTUAL_SHADOW_TILES;
            for (u32_t i = 0; i + candidate.tile_count <= renderer::MAX_PUNCTUAL_SHADOW_TILES; i++)
            {
                if (slots_free(i, candidate.tile_count))
                {
                    first = i;
                    break;
                }
            }

            if (first == renderer::MAX_PUNCTUAL_SHADOW_TILES) { continue; }

            candidate.slot = first;
            candidate.dirty = true;
            claim(first, candidate.tile_count);

            next_cache[static_cast<u32_t>(candidate.entity)] = {
                candidate.slot,  candidate.tile_count,      candidate.position, candidate.direction,
                candidate.range, candidate.outer_angle_rad, candidate.is_spot};
        }

        u32_t budget_left = PUNCTUAL_TILE_BUDGET;

        for (const shadow_candidate_t& candidate : candidates)
        {
            punctual_cache_entry_t* placed = next_cache.find(static_cast<u32_t>(candidate.entity));
            if (placed == nullptr) { continue; }

            const u32_t needed = candidate.tile_count;

            if (std::fmax(candidate.range * PUNCTUAL_NEAR_FRACTION, PUNCTUAL_MIN_NEAR) >= candidate.range)
            {
                next_cache.erase(static_cast<u32_t>(candidate.entity));
                continue;
            }

            const bool first_ever = !punctual_cache.contains(static_cast<u32_t>(candidate.entity));
            bool render = candidate.dirty && (needed <= budget_left || first_ever);
            if (render) { budget_left -= std::min(needed, budget_left); }

            if (!render)
            {
                const punctual_cache_entry_t* previous = punctual_cache.find(static_cast<u32_t>(candidate.entity));
                if (previous != nullptr) { *placed = *previous; }
            }

            const punctual_cache_entry_t& tile = *placed;
            const u32_t slot = tile.slot;
            const f32 tile_near = std::fmax(tile.range * PUNCTUAL_NEAR_FRACTION, PUNCTUAL_MIN_NEAR);

            if (tile.is_spot)
            {
                const vec3_t center = tile.position + tile.direction;
                const vec3_t up = stable_up(tile.direction);

                mat4_t view;
                glm_lookat_lh(tile.position, center, up, view);

                mat4_t projection;
                glm_perspective_lh_zo(2.0f * padded_half_fov(tile.outer_angle_rad), 1.0f, tile_near, tile.range,
                                      projection);
                flip_clip_y(projection);

                renderer::submit_punctual_shadow_view("PunctualShadowTile"_h + slot, view, projection, atlas_desc,
                                                      punctual_tile_rect(slot), slot, tile.range, render,
                                                      PUNCTUAL_NORMAL_BIAS, PUNCTUAL_DEPTH_BIAS);
            }
            else
            {
                for (u32_t face = 0; face < renderer::POINT_SHADOW_FACE_COUNT; face++)
                {
                    const u32_t face_slot = slot + face;
                    const vec3_t center = tile.position + CUBE_FACES[face].forward;

                    mat4_t view;
                    glm_lookat_lh(tile.position, center, CUBE_FACES[face].up, view);

                    mat4_t projection;
                    glm_perspective_lh_zo(2.0f * padded_half_fov(glm_rad(45.0f)), 1.0f, tile_near, tile.range,
                                          projection);
                    flip_clip_y(projection);

                    renderer::submit_punctual_shadow_view(
                        "PunctualShadowTile"_h + face_slot, view, projection, atlas_desc, punctual_tile_rect(face_slot),
                        face_slot, tile.range, render, PUNCTUAL_NORMAL_BIAS, PUNCTUAL_DEPTH_BIAS);
                }
            }

            renderer::submit_punctual_shadow_slot(candidate.entity, slot);
        }

        punctual_cache = std::move(next_cache);
    }

    void update(ecs::registry_t& reg)
    {
        renderer::primary_view_info_t view_info{};
        if (!renderer::get_primary_view_info(reg, view_info)) { return; }

        update_cascades(reg, view_info);
        update_punctual(reg, view_info);
    }
} // namespace tau::shadow_system
